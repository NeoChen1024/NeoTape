// NeoTape Frame EOT probe utility.
//
// Purpose
// -------
// This standalone experimental tool validates how a real tape backend behaves
// near EOT/EOM when writing complete NeoTape-sized Frame records.
//
// It is NOT a stable NeoTape archival writer. It follows the current
// docs/spec/03-frame-header.md field order closely enough for backend behavior
// testing, but it intentionally uses a local probe digest instead of BLAKE3 so
// it has no external library dependency.
//
// Build:
//   c++ -std=c++20 -O2 -Wall -Wextra -pedantic testing/tape_eot_frame_probe.cpp -o tape_eot_frame_probe
//
// Safer file test:
//   ./tape_eot_frame_probe write --path /tmp/probe.ntframes --block-size 1048576 --max-frames 8 --yes-write --log write.jsonl
//   ./tape_eot_frame_probe read  --path /tmp/probe.ntframes --block-size 1048576 --max-frames 8 --log read.jsonl
//
// Tape test outline, after preparing a deliberately small partition:
//   mt -f /dev/nst0 rewind
//   ./tape_eot_frame_probe write --path /dev/nst0 --block-size 1048576 \
//       --set-fixed-block --allow-character-device --yes-write --log eot-write.jsonl
//   mt -f /dev/nst0 rewind
//   ./tape_eot_frame_probe read --path /dev/nst0 --block-size 1048576 \
//       --allow-character-device --log eot-read.jsonl
//
// Safety:
//   - write mode requires --yes-write.
//   - writing to character devices also requires --allow-character-device.
//   - this tool does not rewind automatically; use mt explicitly so positioning
//     is visible in shell history/logs.
//   - JSONL logs include read/write return values, errno text, and MTIOCGET
//     status when available.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if __has_include(<sys/mtio.h>)
#include <sys/mtio.h>
#define NT_HAVE_MTIO 1
#else
#define NT_HAVE_MTIO 0
#endif

namespace nt_probe {

constexpr std::size_t HEADER_SIZE = 1024;
constexpr std::uint8_t HEADER_VERSION = 1;
constexpr std::uint8_t HEADER_TYPE_FRAME = 3;
constexpr std::uint8_t PAYLOAD_PROFILE_PROBE = 255;
constexpr std::uint8_t CONTENT_SLICE_CONTENT = 1;
constexpr std::uint16_t FLAG_START = 1u << 0;
constexpr char MAGIC[8] = {'N','e','o','T','a','p','e','\0'};

struct Options {
    enum class Mode { write, read } mode = Mode::write;
    std::string path;
    std::string log_path = "-";
    std::uint32_t block_size = 1024 * 1024;
    std::uint64_t max_frames = 0;
    std::uint64_t seed = 0x4e656f5461706545ULL;
    std::string archive_uuid = "00000000-0000-4000-8000-000000000001";
    std::string archive_name = "NeoTape EOT Frame Probe";
    bool yes_write = false;
    bool allow_character_device = false;
    bool set_fixed_block = false;
    bool write_filemark = false;
    bool mt_nop_after_write = true;
    std::uint64_t continue_after_enospc = 32;
    std::uint64_t stop_after_zero_reads = 2;
    std::uint64_t status_every = 128;
};

[[noreturn]] void die(const std::string& s) { throw std::runtime_error(s); }

std::uint64_t now_us() {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now().time_since_epoch()).count());
}

std::string esc(std::string_view s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '"') r += "\\\"";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) {
            std::ostringstream os;
            os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned>(c);
            r += os.str();
        } else {
            r.push_back(static_cast<char>(c));
        }
    }
    return r;
}

struct Log {
    std::ofstream file;
    std::ostream* out = nullptr;

    explicit Log(const std::string& path) {
        if (path == "-") {
            out = &std::cout;
        } else {
            file.open(path, std::ios::out | std::ios::trunc);
            if (!file) die("cannot open log: " + path);
            out = &file;
        }
    }

    struct Event {
        Log& l;
        bool first = true;

        explicit Event(Log& log) : l(log) {
            *l.out << '{';
            u64("t_us", now_us());
        }

        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;

        ~Event() {
            *l.out << "}\n";
            l.out->flush();
        }

        void comma() {
            if (!first) *l.out << ',';
            first = false;
        }

