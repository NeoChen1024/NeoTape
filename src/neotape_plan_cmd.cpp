#include "neotape/closable_queue.hpp"
#include "neotape/common.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <grp.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// ====================== Planner State ============================

namespace fs = std::filesystem;
using std::format;
using std::string;
using std::string_view;
using std::vector;

struct Options {
    uint64_t slice_size = 64ULL * 1024 * 1024 * 1024;
    uint64_t metadata_buffer_size = 256ULL * 1024 * 1024;
    bool one_file_system = false;
    bool verbose = false;
    string output_path = "-";
    FILE *meta_out = nullptr;
    string chdir_dir;
    vector<fs::path> sources;
    unsigned io_threads = 1;
};

struct EntryMeta {
    fs::path source_path;
    string archive_path;
    char kind = '?';
    uint64_t disk_bytes = 0;
    uint64_t apparent_bytes = 0;
    dev_t device = 0;
    ino_t inode = 0;
    int64_t mtime = 0;
    uid_t uid = 0;
    string uname;
    gid_t gid = 0;
    string gname;
};

struct SlicePlan {
    vector<EntryMeta> entries;
    uint64_t apparent_bytes = 0;
    uint64_t allocated_bytes = 0;
};

struct ScanTotals {
    uint64_t entries = 0;
    uint64_t apparent_bytes = 0;
};

// ====================== Diagnostics & CLI ========================

[[noreturn]] void fail(const string &message) {
    std::cerr << format("neotape-plan: {}\n", message);
    std::exit(1);
}

void warn(const string &message) {
    std::cerr << format("neotape-plan: warning: {}\n", message);
}

