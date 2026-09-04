#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/signature.hpp"
#include "neotape/socket_util.hpp"
#include "neotape/tape.hpp"
#include "neotape/tape_ioctl.hpp"
#include "neotape/tcp_protocol.hpp"
#include "neotape/validate.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using neotape::connect_to_server;
using neotape::FdGuard;
using neotape::uint64_to_le_bytes;
using std::format;
using std::string;
using std::string_view;
using std::vector;

namespace fs = std::filesystem;

constexpr uint64_t default_recovery_bundle_block_size = 256ULL * 1024;
constexpr int exit_volume_change_required = 3;

struct TargetLocator {
    enum Kind { none, tape, spool, null_sink } kind = none;
    std::string path;
};

TargetLocator parse_target(const std::string &s) {
    if (s == "null") {
        return {TargetLocator::null_sink, {}};
    }
    if (s.starts_with("tape:")) {
        return {TargetLocator::tape, s.substr(5)};
    }
    if (s.starts_with("spool:")) {
        return {TargetLocator::spool, s.substr(6)};
    }
    throw std::runtime_error(
        "target must be tape:<device>, spool:<dir>, or null");
}

struct Options {
    string source_address;
    TargetLocator target;
    bool erase = false;
    bool append = false;
    std::optional<fs::path> recovery_bundle;
    std::optional<uint64_t> recovery_bundle_block_size;
    size_t output_buffer_size = 256ULL * 1024 * 1024;
    std::optional<uint64_t> max_volume_bytes;
    vector<string> verify_pubkey_paths;
    bool debug = false;
};

string_view target_kind_name(TargetLocator::Kind kind) {
    switch (kind) {
    case TargetLocator::tape:
        return "tape";
    case TargetLocator::spool:
        return "spool";
    case TargetLocator::null_sink:
        return "null";
    case TargetLocator::none:
        break;
    }
    return "unknown";
}

void report_volume_capacity_reached(TargetLocator::Kind kind,
                                    uint64_t committed_frames) {
    if (kind == TargetLocator::tape) {
        neotape::write_diagnostic(
            format("neotape-write: end of tape reached: committed_frames={}",
                   committed_frames));
        return;
    }
    neotape::write_diagnostic(
        format("neotape-write: volume capacity reached: target={} "
               "committed_frames={}",
               target_kind_name(kind), committed_frames));
}

[[noreturn]] void fail(const string &msg) {
    neotape::write_diagnostic(format("neotape-write: {}", msg));
    std::exit(1);
}