        void str(std::string_view k, std::string_view v) {
            comma();
            *l.out << '"' << esc(k) << "\":\"" << esc(v) << '"';
        }

        void u64(std::string_view k, std::uint64_t v) {
            comma();
            *l.out << '"' << esc(k) << "\":" << v;
        }

        void i64(std::string_view k, std::int64_t v) {
            comma();
            *l.out << '"' << esc(k) << "\":" << v;
        }

        void boolean(std::string_view k, bool v) {
            comma();
            *l.out << '"' << esc(k) << "\":" << (v ? "true" : "false");
        }
    };

    Event event() { return Event(*this); }
};

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

void fill_payload(std::uint8_t* dst, std::size_t n, std::uint64_t seed, std::uint64_t frame) {
    std::uint64_t s = seed ^ (frame * 0x9e3779b97f4a7c15ULL) ^ 0x4652414d45505242ULL;
    for (std::size_t off = 0; off < n;) {
        std::uint64_t x = splitmix64(s++);
        for (int i = 0; i < 8 && off < n; ++i, ++off) {
            dst[off] = static_cast<std::uint8_t>(x >> (8 * i));
        }
    }
}

std::array<std::uint8_t, 32> probe_digest(const std::uint8_t* p, std::size_t n) {
    std::array<std::uint64_t, 4> h{
        0x243f6a8885a308d3ULL,
        0x13198a2e03707344ULL,
        0xa4093822299f31d0ULL,
        0x082efa98ec4e6c89ULL
    };
    for (std::size_t i = 0; i < n; ++i) {
        h[i & 3] = splitmix64(h[i & 3] ^ static_cast<std::uint64_t>(p[i]) ^ i);
    }
    std::array<std::uint8_t, 32> out{};
    for (int lane = 0; lane < 4; ++lane) {
        std::uint64_t v = splitmix64(h[lane]);
        for (int i = 0; i < 8; ++i) {
            out[lane * 8 + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    return out;
}

std::uint32_t crc32c(const std::uint8_t* p, std::size_t n) {
    static std::array<std::uint32_t, 256> table{};
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0x82F63B78U ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }

    std::uint32_t c = 0xffffffffU;
    for (std::size_t i = 0; i < n; ++i) {
        c = table[(c ^ p[i]) & 0xffU] ^ (c >> 8);
    }
    return c ^ 0xffffffffU;
}

void le16(std::vector<std::uint8_t>& b, std::size_t& o, std::uint16_t v) {
    b.at(o++) = static_cast<std::uint8_t>(v);
    b.at(o++) = static_cast<std::uint8_t>(v >> 8);
}

void le32(std::vector<std::uint8_t>& b, std::size_t& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.at(o++) = static_cast<std::uint8_t>(v >> (8 * i));
}

void le64(std::vector<std::uint8_t>& b, std::size_t& o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.at(o++) = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t o) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b.at(o + i)) << (8 * i);
    return v;
}

std::uint64_t rd64(const std::vector<std::uint8_t>& b, std::size_t o) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b.at(o + i)) << (8 * i);
    return v;
}

void fixed_str(std::vector<std::uint8_t>& b, std::size_t& o, const std::string& s, std::size_t n) {
    const std::size_t m = std::min(s.size(), n ? n - 1 : 0);
    std::memcpy(b.data() + o, s.data(), m);
    o += n;
}

std::vector<std::uint8_t> make_frame(const Options& opt, std::uint64_t frame) {
    if (opt.block_size < 65536 || opt.block_size <= HEADER_SIZE) die("invalid block size");

    std::vector<std::uint8_t> b(opt.block_size, 0);
    const std::size_t payload_size = opt.block_size - HEADER_SIZE;

    fill_payload(b.data() + HEADER_SIZE, payload_size, opt.seed, frame);
    const auto dg = probe_digest(b.data() + HEADER_SIZE, payload_size);

    std::size_t o = 0;
    std::memcpy(b.data() + o, MAGIC, 8); o += 8;
    b.at(o++) = HEADER_VERSION;
    b.at(o++) = HEADER_TYPE_FRAME;
    le32(b, o, opt.block_size);
    fixed_str(b, o, opt.archive_uuid, 37);
    fixed_str(b, o, opt.archive_name, 256);
    le64(b, o, 1);                       // volume_seq_num
    b.at(o++) = PAYLOAD_PROFILE_PROBE;
    le64(b, o, 1);                       // logical_slice_seq_num
    le64(b, o, frame);                   // global_frame_seq_num
    le64(b, o, frame);                   // frame_seq_num_within_slice
    le64(b, o, payload_size);
    b.at(o++) = CONTENT_SLICE_CONTENT;
    std::memcpy(b.data() + o, dg.data(), dg.size()); o += dg.size();
    le16(b, o, frame == 0 ? FLAG_START : 0);
    le64(b, o, 0);                       // slice_content_size: no END in fill-until-EOT probe
    o += 32;                             // slice_content_blake3 zero

    if (o > HEADER_SIZE - 4) die("header layout overflow");
    const std::uint32_t crc = crc32c(b.data(), HEADER_SIZE - 4);
    std::size_t co = HEADER_SIZE - 4;
    le32(b, co, crc);

    return b;
}