string display_path(const fs::path &path) {
    return neotape::escape_bytes_for_diagnostic(path.string());
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} [-C <dir>] -o <file|-> [-s|--slice-size <SIZE>]\n"
        "       [-m|--metadata-buffer-size <SIZE>] [-x] [-v]\n"
        "       [-j|--io-threads <N>] <path> [path ...]\n"
        "SIZE accepts K, M, G, or T binary suffixes (for example 4M or 16G).\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"slice-size", required_argument, nullptr, 's'},
        {"metadata-buffer-size", required_argument, nullptr, 'm'},
        {"output", required_argument, nullptr, 'o'},
        {"directory", required_argument, nullptr, 'C'},
        {"io-threads", required_argument, nullptr, 'j'},
        {"one-file-system", no_argument, nullptr, 'x'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    bool saw_chdir = false;
    int c = 0;
    optind = 1;
    while ((c = getopt_long(argc, argv, "C:o:s:m:j:xvh", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 's':
            opts.slice_size = neotape::parse_size(optarg, "slice size");
            break;
        case 'm':
            opts.metadata_buffer_size =
                neotape::parse_size(optarg, "metadata buffer size");
            break;
        case 'o':
            opts.output_path = optarg;
            break;
        case 'C':
            if (saw_chdir) {
                fail("-C may be specified at most once");
            }
            saw_chdir = true;
            opts.chdir_dir = optarg;
            break;
        case 'x':
            opts.one_file_system = true;
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 'j': {
            char *end = nullptr;
            unsigned long const n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0') {
                fail("--io-threads requires a number");
            }
            opts.io_threads = static_cast<unsigned>(n);
            break;
        }
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    while (optind < argc) {
        opts.sources.emplace_back(argv[optind++]);
    }

    if (opts.output_path.empty() || opts.sources.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

// ====================== Filesystem Metadata ======================

char kind_from_mode(mode_t mode) {
    if (S_ISREG(mode)) {
        return 'f';
    }
    if (S_ISDIR(mode)) {
        return 'd';
    }
    if (S_ISLNK(mode)) {
        return 'l';
    }
    if (S_ISCHR(mode)) {
        return 'c';
    }
    if (S_ISBLK(mode)) {
        return 'b';
    }
    if (S_ISFIFO(mode)) {
        return 'p';
    }
    if (S_ISSOCK(mode)) {
        return 's';
    }
    return '?';
}

uint64_t disk_bytes_from_stat(const struct stat &st) {
    if (st.st_blocks <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_blocks) * 512;
}

uint64_t apparent_bytes_from_stat(const struct stat &st) {
    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        return st.st_size < 0 ? 0 : static_cast<uint64_t>(st.st_size);
    }
    return 0;
}

vector<fs::path> sorted_children(const fs::path &path) {
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
        warn(
            format("opendir {}: {}", display_path(path), std::strerror(errno)));
        return {};
    }

    vector<fs::path> children;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (entry == nullptr) {
            break;
        }
        string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        children.push_back(path / string(name));
    }
    if (errno != 0) {
        warn(
            format("readdir {}: {}", display_path(path), std::strerror(errno)));
    }
    if (closedir(dir) != 0) {
        warn(format("closedir {}: {}", display_path(path),
                    std::strerror(errno)));
    }

    std::ranges::sort(children);
    return children;
}

struct LstatWork {
    size_t index;
    fs::path path;
    bool one_file_system = false;
    std::optional<dev_t> root_device;
    const neotape::SourceSpec *spec = nullptr;
};

struct LstatResult {
    size_t index;
    bool ok = false;
    EntryMeta meta;
    std::exception_ptr error;
    string warning;
};

class WorkerPool {
    neotape::ClosableQueue<LstatWork> jobs_;
    neotape::ClosableQueue<LstatResult> results_;
    std::vector<std::thread> workers_;
    size_t queue_capacity_ = 0;

    static LstatResult run_lstat(const LstatWork &w) {
        try {
            struct stat st{};
            if (lstat(w.path.c_str(), &st) != 0) {
                LstatResult r{};
                r.index = w.index;
                r.warning = format("lstat {}: {}", display_path(w.path),
                                   std::strerror(errno));
                return r;
            }

            if (w.one_file_system && w.root_device.has_value() &&
                S_ISDIR(st.st_mode) && st.st_dev != *w.root_device) {
                LstatResult r{};
                r.index = w.index;
                return r;
            }

            LstatResult r{};
            r.index = w.index;
            r.ok = true;
            r.meta = EntryMeta{
                .source_path = w.path,
                .archive_path = neotape::archive_path_for_source(
                    *w.spec, w.path.generic_string()),
                .kind = kind_from_mode(st.st_mode),
                .disk_bytes = disk_bytes_from_stat(st),
                .apparent_bytes = apparent_bytes_from_stat(st),
                .device = st.st_dev,
                .inode = st.st_ino,
                .mtime = static_cast<int64_t>(st.st_mtime),
                .uid = st.st_uid,
                .uname = {},
                .gid = st.st_gid,
                .gname = {},
            };
            return r;
        } catch (...) {
            LstatResult r{};
            r.index = w.index;
            r.error = std::current_exception();
            return r;
        }
    }

    void worker_loop() {
        for (;;) {
            auto opt = jobs_.pop();
            if (!opt) {
                return;
            }
            if (!results_.push(run_lstat(*opt))) {
                return;
            }
        }
    }

  public:
    explicit WorkerPool(unsigned nworkers = 0)
        : jobs_(nworkers == 0 ? 0 : static_cast<size_t>(nworkers) * 4),
          results_(nworkers == 0 ? 0 : static_cast<size_t>(nworkers) * 4),
          queue_capacity_(nworkers == 0 ? 0
                                        : static_cast<size_t>(nworkers) * 4) {}

    ~WorkerPool() { stop(); }

    void start(unsigned nworkers) {
        if (nworkers == 0) {
            return;
        }
        for (unsigned i = 0; i < nworkers; i++) {
            workers_.emplace_back(&WorkerPool::worker_loop, this);
        }
    }

    void submit(LstatWork w) {
        if (!jobs_.push(std::move(w))) {
            throw std::runtime_error("worker pool is closed");
        }
    }

    std::vector<LstatResult> collect(const vector<fs::path> &paths,
                                     bool one_file_system,
                                     std::optional<dev_t> root_device,
                                     const neotape::SourceSpec &spec) {
        size_t count = paths.size();
        std::map<size_t, LstatResult> pending;
        size_t submitted = 0;
        size_t received = 0;
        size_t window = queue_capacity_ == 0 ? count : queue_capacity_;

        auto submit_more = [&] {
            while (submitted < count && submitted - received < window) {
                submit(LstatWork{submitted, paths[submitted], one_file_system,
                                 root_device, &spec});
                ++submitted;
            }
        };

        submit_more();
        while (received < count) {
            auto r = results_.pop();
            if (!r) {
                throw std::runtime_error(
                    "worker pool closed while waiting for lstat results");
            }
            ++received;
            if (r->error) {
                std::rethrow_exception(r->error);
            }
            if (!r->warning.empty()) {
                warn(r->warning);
            }
            pending[r->index] = std::move(*r);
            submit_more();
        }

        std::vector<LstatResult> out;
        out.reserve(count);
        for (auto &[_, r] : pending) {
            out.push_back(std::move(r));
        }
        return out;
    }

    [[nodiscard]] unsigned nworkers() const {
        return static_cast<unsigned>(workers_.size());
    }

    void stop() {
        jobs_.close();
        results_.close();
        for (auto &t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
    }
};

// ====================== Name & Inode Resolution ==================

class NameCache {
    std::unordered_map<uid_t, string> uid_to_name_;
    std::unordered_map<gid_t, string> gid_to_name_;

  public:
    string resolve_user(uid_t uid) {
        auto it = uid_to_name_.find(uid);
        if (it != uid_to_name_.end()) {
            return it->second;
        }
        string name = lookup_user(uid);
        uid_to_name_[uid] = name;
        return name;
    }

    string resolve_group(gid_t gid) {
        auto it = gid_to_name_.find(gid);
        if (it != gid_to_name_.end()) {
            return it->second;
        }
        string name = lookup_group(gid);
        gid_to_name_[gid] = name;
        return name;
    }

  private:
    static string lookup_user(uid_t uid) {
        long const bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        size_t const sz = bufsize > 0 ? static_cast<size_t>(bufsize) : 16384;
        string buf(sz, '\0');
        struct passwd pwd{};
        struct passwd *result = nullptr;
        int const rc = getpwuid_r(uid, &pwd, buf.data(), sz, &result);
        if (rc != 0 || result == nullptr) {
            return {};
        }
        return string(result->pw_name);
    }

    static string lookup_group(gid_t gid) {
        long const bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
        size_t const sz = bufsize > 0 ? static_cast<size_t>(bufsize) : 16384;
        string buf(sz, '\0');
        struct group grp{};
        struct group *result = nullptr;
        int const rc = getgrgid_r(gid, &grp, buf.data(), sz, &result);
        if (rc != 0 || result == nullptr) {
            return {};
        }
        return string(result->gr_name);
    }
};

struct InodeKey {
    dev_t dev;
    ino_t ino;
    bool operator==(const InodeKey &o) const {
        return dev == o.dev && ino == o.ino;
    }
};

struct InodeKeyHash {
    size_t operator()(const InodeKey &k) const {
        return std::hash<dev_t>{}(k.dev) ^ (std::hash<ino_t>{}(k.ino) << 1);
    }
};

class InodeTracker {
    std::unordered_set<InodeKey, InodeKeyHash> seen_;

  public:
    bool is_hardlink(dev_t dev, ino_t ino) {
        InodeKey const key{dev, ino};
        if (!seen_.insert(key).second) {
            return true; // Already seen — this is a hardlink.
        }
        return false;
    }
};

// ====================== Streaming Slice Packing ==================

void emit_slice(const SlicePlan &slice, const Options &opts, uint64_t slice_num,
                vector<uint64_t> &slice_sizes) {
    for (size_t i = 0; i < slice.entries.size(); ++i) {
        const EntryMeta &e = slice.entries[i];
        string line = format("/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", slice_num, i,
                             e.kind, e.apparent_bytes, e.mtime, e.uid, e.uname,
                             e.gid, e.gname, e.archive_path);
        fwrite(line.data(), 1, line.size(), opts.meta_out);
        fputc('\0', opts.meta_out);
        fputc('\n', opts.meta_out);
    }
    std::cerr << format("neotape-plan: slice: id={} entries={} size={}\n",
                        slice_num, slice.entries.size(),
                        neotape::humanize_number(slice.apparent_bytes));
    slice_sizes.push_back(slice.apparent_bytes);

    if (!opts.verbose) {
        return;
    }
    for (const EntryMeta &entry : slice.entries) {
        std::cerr << format(
            "neotape-plan: entry: kind={} size={} uid={} user={} gid={} "
            "group={} path={}\n",
            entry.kind, neotape::humanize_number(entry.apparent_bytes),
            entry.uid, entry.uname, entry.gid, entry.gname,
            display_path(entry.source_path));
    }
}

constexpr uint64_t entry_struct_size = sizeof(EntryMeta);

uint64_t slice_preadd_limit(const Options &opts) {
    const uint64_t half = opts.slice_size / 2;
    if (opts.slice_size > std::numeric_limits<uint64_t>::max() - half) {
        return std::numeric_limits<uint64_t>::max();
    }
    return opts.slice_size + half;
}

bool should_start_next_slice(const SlicePlan &slice, const EntryMeta &entry,
                             const Options &opts) {
    if (slice.entries.empty()) {
        return false;
    }

    const uint64_t limit = slice_preadd_limit(opts);
    return slice.apparent_bytes > limit ||
           entry.apparent_bytes > limit - slice.apparent_bytes;
}

void add_to_slice(SlicePlan &slice, const EntryMeta &entry, const Options &opts,
                  uint64_t &slice_num, ScanTotals &totals,
                  vector<uint64_t> &slice_sizes) {
    if (should_start_next_slice(slice, entry, opts)) {
        emit_slice(slice, opts, slice_num++, slice_sizes);
        slice = SlicePlan{};
    }

    slice.entries.push_back(entry);
    slice.apparent_bytes += entry.apparent_bytes;

    const auto &s = entry.source_path.native();
    uint64_t path_heap = s.size();
    if (path_heap <= 15) {
        path_heap = 0;
    }
    uint64_t ap_heap = entry.archive_path.size();
    if (ap_heap <= 15) {
        ap_heap = 0;
    }
    slice.allocated_bytes += entry_struct_size + path_heap + ap_heap;

    ++totals.entries;
    totals.apparent_bytes += entry.apparent_bytes;

    if (slice.allocated_bytes >= opts.metadata_buffer_size ||
        slice.apparent_bytes >= opts.slice_size) {
        emit_slice(slice, opts, slice_num++, slice_sizes);
        slice = SlicePlan{};
    }
}

constexpr size_t directory_frontier_capacity = 4096;

class PlannerScanner {
    struct DirectoryWork {
        fs::path path;
        const neotape::SourceSpec *spec = nullptr;
        std::optional<dev_t> root_device;
    };

    const Options &opts_;
    SlicePlan &current_slice_;
    uint64_t &slice_num_;
    ScanTotals &totals_;
    vector<uint64_t> &slice_sizes_;
    WorkerPool *pool_;
    std::deque<DirectoryWork> frontier_;
    NameCache name_cache_;
    InodeTracker inode_tracker_;

    void resolve_names(EntryMeta &meta) {
        meta.uname = name_cache_.resolve_user(meta.uid);
        meta.gname = name_cache_.resolve_group(meta.gid);
        if (inode_tracker_.is_hardlink(meta.device, meta.inode)) {
            meta.kind = 'h';
            meta.disk_bytes = 0;
            meta.apparent_bytes = 0;
        }
    }

    LstatResult stat_child(size_t index, const fs::path &path,
                           const neotape::SourceSpec &spec,
                           std::optional<dev_t> root_device) const {
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0) {
            LstatResult r{};
            r.index = index;
            r.warning = format("lstat {}: {}", display_path(path),
                               std::strerror(errno));
            return r;
        }

        if (opts_.one_file_system && root_device.has_value() &&
            S_ISDIR(st.st_mode) && st.st_dev != *root_device) {
            LstatResult r{};
            r.index = index;
            return r;
        }

        LstatResult r{};
        r.index = index;
        r.ok = true;
        r.meta = EntryMeta{
            .source_path = path,
            .archive_path =
                neotape::archive_path_for_source(spec, path.generic_string()),
            .kind = kind_from_mode(st.st_mode),
            .disk_bytes = disk_bytes_from_stat(st),
            .apparent_bytes = apparent_bytes_from_stat(st),
            .device = st.st_dev,
            .inode = st.st_ino,
            .mtime = static_cast<int64_t>(st.st_mtime),
            .uid = st.st_uid,
            .uname = {},
            .gid = st.st_gid,
            .gname = {},
        };
        return r;
    }

    vector<LstatResult> stat_children(const vector<fs::path> &children,
                                      const neotape::SourceSpec &spec,
                                      std::optional<dev_t> root_device) {
        if ((pool_ != nullptr) && pool_->nworkers() > 0) {
            return pool_->collect(children, opts_.one_file_system, root_device,
                                  spec);
        }

        vector<LstatResult> results;
        results.reserve(children.size());
        for (size_t i = 0; i < children.size(); ++i) {
            results.push_back(stat_child(i, children[i], spec, root_device));
        }
        return results;
    }

    void enqueue_directory(fs::path path, const neotape::SourceSpec &spec,
                           std::optional<dev_t> root_device) {
        while (frontier_.size() >= directory_frontier_capacity) {
            process_one_directory();
        }
        frontier_.push_back(DirectoryWork{
            .path = std::move(path),
            .spec = &spec,
            .root_device = root_device,
        });
    }

    void process_one_directory() {
        DirectoryWork const dir = std::move(frontier_.front());
        frontier_.pop_front();

        vector<fs::path> const children = sorted_children(dir.path);
        if (children.empty()) {
            return;
        }

        for (auto &r : stat_children(children, *dir.spec, dir.root_device)) {
            if (!r.warning.empty()) {
                warn(r.warning);
            }
            if (!r.ok) {
                continue;
            }

            resolve_names(r.meta);
            add_to_slice(current_slice_, r.meta, opts_, slice_num_, totals_,
                         slice_sizes_);
            if (r.meta.kind == 'd') {
                enqueue_directory(r.meta.source_path, *dir.spec,
                                  dir.root_device);
            }
        }
    }

  public:
    PlannerScanner(const Options &opts, SlicePlan &current_slice,
                   uint64_t &slice_num, ScanTotals &totals,
                   vector<uint64_t> &slice_sizes, WorkerPool *pool)
        : opts_(opts), current_slice_(current_slice), slice_num_(slice_num),
          totals_(totals), slice_sizes_(slice_sizes), pool_(pool) {}

    void scan_source(const neotape::SourceSpec &spec) {
        struct stat st{};
        if (lstat(spec.open_path.c_str(), &st) != 0) {
            fail(format("lstat {}: {}", display_path(spec.open_path),
                        std::strerror(errno)));
        }

        std::optional<dev_t> const root_device = st.st_dev;
        EntryMeta root_meta{
            .source_path = spec.open_path,
            .archive_path = neotape::archive_path_for_source(
                spec, spec.open_path.generic_string()),
            .kind = kind_from_mode(st.st_mode),
            .disk_bytes = disk_bytes_from_stat(st),
            .apparent_bytes = apparent_bytes_from_stat(st),
            .device = st.st_dev,
            .inode = st.st_ino,
            .mtime = static_cast<int64_t>(st.st_mtime),
            .uid = st.st_uid,
            .uname = {},
            .gid = st.st_gid,
            .gname = {},
        };
        resolve_names(root_meta);
        add_to_slice(current_slice_, root_meta, opts_, slice_num_, totals_,
                     slice_sizes_);

        if (!S_ISDIR(st.st_mode)) {
            return;
        }

        enqueue_directory(spec.open_path, spec, root_device);
        while (!frontier_.empty()) {
            process_one_directory();
        }
    }
};

void run_plan(Options &opts) {
    if (opts.output_path == "-") {
        opts.meta_out = stdout;
    } else {
        opts.meta_out = fopen(opts.output_path.c_str(), "wb");
        if (opts.meta_out == nullptr) {
            fail(format("open {}: {}",
                        neotape::escape_bytes_for_diagnostic(opts.output_path),
                        std::strerror(errno)));
        }
    }

    if (!opts.chdir_dir.empty()) {
        if (chdir(opts.chdir_dir.c_str()) != 0) {
            fail(format("chdir {}: {}", opts.chdir_dir, std::strerror(errno)));
        }
        string line = format("/chdir/{}", opts.chdir_dir);
        fwrite(line.data(), 1, line.size(), opts.meta_out);
        fputc('\0', opts.meta_out);
        fputc('\n', opts.meta_out);
    }

    vector<neotape::SourceSpec> sources;
    sources.reserve(opts.sources.size());
    for (const fs::path &source : opts.sources) {
        sources.push_back(neotape::make_source_spec(source.generic_string()));
    }

    vector<uint64_t> slice_sizes;
    SlicePlan current_slice;
    ScanTotals totals;
    uint64_t slice_num = 0;

    unsigned const nworkers = opts.io_threads > 0 ? opts.io_threads - 1 : 0;
    WorkerPool pool(nworkers);
    pool.start(nworkers);

    PlannerScanner scanner(opts, current_slice, slice_num, totals, slice_sizes,
                           nworkers > 0 ? &pool : nullptr);
    for (const neotape::SourceSpec &spec : sources) {
        scanner.scan_source(spec);
    }

    pool.stop();

    if (!current_slice.entries.empty()) {
        emit_slice(current_slice, opts, slice_num++, slice_sizes);
    }

    if ((opts.meta_out != nullptr) && opts.meta_out != stdout) {
        fclose(opts.meta_out);
    }

    std::cerr << format("neotape-plan: scan complete: entries={} "
                        "total_size={} target_slice={} buffer_size={}\n",
                        totals.entries,
                        neotape::humanize_number(totals.apparent_bytes),
                        neotape::humanize_number(opts.slice_size),
                        neotape::humanize_number(opts.metadata_buffer_size));

    if (!slice_sizes.empty()) {
        uint64_t min_sz = slice_sizes[0];
        uint64_t max_sz = slice_sizes[0];
        uint64_t sum_sz = 0;
        for (uint64_t const sz : slice_sizes) {
            min_sz = std::min(sz, min_sz);
            max_sz = std::max(sz, max_sz);
            sum_sz += sz;
        }
        double const avg = static_cast<double>(sum_sz) / slice_sizes.size();
        double var_sum = 0;
        for (uint64_t const sz : slice_sizes) {
            double const d = static_cast<double>(sz) - avg;
            var_sum += d * d;
        }
        double const stddev = std::sqrt(var_sum / slice_sizes.size());
        std::cerr << format(
            "neotape-plan: slice summary: slices={} min={} avg={} max={} "
            "stddev={}\n",
            slice_sizes.size(), neotape::humanize_number(min_sz),
            neotape::humanize_number(static_cast<uint64_t>(avg)),
            neotape::humanize_number(max_sz),
            neotape::humanize_number(static_cast<uint64_t>(stddev)));
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        run_plan(opts);
        return 0;

    } catch (const std::exception &e) {
        fail(e.what());
    }

    return 0;
}