[[noreturn]] void usage_error(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(2);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} -s|--source <tcp://host:port|unix://path>\n"
                        "       -t|--target <tape:/dev/nst0|spool:./dir|null>\n"
                        "       [-k|--verify-pubkey <file.pub>]...\n"
                        "       [-e|--erase | -a|--append]\n"
                        "       [-R|--recovery-bundle <tar>]\n"
                        "       [-r|--recovery-bundle-block-size <SIZE>] "
                        "(default 256K)\n"
                        "       [-B|--output-buffer-size <SIZE>]\n"
                        "       [-m|--max-volume-bytes <SIZE>] [-d|--debug]\n"
                        "SIZE accepts K, M, G, or T binary suffixes "
                        "(for example 4M or 16G).\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"erase", no_argument, nullptr, 'e'},
        {"append", no_argument, nullptr, 'a'},
        {"recovery-bundle", required_argument, nullptr, 'R'},
        {"recovery-bundle-block-size", required_argument, nullptr, 'r'},
        {"output-buffer-size", required_argument, nullptr, 'B'},
        {"max-volume-bytes", required_argument, nullptr, 'm'},
        {"debug", no_argument, nullptr, 'd'},
        {"verify-pubkey", required_argument, nullptr, 'k'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c = 0;
    while ((c = getopt_long(argc, argv, "s:t:eaR:r:B:m:dk:h", long_opts,
                            nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source_address = optarg;
            break;
        case 't':
            opts.target = parse_target(optarg);
            break;
        case 'e':
            opts.erase = true;
            break;
        case 'a':
            opts.append = true;
            break;
        case 'R':
            opts.recovery_bundle = fs::path(optarg);
            break;
        case 'r': {
            try {
                opts.recovery_bundle_block_size =
                    neotape::parse_size(optarg, "recovery bundle block size");
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'B': {
            try {
                opts.output_buffer_size = static_cast<size_t>(
                    neotape::parse_size(optarg, "output buffer size"));
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'm': {
            try {
                opts.max_volume_bytes =
                    neotape::parse_size(optarg, "max volume bytes");
            } catch (const std::exception &e) {
                std::cerr << format("neotape-write: {}\n", e.what());
                std::exit(2);
            }
            break;
        }
        case 'd':
            opts.debug = true;
            break;
        case 'k':
            opts.verify_pubkey_paths.emplace_back(optarg);
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.source_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (opts.target.kind == TargetLocator::none) {
        usage_error("--target is required");
    }
    if (opts.erase && opts.append) {
        usage_error("--erase and --append are mutually exclusive");
    }
    if (opts.append && opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle cannot be used with --append");
    }
    if (opts.target.kind == TargetLocator::null_sink &&
        (opts.erase || opts.append)) {
        usage_error("--erase and --append are not valid with a null target");
    }
    if (opts.target.kind == TargetLocator::null_sink &&
        opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle is not valid with a null target");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        !opts.recovery_bundle.has_value()) {
        usage_error("--recovery-bundle-block-size requires "
                    "--recovery-bundle");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        opts.target.kind != TargetLocator::tape) {
        usage_error(
            "--recovery-bundle-block-size is only valid with tape targets");
    }
    if (opts.recovery_bundle_block_size.has_value() &&
        (*opts.recovery_bundle_block_size > neotape::max_block_size ||
         *opts.recovery_bundle_block_size < neotape::min_block_size ||
         *opts.recovery_bundle_block_size % 1024 != 0)) {
        usage_error(format(
            "--recovery-bundle-block-size must be from {} to {} bytes and a "
            "multiple of 1 KiB",
            neotape::min_block_size, neotape::max_block_size));
    }

    if (opts.recovery_bundle.has_value()) {
        std::error_code ec;
        if (!fs::is_regular_file(*opts.recovery_bundle, ec) || ec) {
            usage_error(format("recovery bundle is not a regular file: {}",
                               opts.recovery_bundle->string()));
        }
        uintmax_t const size = fs::file_size(*opts.recovery_bundle, ec);
        if (ec) {
            usage_error(format("cannot stat recovery bundle {}: {}",
                               opts.recovery_bundle->string(), ec.message()));
        }
        if (size == 0) {
            usage_error("recovery bundle must not be empty");
        }
    }

    if (opts.max_volume_bytes.has_value() &&
        opts.target.kind != TargetLocator::spool &&
        opts.target.kind != TargetLocator::null_sink) {
        usage_error(
            "--max-volume-bytes is only valid with spool or null targets");
    }

    constexpr size_t min_output_buffer_size = 8ULL * 1024 * 1024;
    if (opts.output_buffer_size < min_output_buffer_size) {
        usage_error("--output-buffer-size must be at least 8 MiB");
    }

    return opts;
}

string decode_error_payload(const vector<std::byte> &payload,
                            string_view fallback) {
    string reason;
    reason.reserve(payload.size());
    for (std::byte const b : payload) {
        reason.push_back(static_cast<char>(b));
    }
    if (reason.empty()) {
        reason = fallback;
    }
    return reason;
}

std::vector<std::byte>
auth_challenge_payload(const neotape::AuthNonceBytes &nonce) {
    std::vector<std::byte> payload(nonce.size());
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        payload[i] = static_cast<std::byte>(nonce[i]);
    }
    return payload;
}

neotape::DetachedSignatureBytes
decode_auth_response_payload(const vector<std::byte> &payload) {
    if (payload.size() != neotape::DetachedSignatureBytes{}.size()) {
        throw std::runtime_error(
            format("auth_response payload must be {} bytes",
                   neotape::DetachedSignatureBytes{}.size()));
    }

    neotape::DetachedSignatureBytes signature{};
    for (std::size_t i = 0; i < signature.size(); ++i) {
        signature[i] = static_cast<uint8_t>(payload[i]);
    }
    return signature;
}

void authenticate_source_server(int fd,
                                const vector<neotape::SignifyPublicKey> &keys) {
    if (keys.empty()) {
        return;
    }

    neotape::AuthNonceBytes const nonce = neotape::random_auth_nonce();
    neotape::tcp::write_message(
        fd, neotape::tcp::Message{neotape::tcp::MessageType::auth_challenge,
                                  auth_challenge_payload(nonce)});
    auto response = neotape::tcp::read_message(fd);
    if (!response.has_value()) {
        throw std::runtime_error("unexpected disconnect during auth handshake");
    }

    using neotape::tcp::MessageType;
    switch (response->type) {
    case MessageType::auth_response: {
        neotape::DetachedSignatureBytes const signature =
            decode_auth_response_payload(response->payload);
        bool verified = false;
        for (const auto &key : keys) {
            if (neotape::verify_auth_nonce_signature(signature, nonce, key)) {
                verified = true;
                break;
            }
        }
        if (!verified) {
            throw std::runtime_error(
                "auth_response signature verification failed");
        }
        return;
    }
    case MessageType::error:
        throw std::runtime_error(
            decode_error_payload(response->payload, "archiver auth failure"));
    default:
        throw std::runtime_error(
            format("expected auth_response, got {}",
                   neotape::tcp::message_type_name(response->type)));
    }
}

neotape::FrameSignatureStatus verify_frame_record(
    const vector<std::byte> &record, const neotape::FrameHeader &header,
    const vector<neotape::SignifyPublicKey> &keys,
    neotape::FrameValidator &validator, bool &validator_seeded) {
    const auto *data = reinterpret_cast<const uint8_t *>(record.data());
    if (!validator_seeded) {
        validator.seed_for_stream_start(header);
        validator_seeded = true;
    }
    if (auto validation_error = validator.validate(header, data, record.size());
        validation_error.has_value()) {
        throw std::runtime_error(*validation_error);
    }

    neotape::FrameSignatureValidation const signature_validation =
        neotape::validate_frame_signature(header, keys, !keys.empty());
    if (signature_validation.error.has_value()) {
        throw std::runtime_error(*signature_validation.error);
    }
    return signature_validation.status;
}

struct PendingFrame {
    uint64_t global_seq_num;
    uint64_t volume_seq_num;
    uint64_t slice_seq_num;
    vector<std::byte> record;
    bool is_filemark = false;
};

struct WriterState {
    std::deque<PendingFrame> output_queue;
    std::mutex output_mtx;
    std::condition_variable output_cv;
    std::atomic<bool> writer_stop{false};
    std::atomic<bool> writer_error{false};
    string writer_error_text;
    std::atomic<bool> has_written_frame{false};
    std::atomic<uint64_t> last_written_seq{0};
    std::atomic<uint64_t> last_written_volume{0};
    std::atomic<uint64_t> last_written_slice{0};
    std::atomic<uint64_t> written_frame_count{0};
    std::atomic<uint64_t> validated_bytes{0};
    std::atomic<uint64_t> written_bytes{0};
    std::atomic<size_t> queued_bytes{0};
    std::atomic<bool> eot_reached{false};
};

string status_count_rate(uint64_t items_per_second) {
    if (items_per_second < 1000) {
        return std::to_string(items_per_second);
    }
    if (items_per_second < 1000ULL * 1000) {
        return format("{:.1f}k",
                      static_cast<double>(items_per_second) / 1000.0);
    }
    return format("{:.1f}M",
                  static_cast<double>(items_per_second) / (1000.0 * 1000.0));
}

void print_writer_progress(uint64_t in_rate, uint64_t out_rate,
                           uint64_t frame_rate, const WriterState &state,
                           size_t output_buffer_size) {
    bool const has_frame = state.has_written_frame.load();
    string const volume =
        has_frame ? std::to_string(state.last_written_volume.load()) : "-";
    string const slice =
        has_frame ? std::to_string(state.last_written_slice.load()) : "-";
    string const frame =
        has_frame ? std::to_string(state.last_written_seq.load()) : "-";
    size_t const queued = state.queued_bytes.load();
    size_t const buffer_percent =
        output_buffer_size == 0
            ? 0
            : static_cast<size_t>(std::min<long double>(
                  100.0L, static_cast<long double>(queued) * 100.0L /
                              static_cast<long double>(output_buffer_size)));

    neotape::write_progress(
        format("in @ {:>6}/s, out @ {:>6}/s, frames @ {:>6}/s, "
               "volume {:>6}, slice {:>6}, frame {:>10}, {:>6} total, "
               "buffer {:3}% full  ",
               neotape::humanize_number(static_cast<size_t>(in_rate)),
               neotape::humanize_number(static_cast<size_t>(out_rate)),
               status_count_rate(frame_rate), volume, slice, frame,
               neotape::humanize_number(
                   static_cast<size_t>(state.written_bytes.load())),
               buffer_percent));
}

class WriterProgress {
  public:
    WriterProgress(const WriterState &state, size_t output_buffer_size)
        : state_(state), output_buffer_size_(output_buffer_size),
          thread_([this] { run(); }) {}

    ~WriterProgress() { stop(); }
    WriterProgress(const WriterProgress &) = delete;
    WriterProgress &operator=(const WriterProgress &) = delete;

    void stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            return;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        neotape::finish_progress();
    }

  private:
    void run() {
        uint64_t last_input = 0;
        uint64_t last_output = 0;
        uint64_t last_frames = 0;
        auto last_time = std::chrono::steady_clock::now();

        std::unique_lock lock(mutex_);
        while (!cv_.wait_for(lock, std::chrono::seconds(1),
                             [this] { return stopped_.load(); })) {
            auto const now = std::chrono::steady_clock::now();
            double const seconds =
                std::chrono::duration<double>(now - last_time).count();
            uint64_t const input = state_.validated_bytes.load();
            uint64_t const output = state_.written_bytes.load();
            uint64_t const frames = state_.written_frame_count.load();
            print_writer_progress(
                static_cast<uint64_t>((input - last_input) / seconds),
                static_cast<uint64_t>((output - last_output) / seconds),
                static_cast<uint64_t>((frames - last_frames) / seconds), state_,
                output_buffer_size_);
            last_input = input;
            last_output = output;
            last_frames = frames;
            last_time = now;
        }
    }

    const WriterState &state_;
    size_t output_buffer_size_;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

// Do not write a trailing filemark when EOT has been reached. On real tape
// hardware, issuing MTWEOF near the physical end of the medium can block while
// the kernel tries to flush data that cannot fit. The next volume resumes with
// the next uncommitted frame.
void write_trailing_filemark(mt::TapeDevice *dev) {
    (void)dev;
    NEOTAPE_DEBUG("neotape-write: omitting trailing filemark at EOT/EOD\n");
}

class NullTapeDevice final : public mt::TapeDevice {
  public:
    NullTapeDevice() : mt::TapeDevice(-1, "null", true) {}

    void write_record(const void *data, std::size_t size) override {
        (void)data;
        (void)size;
        ++current_record_;
    }

  protected:
    void do_mtop(int op, int count) override {
        switch (op) {
        case mt::MTWEOF:
            current_file_ += static_cast<uint64_t>(count);
            current_record_ = 0;
            return;
        case mt::MTREW:
            current_file_ = 0;
            current_record_ = 0;
            return;
        case mt::MTEOM:
            return;
        default:
            throw mt::Error(device_path(), "mtop", ENOTSUP);
        }
    }

    mt::Position do_tell() override {
        return {static_cast<long>(current_record_)};
    }

    mt::Status do_status() override {
        long flags = mt::GMT_EOD | mt::GMT_ONLINE;
        if (current_file_ == 0 && current_record_ == 0) {
            flags |= mt::GMT_BOT;
        }
        return mt::Status(0, 0, 0, flags, 0, static_cast<int>(current_file_),
                          static_cast<int>(current_record_));
    }

  private:
    uint64_t current_file_ = 0;
    uint64_t current_record_ = 0;
};

class CapacityLimitedTapeDevice : public mt::TapeDevice {
  public:
    CapacityLimitedTapeDevice(std::unique_ptr<mt::TapeDevice> inner,
                              uint64_t max_bytes, uint64_t initial_written = 0)
        : mt::TapeDevice(-1, inner->device_path(), inner->is_read_write()),
          inner_(std::move(inner)), max_bytes_(max_bytes),
          written_(initial_written) {
        if (written_ > max_bytes_) {
            throw mt::Error(device_path(), "capacity limit", ENOSPC);
        }
    }

    [[nodiscard]] int fd() const noexcept override { return inner_->fd(); }

    void write_record(const void *data, std::size_t size) override {
        if (size > max_bytes_ - written_) {
            throw mt::Error(device_path(), "capacity limit", ENOSPC);
        }
        inner_->write_record(data, size);
        written_ += size;
    }

  protected:
    void do_mtop(int op, int count) override {
        switch (op) {
        case mt::MTWEOF:
            inner_->write_filemark(count);
            return;
        case mt::MTREW:
            inner_->rewind();
            return;
        case mt::MTEOM:
            inner_->space_to_eod();
            return;
        case mt::MTFSF:
            inner_->space_fwd(count);
            return;
        case mt::MTBSF:
            inner_->space_bwd(count);
            return;
        case mt::MTFSFM:
            inner_->space_fwd_filemark(count);
            return;
        case mt::MTBSFM:
            inner_->space_bwd_filemark(count);
            return;
        case mt::MTFSR:
            inner_->space_fwd_records(count);
            return;
        case mt::MTBSR:
            inner_->space_bwd_records(count);
            return;
        case mt::MTFSS:
            inner_->space_fwd_setmarks(count);
            return;
        case mt::MTBSS:
            inner_->space_bwd_setmarks(count);
            return;
        case mt::MTSEEK:
            inner_->seek_block(count);
            return;
        case mt::MTSETBLK:
            inner_->set_block_size(count);
            return;
        case mt::MTSETDENSITY:
            inner_->set_density(count);
            return;
        case mt::MTCOMPRESSION:
            inner_->set_compression(count != 0);
            return;
        case mt::MTLOCK:
            inner_->lock();
            return;
        case mt::MTUNLOCK:
            inner_->unlock();
            return;
        case mt::MTLOAD:
            inner_->load(count);
            return;
        case mt::MTOFFL:
            inner_->offline();
            return;
        case mt::MTERASE:
            inner_->erase(count);
            return;
        default:
            throw mt::Error(device_path(), "mtop", ENOTSUP);
        }
    }

    mt::Position do_tell() override { return inner_->tell(); }
    mt::Status do_status() override { return inner_->status(); }

  private:
    std::unique_ptr<mt::TapeDevice> inner_;
    uint64_t max_bytes_;
    uint64_t written_;
};

uint64_t install_spool_recovery_bundle(const fs::path &root,
                                       const fs::path &source) {
    fs::create_directories(root);
    fs::path const destination = root / "recovery-bundle.tar";
    fs::path const pending =
        root / format("recovery-bundle.tar.pending.{}", ::getpid());
    std::error_code ec;
    fs::remove(pending, ec);
    ec.clear();
    fs::copy_file(source, pending, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error(
            format("copy recovery bundle: {}", ec.message()));
    }
    uint64_t const size = fs::file_size(pending, ec);
    if (ec) {
        fs::remove(pending);
        throw std::runtime_error(
            format("stat copied recovery bundle: {}", ec.message()));
    }
    fs::rename(pending, destination, ec);
    if (ec) {
        fs::remove(pending);
        throw std::runtime_error(
            format("install recovery bundle: {}", ec.message()));
    }
    return size;
}

void write_tape_recovery_bundle(mt::TapeDevice &dev, const fs::path &source,
                                size_t record_size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            format("cannot open recovery bundle: {}", source.string()));
    }

    vector<std::byte> record(record_size);
    uint64_t source_bytes = 0;
    uint64_t record_count = 0;
    for (;;) {
        std::fill(record.begin(), record.end(), std::byte{0});
        input.read(reinterpret_cast<char *>(record.data()),
                   static_cast<std::streamsize>(record.size()));
        std::streamsize const n = input.gcount();
        if (n == 0) {
            if (!input.eof()) {
                throw std::runtime_error("error reading recovery bundle");
            }
            break;
        }
        source_bytes += static_cast<uint64_t>(n);
        dev.write_record(record.data(), record.size());
        ++record_count;
        if (n < static_cast<std::streamsize>(record.size())) {
            if (!input.eof()) {
                throw std::runtime_error("error reading recovery bundle");
            }
            break;
        }
    }
    dev.write_filemark();
    std::cerr << format("neotape-write: wrote recovery bundle {} ({} source "
                        "bytes, {} tape records "
                        "of {} bytes)\n",
                        source.string(), source_bytes, record_count,
                        record_size);
}

void tape_writer_thread(mt::TapeDevice *dev, int fd,
                        std::mutex &socket_write_mtx, WriterState &state) {
    using neotape::tcp::Message;
    using neotape::tcp::MessageType;

    for (;;) {
        std::unique_lock lock(state.output_mtx);
        state.output_cv.wait(lock, [&] {
            return !state.output_queue.empty() || state.writer_stop.load();
        });
        if (state.output_queue.empty() && state.writer_stop.load()) {
            NEOTAPE_DEBUG("neotape-write: writer thread stopping\n");
            return;
        }
        auto frame = std::move(state.output_queue.front());
        state.output_queue.pop_front();
        state.queued_bytes.fetch_sub(frame.record.size());
        lock.unlock();

        if (frame.is_filemark) {
            NEOTAPE_DEBUG("neotape-write: writer thread writing filemark\n");
            try {
                dev->write_filemark();
            } catch (const std::exception &e) {
                state.writer_error_text = e.what();
                state.writer_error.store(true);
                return;
            }
            continue;
        }

        bool status_eot = false;
        NEOTAPE_DEBUG(
            "neotape-write: writer thread frame global_seq={} record_size={}\n",
            frame.global_seq_num, frame.record.size());
        try {
            status_eot = dev->status().eot();
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG(
                    "neotape-write: writer thread pre-write status ENOSPC, "
                    "treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
        if (status_eot) {
            NEOTAPE_DEBUG("neotape-write: writer thread pre-write status EOT, "
                          "treating as EOT\n");
            state.eot_reached.store(true);
            return;
        }

        try {
            dev->write_record(frame.record.data(), frame.record.size());
            state.written_bytes.fetch_add(frame.record.size());
            state.last_written_volume.store(frame.volume_seq_num);
            state.last_written_slice.store(frame.slice_seq_num);
            state.last_written_seq.store(frame.global_seq_num);
            state.has_written_frame.store(true);
            state.written_frame_count.fetch_add(1);
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG("neotape-write: writer thread write_record "
                              "ENOSPC, treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        } catch (const std::exception &e) {
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }

        NEOTAPE_DEBUG(
            "neotape-write: writer thread frame global_seq={} written\n",
            frame.global_seq_num);

        status_eot = false;
        try {
            status_eot = dev->status().eot();
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                NEOTAPE_DEBUG(
                    "neotape-write: writer thread post-write status ENOSPC, "
                    "treating as EOT\n");
                state.eot_reached.store(true);
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
        if (status_eot) {
            NEOTAPE_DEBUG("neotape-write: writer thread post-write status EOT, "
                          "treating as EOT\n");
            state.eot_reached.store(true);
            return;
        }

        try {
            std::scoped_lock const write_lock(socket_write_mtx);
            NEOTAPE_DEBUG(
                "neotape-write: writer thread sending ack global_seq={}\n",
                frame.global_seq_num);
            neotape::tcp::write_message(
                fd, Message{MessageType::ack_frame,
                            uint64_to_le_bytes(frame.global_seq_num)});
            NEOTAPE_DEBUG(
                "neotape-write: writer thread ack sent global_seq={}\n",
                frame.global_seq_num);
        } catch (const std::exception &e) {
            // If the archiver has already closed its end (archive complete),
            // the ACK is not needed; treat this as a clean shutdown.
            const char *what = e.what();
            if (std::strstr(what, "EPIPE") != nullptr ||
                std::strstr(what, "Broken pipe") != nullptr) {
                NEOTAPE_DEBUG("neotape-write: writer thread ack got EPIPE, "
                              "clean shutdown\n");
                return;
            }
            state.writer_error_text = e.what();
            state.writer_error.store(true);
            return;
        }
    }
}

struct WriterThreadJoiner {
    WriterState *state = nullptr;
    std::thread *thread = nullptr;
    bool joined = false;
    WriterThreadJoiner(WriterState &s, std::thread &t)
        : state(&s), thread(&t) {}
    void join() {
        if (!joined && state != nullptr && thread != nullptr) {
            state->writer_stop.store(true);
            state->output_cv.notify_all();
            if (thread->joinable()) {
                thread->join();
            }
            joined = true;
        }
    }
    ~WriterThreadJoiner() { join(); }
    WriterThreadJoiner(const WriterThreadJoiner &) = delete;
    WriterThreadJoiner &operator=(const WriterThreadJoiner &) = delete;
};

} // namespace

int main(int argc, char **argv) {
    // Acks are sent from the writer thread while the main thread reads
    // responses.  If the archiver closes the connection because the archive
    // is complete, an in-flight ACK could raise SIGPIPE; ignore it and handle
    // the resulting EPIPE as a clean shutdown.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        Options opts = parse_args(argc, argv);
        neotape::g_debug = opts.debug;
        vector<neotape::SignifyPublicKey> verify_keys;
        verify_keys.reserve(opts.verify_pubkey_paths.size());
        for (const string &path : opts.verify_pubkey_paths) {
            verify_keys.push_back(neotape::load_signify_public_key(path));
        }

        FdGuard const fd_guard(connect_to_server(opts.source_address));
        int fd = fd_guard.fd;
        authenticate_source_server(fd, verify_keys);

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        std::optional<uint32_t> volume_block_size;
        neotape::FrameValidator frame_validator;
        bool validator_seeded = false;
        bool warned_signed_unverified = false;

        // Output abstraction: a tape, spool, or validation-only null device.
        struct TargetOutput {
            std::unique_ptr<mt::TapeDevice> device;
        };
        TargetOutput output;

        {
            std::unique_ptr<mt::TapeDevice> dev;
            if (opts.target.kind == TargetLocator::tape) {
                dev = std::make_unique<mt::TapeDevice>(opts.target.path, true);
            } else if (opts.target.kind == TargetLocator::spool) {
                fs::path const spool_root(opts.target.path);
                fs::path const installed_bundle =
                    spool_root / "recovery-bundle.tar";
                if (!opts.erase && !opts.append &&
                    fs::exists(installed_bundle)) {
                    fail("spool appears to contain a recovery bundle; use "
                         "--erase or --append");
                }
                dev = std::make_unique<mt::SpoolTapeDevice>(
                    std::filesystem::path(opts.target.path), true);
            } else {
                dev = std::make_unique<NullTapeDevice>();
            }

            // Default policy: refuse to overwrite existing tape content.
            if (!opts.erase && !opts.append) {
                auto st = dev->status();
                if (!st.bot()) {
                    fail("tape is not at BOT; use --erase or --append");
                }
                // BOT alone doesn't guarantee the tape is empty.  If the
                // drive reports EOD at BOT, the tape is empty; otherwise
                // there is at least one record after BOT.
                if (!st.eod()) {
                    fail("tape appears to contain data; use --erase or "
                         "--append");
                }
            }

            if (opts.append) {
                dev->space_to_eod();
            } else {
                dev->rewind(); // --erase or empty tape
            }

            uint64_t recovery_bundle_bytes = 0;
            if (opts.target.kind == TargetLocator::tape &&
                opts.recovery_bundle.has_value()) {
                uint64_t const bundle_block_size =
                    opts.recovery_bundle_block_size.value_or(
                        default_recovery_bundle_block_size);
                write_tape_recovery_bundle(
                    *dev, *opts.recovery_bundle,
                    static_cast<size_t>(bundle_block_size));
            } else if (opts.target.kind == TargetLocator::spool) {
                fs::path const spool_root(opts.target.path);
                fs::path const installed_bundle =
                    spool_root / "recovery-bundle.tar";
                if (opts.recovery_bundle.has_value()) {
                    recovery_bundle_bytes = install_spool_recovery_bundle(
                        spool_root, *opts.recovery_bundle);
                    std::cerr << format("neotape-write: installed recovery "
                                        "bundle {} ({} bytes)\n",
                                        installed_bundle.string(),
                                        recovery_bundle_bytes);
                } else if (opts.erase) {
                    std::error_code ec;
                    fs::remove(installed_bundle, ec);
                    if (ec) {
                        fail(format("remove stale recovery bundle: {}",
                                    ec.message()));
                    }
                }
            }
            if (opts.max_volume_bytes.has_value()) {
                dev = std::make_unique<CapacityLimitedTapeDevice>(
                    std::move(dev), *opts.max_volume_bytes,
                    recovery_bundle_bytes);
            }

            output = TargetOutput{std::move(dev)};
        }

        auto write_bytes = [&](const std::vector<std::byte> &bytes) {
            output.device->write_record(bytes.data(), bytes.size());
        };

        WriterState wstate;
        std::mutex socket_write_mtx;

        std::thread writer_thread(tape_writer_thread, output.device.get(), fd,
                                  std::ref(socket_write_mtx), std::ref(wstate));
        WriterThreadJoiner joiner(wstate, writer_thread);
        WriterProgress progress(wstate, opts.output_buffer_size);

        auto joined_fail = [&](const string &msg) {
            joiner.join();
            progress.stop();
            fail(msg);
        };

        auto write_msg = [&](const Message &msg) {
            std::scoped_lock const lock(socket_write_mtx);
            neotape::tcp::write_message(fd, msg);
        };

        for (;;) {
            if (wstate.writer_error.load()) {
                joined_fail(wstate.writer_error_text);
            }

            if (wstate.eot_reached.load()) {
                NEOTAPE_DEBUG(
                    "neotape-write: eot reached, joining writer thread\n");
                joiner.join();
                write_trailing_filemark(output.device.get());
                uint64_t final_seq = wstate.last_written_seq.load();
                if (wstate.has_written_frame.load()) {
                    write_msg(Message{MessageType::ack_frame,
                                      uint64_to_le_bytes(final_seq)});
                }
                progress.stop();
                report_volume_capacity_reached(
                    opts.target.kind, wstate.written_frame_count.load());
                return exit_volume_change_required;
            }

            // Enforce output buffer limit.
            {
                std::unique_lock lock(wstate.output_mtx);
                if (wstate.queued_bytes.load() >= opts.output_buffer_size) {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }

            NEOTAPE_DEBUG("neotape-write: requesting next frame\n");
            write_msg(Message{MessageType::next_frame});
            auto msg = neotape::tcp::read_message(fd);
            if (!msg) {
                joined_fail("unexpected disconnect");
            }
            NEOTAPE_DEBUG("neotape-write: received message type={}\n",
                          static_cast<int>(msg->type));

            switch (msg->type) {
            case MessageType::frame_record: {
                neotape::FrameHeader header = neotape::parse_fixed_header(
                    reinterpret_cast<const uint8_t *>(msg->payload.data()),
                    msg->payload.size());
                uint32_t record_size = neotape::decoded_block_size(header);
                if (!volume_block_size.has_value()) {
                    volume_block_size = record_size;
                    NEOTAPE_DEBUG(
                        "neotape-write: first frame: block_size={} "
                        "archive_label=\"{}\" archive_uuid={} volume_seq={} "
                        "slice_seq={} global_seq={} channel={} "
                        "payload_size={}\n",
                        record_size, header.archive_label, header.archive_uuid,
                        header.volume_seq_num, header.slice_seq_num,
                        header.global_frame_seq_num,
                        neotape::channel_type_name(header.channel_type),
                        header.frame_payload_size);
                }
                if (msg->payload.size() != *volume_block_size) {
                    joined_fail(
                        format("frame size mismatch: expected {}, got {}",
                               *volume_block_size, msg->payload.size()));
                }
                try {
                    neotape::FrameSignatureStatus const signature_status =
                        verify_frame_record(msg->payload, header, verify_keys,
                                            frame_validator, validator_seeded);
                    if (signature_status ==
                            neotape::FrameSignatureStatus::signed_unverified &&
                        !warned_signed_unverified) {
                        neotape::write_diagnostic(
                            "neotape-write: warning: signed frames are being "
                            "written without authentication because no "
                            "public key is configured");
                        warned_signed_unverified = true;
                    }
                } catch (const std::exception &e) {
                    joined_fail(e.what());
                }
                wstate.validated_bytes.fetch_add(msg->payload.size());

                if (header.channel_type == neotape::ChannelType::ARCHIVE_END) {
                    NEOTAPE_DEBUG(
                        "neotape-write: archive_end frame, draining queue\n");
                    wstate.output_cv.notify_all();
                    for (;;) {
                        std::unique_lock lock(wstate.output_mtx);
                        if (wstate.output_queue.empty()) {
                            break;
                        }
                        lock.unlock();
                        if (wstate.writer_error.load()) {
                            joined_fail(wstate.writer_error_text);
                        }
                        if (wstate.eot_reached.load()) {
                            joiner.join();
                            write_trailing_filemark(output.device.get());
                            uint64_t final_seq = wstate.last_written_seq.load();
                            if (wstate.has_written_frame.load()) {
                                write_msg(
                                    Message{MessageType::ack_frame,
                                            uint64_to_le_bytes(final_seq)});
                            }
                            progress.stop();
                            report_volume_capacity_reached(
                                opts.target.kind,
                                wstate.written_frame_count.load());
                            return exit_volume_change_required;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                    }
                    joiner.join();
                    try {
                        write_bytes(msg->payload);
                    } catch (const mt::Error &e) {
                        if (e.error_code() != ENOSPC) {
                            throw;
                        }
                        write_trailing_filemark(output.device.get());
                        progress.stop();
                        report_volume_capacity_reached(
                            opts.target.kind,
                            wstate.written_frame_count.load());
                        return exit_volume_change_required;
                    }
                    wstate.written_bytes.fetch_add(msg->payload.size());
                    wstate.last_written_volume.store(header.volume_seq_num);
                    wstate.last_written_slice.store(header.slice_seq_num);
                    wstate.last_written_seq.store(header.global_frame_seq_num);
                    wstate.has_written_frame.store(true);
                    wstate.written_frame_count.fetch_add(1);
                    write_msg(Message{
                        MessageType::ack_frame,
                        uint64_to_le_bytes(header.global_frame_seq_num)});
                    NEOTAPE_DEBUG("neotape-write: sent ack for archive_end "
                                  "global_seq={}\n",
                                  header.global_frame_seq_num);
                    progress.stop();
                    neotape::write_diagnostic(
                        format("neotape-write: archive complete: target={} "
                               "volume_frames={} final_global_seq={}",
                               target_kind_name(opts.target.kind),
                               wstate.written_frame_count.load(),
                               header.global_frame_seq_num));
                    return 0;
                }

                uint64_t const gseq = header.global_frame_seq_num;
                std::unique_lock const lock(wstate.output_mtx);
                wstate.queued_bytes.fetch_add(msg->payload.size());
                wstate.output_queue.push_back(PendingFrame{
                    gseq, header.volume_seq_num, header.slice_seq_num,
                    std::move(msg->payload)});
                wstate.output_cv.notify_one();
                break;
            }
            case MessageType::tape_eof: {
                NEOTAPE_DEBUG("neotape-write: queuing filemark\n");
                std::unique_lock const lock(wstate.output_mtx);
                wstate.output_queue.push_back(PendingFrame{0, 0, 0, {}, true});
                wstate.output_cv.notify_one();
                break;
            }

            case MessageType::error: {
                joiner.join();
                progress.stop();
                string const reason = decode_error_payload(
                    msg->payload, "archiver reported error");
                neotape::write_diagnostic(
                    format("neotape-write: source error: {}", reason));
                std::exit(1);
            }
            case MessageType::auth_challenge:
            case MessageType::auth_response:
                joined_fail(format("unexpected message type {}",
                                   static_cast<int>(msg->type)));
                break;
            default:
                joined_fail(format("unexpected message type {}",
                                   static_cast<int>(msg->type)));
            }
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