struct Parsed {
    std::uint32_t block_size = 0;
    std::uint64_t frame = 0;
    std::uint64_t payload_size = 0;
    std::array<std::uint8_t, 32> digest{};
};

Parsed parse_header(const std::vector<std::uint8_t>& b) {
    if (b.size() < HEADER_SIZE) die("short header");
    if (std::memcmp(b.data(), MAGIC, 8) != 0) die("bad magic");
    if (b.at(8) != HEADER_VERSION) die("bad version");
    if (b.at(9) != HEADER_TYPE_FRAME) die("bad header type");

    const std::uint32_t stored = rd32(b, HEADER_SIZE - 4);
    const std::uint32_t actual = crc32c(b.data(), HEADER_SIZE - 4);
    if (stored != actual) die("bad header crc32c");

    Parsed p{};
    std::size_t o = 10;
    p.block_size = rd32(b, o); o += 4;
    o += 37 + 256 + 8 + 1 + 8;           // uuid, name, volume seq, profile, slice seq
    p.frame = rd64(b, o); o += 8;
    o += 8;                              // frame_seq_num_within_slice
    p.payload_size = rd64(b, o); o += 8;

    const std::uint8_t content_type = b.at(o++);
    if (content_type != CONTENT_SLICE_CONTENT) die("unexpected content type");

    std::memcpy(p.digest.data(), b.data() + o, 32);
    return p;
}

#if NT_HAVE_MTIO
void mt_op(int fd, short op, int count) {
    mtop m{};
    m.mt_op = op;
    m.mt_count = count;
    if (::ioctl(fd, MTIOCTOP, &m) != 0) {
        die(std::string("MTIOCTOP failed: ") + std::strerror(errno));
    }
}

void log_mt_status(Log& log, int fd, std::string_view label) {
    mtget g{};
    int rc = ::ioctl(fd, MTIOCGET, &g);

    auto ev = log.event();
    ev.str("event", "mt_status");
    ev.str("label", label);
    ev.i64("rc", rc);

    if (rc == 0) {
        ev.i64("mt_resid", g.mt_resid);
        ev.i64("mt_dsreg", g.mt_dsreg);
        ev.i64("mt_gstat", g.mt_gstat);
        ev.i64("mt_erreg", g.mt_erreg);
        ev.i64("mt_fileno", g.mt_fileno);
        ev.i64("mt_blkno", g.mt_blkno);
    } else {
        ev.i64("errno", errno);
        ev.str("errno_text", std::strerror(errno));
    }
}
#else
void log_mt_status(Log& log, int, std::string_view label) {
    auto ev = log.event();
    ev.str("event", "mt_status_unavailable");
    ev.str("label", label);
}
#endif

int open_checked(const Options& opt, int flags) {
    struct stat st{};
    const bool writing = (flags & O_WRONLY) || (flags & O_RDWR);

    if (::stat(opt.path.c_str(), &st) == 0) {
        if (writing && S_ISCHR(st.st_mode) && !opt.allow_character_device) {
            die("refusing character device without --allow-character-device");
        }
    } else if (!writing) {
        die(std::string("stat failed: ") + std::strerror(errno));
    }

    int fd = ::open(opt.path.c_str(), flags, 0666);
    if (fd < 0) die(std::string("open failed: ") + std::strerror(errno));
    return fd;
}

void run_write(const Options& opt, Log& log) {
    if (!opt.yes_write) die("refusing write without --yes-write");

    int fd = open_checked(opt, O_WRONLY | O_CREAT);

#if NT_HAVE_MTIO
    if (opt.set_fixed_block) {
        mt_op(fd, MTSETBLK, static_cast<int>(opt.block_size));
        log_mt_status(log, fd, "after_set_fixed_block");
    }
#endif

    {
        auto ev = log.event();
        ev.str("event", "write_start");
        ev.str("path", opt.path);
        ev.u64("block_size", opt.block_size);
        ev.u64("seed", opt.seed);
        ev.u64("max_frames", opt.max_frames);
    }

    std::uint64_t frame = 0;
    std::uint64_t enospc = 0;

    while (opt.max_frames == 0 || frame < opt.max_frames) {
        auto b = make_frame(opt, frame);

        const auto t0 = now_us();
        errno = 0;
        const ssize_t n = ::write(fd, b.data(), b.size());
        const int e = errno;
        const auto t1 = now_us();

        {
            auto ev = log.event();
            ev.str("event", "write");
            ev.u64("frame", frame);
            ev.i64("ret", n);
            ev.i64("errno", n < 0 ? e : 0);
            ev.str("errno_text", n < 0 ? std::strerror(e) : "");
            ev.u64("duration_us", t1 - t0);
            ev.boolean("complete", n == static_cast<ssize_t>(b.size()));
        }

        if (opt.status_every != 0 && frame % opt.status_every == 0) {
            log_mt_status(log, fd, "periodic_write_status");
        }

        if (n == static_cast<ssize_t>(b.size())) {
            ++frame;
            continue;
        }

        log_mt_status(log, fd, n < 0 ? "write_error" : "short_write");

        if (n < 0 && e == ENOSPC && enospc++ < opt.continue_after_enospc) {
            ++frame; // make later attempts distinguishable if any succeed
            continue;
        }

        break;
    }

#if NT_HAVE_MTIO
    if (opt.mt_nop_after_write) {
        try {
            mt_op(fd, MTNOP, 1);
            log_mt_status(log, fd, "after_mtnop");
        } catch (const std::exception& ex) {
            auto ev = log.event();
            ev.str("event", "mtnop_error");
            ev.str("error", ex.what());
        }
    }

    if (opt.write_filemark) {
        try {
            mt_op(fd, MTWEOF, 1);
            log_mt_status(log, fd, "after_mtweof");
        } catch (const std::exception& ex) {
            auto ev = log.event();
            ev.str("event", "mtweof_error");
            ev.str("error", ex.what());
        }
    }
#endif

    ::close(fd);

    auto ev = log.event();
    ev.str("event", "write_done");
    ev.u64("next_frame", frame);
    ev.u64("enospc_events", enospc);
}

void run_read(const Options& opt, Log& log) {
    int fd = open_checked(opt, O_RDONLY);
    std::vector<std::uint8_t> b(opt.block_size);

    {
        auto ev = log.event();
        ev.str("event", "read_start");
        ev.str("path", opt.path);
        ev.u64("block_size", opt.block_size);
        ev.u64("seed", opt.seed);
        ev.u64("max_frames", opt.max_frames);
    }

    std::uint64_t idx = 0;
    std::uint64_t zeros = 0;

    while (opt.max_frames == 0 || idx < opt.max_frames) {
        const auto t0 = now_us();
        errno = 0;
        const ssize_t n = ::read(fd, b.data(), b.size());
        const int e = errno;
        const auto t1 = now_us();

        auto ev = log.event();
        ev.str("event", "read");
        ev.u64("index", idx);
        ev.i64("ret", n);
        ev.i64("errno", n < 0 ? e : 0);
        ev.str("errno_text", n < 0 ? std::strerror(e) : "");
        ev.u64("duration_us", t1 - t0);

        if (n == 0) {
            ev.str("result", "zero");
            log_mt_status(log, fd, "read_zero");
            if (++zeros >= opt.stop_after_zero_reads) break;
            continue;
        }

        zeros = 0;

        if (n != static_cast<ssize_t>(opt.block_size)) {
            ev.str("result", "short");
            log_mt_status(log, fd, "short_read");
            break;
        }

        try {
            Parsed p = parse_header(b);
            bool ok = p.block_size == opt.block_size &&
                      p.payload_size <= opt.block_size - HEADER_SIZE;

            const auto dg = probe_digest(b.data() + HEADER_SIZE, static_cast<std::size_t>(p.payload_size));
            ok = ok && (dg == p.digest);

            std::vector<std::uint8_t> expected(static_cast<std::size_t>(p.payload_size));
            fill_payload(expected.data(), expected.size(), opt.seed, p.frame);
            ok = ok && std::equal(expected.begin(), expected.end(), b.begin() + HEADER_SIZE);

            ev.str("result", ok ? "valid" : "invalid");
            ev.u64("frame", p.frame);
            ev.u64("payload_size", p.payload_size);
        } catch (const std::exception& ex) {
            ev.str("result", "invalid_header");
            ev.str("error", ex.what());
            log_mt_status(log, fd, "invalid_header");
            break;
        }

        ++idx;
    }

    ::close(fd);

    auto ev = log.event();
    ev.str("event", "read_done");
    ev.u64("frames_read", idx);
}

std::uint64_t parse_u64(const std::string& s) {
    std::size_t p = 0;
    auto v = std::stoull(s, &p, 0);
    if (p != s.size()) die("bad integer: " + s);
    return v;
}

void usage() {
    std::cerr <<
R"(usage:
  tape_eot_frame_probe write --path PATH [options] --yes-write
  tape_eot_frame_probe read  --path PATH [options]

common options:
  --path PATH
  --log PATH                       JSONL log path, default: stdout
  --block-size BYTES               default: 1048576
  --max-frames N                   default: 0; write until error, read until EOF/zero/error
  --seed N                         deterministic payload seed
  --archive-uuid UUID              default test UUID
  --archive-name NAME              default test name

write-only options:
  --yes-write
  --allow-character-device
  --set-fixed-block                Linux MTSETBLK when sys/mtio.h is available
  --write-filemark                 Linux MTWEOF after write loop
  --no-mtnop-after-write
  --continue-after-enospc N        default: 32
  --status-every N                 default: 128

read-only options:
  --stop-after-zero-reads N        default: 2
)";
}

Options parse(int argc, char** argv) {
    if (argc < 3) {
        usage();
        die("missing arguments");
    }

    Options o;
    std::string mode = argv[1];

    if (mode == "write") o.mode = Options::Mode::write;
    else if (mode == "read") o.mode = Options::Mode::read;
    else {
        usage();
        die("bad mode: " + mode);
    }

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];

        auto val = [&](const char* n) {
            if (i + 1 >= argc) die(std::string("missing value for ") + n);
            return std::string(argv[++i]);
        };

        if (a == "--path") o.path = val("--path");
        else if (a == "--log") o.log_path = val("--log");
        else if (a == "--block-size") o.block_size = static_cast<std::uint32_t>(parse_u64(val("--block-size")));
        else if (a == "--max-frames") o.max_frames = parse_u64(val("--max-frames"));
        else if (a == "--seed") o.seed = parse_u64(val("--seed"));
        else if (a == "--archive-uuid") o.archive_uuid = val("--archive-uuid");
        else if (a == "--archive-name") o.archive_name = val("--archive-name");
        else if (a == "--yes-write") o.yes_write = true;
        else if (a == "--allow-character-device") o.allow_character_device = true;
        else if (a == "--set-fixed-block") o.set_fixed_block = true;
        else if (a == "--write-filemark") o.write_filemark = true;
        else if (a == "--no-mtnop-after-write") o.mt_nop_after_write = false;
        else if (a == "--continue-after-enospc") o.continue_after_enospc = parse_u64(val("--continue-after-enospc"));
        else if (a == "--stop-after-zero-reads") o.stop_after_zero_reads = parse_u64(val("--stop-after-zero-reads"));
        else if (a == "--status-every") o.status_every = parse_u64(val("--status-every"));
        else {
            usage();
            die("unknown option: " + a);
        }
    }

    if (o.path.empty()) die("--path is required");
    if (o.block_size < 65536) die("--block-size must be >= 65536");
    if (o.archive_uuid.size() > 36) die("archive UUID too long");
    if (o.archive_name.size() > 255) die("archive name too long");

    return o;
}

} // namespace nt_probe

int main(int argc, char** argv) {
    try {
        auto opt = nt_probe::parse(argc, argv);
        nt_probe::Log log(opt.log_path);

        if (opt.mode == nt_probe::Options::Mode::write) {
            nt_probe::run_write(opt, log);
        } else {
            nt_probe::run_read(opt, log);
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << "\n";
        return 1;
    }
}
