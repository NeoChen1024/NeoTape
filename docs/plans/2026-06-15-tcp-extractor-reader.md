# TCP Extractor / Reader Implementation Plan

> **REQUIRED SUB-SKILL:** Use the executing-plans skill to implement this plan
> task-by-task.

**Goal:** Build `neotape-extractor` (long-running TCP server) and rewrite
`neotape-read` (per-volume TCP client) as a symmetric complement to the
existing `neotape-archiver` / `neotape-write` pair.

**Architecture:** The extractor is a server that pulls frames from a reader
via pull-based TCP protocol, validates archive continuity (UUID, label, all
sequence numbers, frame hash), reassembles `ch_content` payloads per slice,
and writes the reconstituted pax stream to stdout or a file.  The reader is a
short-lived per-volume client that reads raw NeoTape records from a tape
device or spool directory and forwards them to the extractor on demand.

**Tech Stack:** C++20, same `neotape::tcp` protocol, same
`neotape::format` parser, `getopt_long` CLI.

---

## Background

### Existing archiver pipeline (writing direction, for reference)

```
neotape-archiver --listen tcp://0.0.0.0:9000 -C /data files...
neotape-write    --source tcp://host:9000 --target tape:/dev/nst0
```

Protocol flow (client-pull):

```
Writer                        Archiver (server)
  next_frame -->                                   // writer asks for frame
               <--  frame_record                  // archiver sends frame bytes
  ack_frame   -->                                   // writer confirms receipt
               <--  tape_eof                      // end of logical slice
  next_frame -->                                   // next slice
  ...
               <--  frame_record(archive_end)
  ack_frame   -->
  disconnect <<==   (writer reaches EOT / tape full)
```

### New pipeline (reading direction)

```
neotape-extractor --listen tcp://0.0.0.0:9000 -o output.pax
neotape-read      --source tape:/dev/nst0 --connect tcp://host:9000
```

Protocol flow (extractor-pull; same message types, reversed direction):

```
Reader (client)               Extractor (server)
               <--  next_frame                    // extractor asks for frame
  frame_record -->                                  // reader reads tape, sends frame
               <--  ack_frame(N)                  // extractor validates, acks
               <--  next_frame                    // next frame
  ...
  tape_eof     -->                                  // reader hit filemark / EOT
  disconnect <<==   (reader done with this volume)
```

### Message types (shared, Server/Client naming)

| ID | Name | Sent by | Purpose |
|---|---|---|---|
| 0x01 | `next_frame` | **Server** | Request one frame record. In archiver mode Server=Archiver (archiver doesn't send this, writer does — archiver is the data source). |
| 0x02 | `frame_record` | **Client** | A NeoTape record (header + payload + padding). |
| 0x03 | `tape_eof` | **Client** | No more frames on this volume (filemark / EOT hit). |
| 0x04 | `error` | either | Error message text in payload. |
| 0x05 | `ack_frame` | **Server** | Confirmation; payload = `uint64_t` little-endian global frame seq num of the last validated frame. |

**Rationale:** The extractor is the validation authority, so it must see every
frame before acknowledging it.  The reader is a simple I/O proxy: read a
tape record, send bytes, wait for next request.  This is the same pull-based
pattern as the archiver pipeline, just with roles swapped.

---

## Task 1: Protocol documentation — Server/Client naming

**Files:**
- Modify: `include/neotape/tcp_protocol.hpp` (comments only)
- Create: `docs/spec/13-tcp-protocol.md`

**Goal:** Document message types with Server/Client roles so both archiver and
extractor pipelines share the same unambiguous protocol spec.

**Step 1: Update `tcp_protocol.hpp` comments**

Replace the single `ack_frame` comment:

```cpp
// ack_frame payload: uint64_t little-endian global frame sequence number.
```

with a role table and per-message docs:

```cpp
//
// Message roles (Server ↔ Client):
//
//   next_frame   — Server requests next frame          (dir: Server → Client)
//   frame_record — Client sends frame bytes            (dir: Client → Server)
//   tape_eof     — Client signals no more frames       (dir: Client → Server)
//   error        — Either side reports an error        (dir: bidirectional)
//   ack_frame    — Server confirms frame was validated  (dir: Server → Client)
//                  payload: uint64_t little-endian global frame seq num
//
// In the archiver pipeline Server = Archiver, Client = Writer.
// In the extractor pipeline Server = Extractor, Client = Reader.
```

**Step 2: Write `docs/spec/13-tcp-protocol.md`**

```markdown
# TCP Protocol

Status: normative.

## Overview

NeoTape separates long-running archive servers from short-lived per-volume
clients over a single TCP or Unix-domain socket.  The same protocol serves
both the writing pipeline (archiver ↔ writer) and the reading pipeline
(extractor ↔ reader).

## Roles

| Role | Responsibility | Long-lived? |
|---|---|---|
| **Server** | Owns archive state, validates frames, drives the protocol | Yes |
| **Client** | Reads/writes raw NeoTape records from a physical medium | No (per-volume) |

## Message types

| ID | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | `next_frame` | Server → Client | empty |
| 0x02 | `frame_record` | Client → Server | raw NeoTape record bytes |
| 0x03 | `tape_eof` | Client → Server | empty |
| 0x04 | `error` | bidirectional | UTF-8 error message |
| 0x05 | `ack_frame` | Server → Client | `uint64_t` LE global frame seq num |

## Wire format

Each message is framed:

```
[1 byte type][8 bytes payload length LE][N bytes payload]
```

The 8-byte length is little-endian and counts payload bytes only (not the
9-byte header).  Maximum payload length is implementation-defined; the
current limit is 16 MiB.

## Protocol flow

### Writing pipeline (Archiver = Server, Writer = Client)

Writer connects → Archiver sends `frame_record` → Writer sends `ack_frame` →
Archiver sends `tape_eof` at slice boundaries → Writer reconnects for next
volume.

### Reading pipeline (Extractor = Server, Reader = Client)

Reader connects → Extractor sends `next_frame` → Reader reads tape and sends
`frame_record` → Extractor validates and sends `ack_frame(seq_num)` →
repeat.  Reader sends `tape_eof` at end-of-tape, then disconnects.
Operator loads next volume and re-connects Reader.

## Error handling

Either side may send `error` at any time.  The sender SHOULD close the
connection after sending an `error` message.  The receiver SHOULD log the
error to stderr and exit non-zero.

## Sequence validation

The Server validates every `frame_record` for:
- `magic`, `header_version`
- `frame_hash`
- `archive_uuid` / `archive_label` consistency with first frame
- Monotonic, gapless `global_frame_seq_num`
- Monotonic `volume_seq_num` (advisory but gapless)
- `logical_slice_seq_num` and `frame_seq_num_within_channel` per spec

On validation failure the Server sends `error` with a human-readable message
and closes the connection.
```

**Step 3: Commit**

```bash
git add include/neotape/tcp_protocol.hpp docs/spec/13-tcp-protocol.md
git commit -m "docs: add Server/Client protocol roles; spec 13-tcp-protocol"
```

---

## Task 2: Extractor server — state machine and validation

**Files:**
- Create: `src/neotape_extractor.cpp`
- Create: `include/neotape/extractor.hpp`
- Modify: `Makefile`

**Goal:** Implement the extractor's core validation engine — parse each frame,
validate all sequence numbers, track channel groups, reassemble slice payloads.

### Extractor state

```cpp
struct ExtractorState {
    // Set from first frame, checked on every subsequent frame.
    std::string archive_uuid;
    std::string archive_label;

    // Sequence expectations — all start at 0, set from first frame.
    uint64_t expected_global_frame_seq = 0;
    uint64_t expected_volume_seq_num = 0;
    uint64_t expected_slice_seq_num = 0;

    // Per-channel tracking (reset on new channel group).
    uint64_t expected_channel_seq_num = 0;

    // Channel group state machine.
    enum class ChannelPhase { none, metadata, content };
    ChannelPhase current_phase = ChannelPhase::none;
    bool channel_started = false;    // saw START flag for current group

    // Slice payload reassembly.
    uint64_t current_slice_num = 0;
    std::vector<uint8_t> slice_payload;  // accumulated ch_content bytes

    bool saw_archive_end = false;
    bool saw_any_frame = false;
};

enum class ValidationResult {
    ok,
    reject,       // hard error, disconnect
};
```

### Step 1: Write validation function

```cpp
static ValidationResult validate_frame(const FrameHeader &hdr,
                                       ExtractorState &state,
                                       std::string &error_msg) {
    if (!state.saw_any_frame) {
        // First frame: capture identity.
        state.archive_uuid = hdr.archive_uuid;
        state.archive_label = hdr.archive_label;
        state.expected_global_frame_seq = hdr.global_frame_seq_num;
        state.expected_volume_seq_num = hdr.volume_seq_num;
        state.expected_slice_seq_num = hdr.logical_slice_seq_num;
        state.saw_any_frame = true;
    } else {
        // Identity checks.
        if (hdr.archive_uuid != state.archive_uuid) {
            error_msg = "archive_uuid mismatch";
            return ValidationResult::reject;
        }
        if (hdr.archive_label != state.archive_label) {
            error_msg = "archive_label mismatch";
            return ValidationResult::reject;
        }

        // Archive-end frame bypasses most seq checks.
        if (hdr.channel_type == ChannelType::ARCHIVE_END) {
            if (hdr.global_frame_seq_num != state.expected_global_frame_seq) {
                error_msg = "archive_end global_frame_seq_num mismatch";
                return ValidationResult::reject;
            }
            return ValidationResult::ok;
        }

        // Global frame sequence — must be exactly expected.
        if (hdr.global_frame_seq_num != state.expected_global_frame_seq) {
            error_msg = std::format(
                "global_frame_seq_num mismatch: expected {}, got {}",
                state.expected_global_frame_seq,
                hdr.global_frame_seq_num);
            return ValidationResult::reject;
        }

        // Volume sequence — advisory but monotonic.
        if (hdr.volume_seq_num < state.expected_volume_seq_num) {
            error_msg = std::format(
                "volume_seq_num went backwards: {} -> {}",
                state.expected_volume_seq_num, hdr.volume_seq_num);
            return ValidationResult::reject;
        }
        if (hdr.volume_seq_num > state.expected_volume_seq_num + 1) {
            error_msg = std::format(
                "volume_seq_num skipped: {} -> {}",
                state.expected_volume_seq_num, hdr.volume_seq_num);
            return ValidationResult::reject;
        }

        // Slice sequence.
        if (hdr.logical_slice_seq_num != state.expected_slice_seq_num &&
            hdr.logical_slice_seq_num != state.expected_slice_seq_num + 1) {
            error_msg = std::format(
                "logical_slice_seq_num jumped: {} -> {}",
                state.expected_slice_seq_num,
                hdr.logical_slice_seq_num);
            return ValidationResult::reject;
        }
    }

    // Channel tracking.
    if (hdr.channel_type != ChannelType::ARCHIVE_END) {
        bool slice_advanced =
            hdr.logical_slice_seq_num != state.expected_slice_seq_num;
        bool is_start = has_frame_flag_start(hdr.flags);

        if (slice_advanced || is_start) {
            // New channel group — reset per-channel seq.
            state.channel_started = true;
            state.expected_channel_seq_num = 1;

            if (slice_advanced) {
                state.expected_slice_seq_num = hdr.logical_slice_seq_num;
                state.current_phase = ChannelPhase::none;
            }
        }

        // Channel ordering: metadata must precede content within a slice.
        if (hdr.channel_type == ChannelType::CH_CONTENT) {
            state.current_phase = ChannelPhase::content;
        } else if (hdr.channel_type == ChannelType::CH_METADATA) {
            if (state.current_phase == ChannelPhase::content) {
                error_msg = "metadata frame after content in same slice";
                return ValidationResult::reject;
            }
            state.current_phase = ChannelPhase::metadata;
        }

        // Per-channel sequence.
        if (hdr.frame_seq_num_within_channel !=
            state.expected_channel_seq_num) {
            error_msg = std::format(
                "frame_seq_num_within_channel mismatch: expected {}, got {}",
                state.expected_channel_seq_num,
                hdr.frame_seq_num_within_channel);
            return ValidationResult::reject;
        }
        state.expected_channel_seq_num++;

        if (has_frame_flag_end(hdr.flags)) {
            state.channel_started = false;
        }
    }

    // Advance global frame seq.
    state.expected_global_frame_seq = hdr.global_frame_seq_num + 1;
    state.expected_volume_seq_num = hdr.volume_seq_num;

    return ValidationResult::ok;
}
```

### Step 2: Write `serve_client()` — the per-connection loop

```cpp
struct ExtractorOptions {
    std::string listen_address;
    std::string output_path;    // empty = stdout
    bool verbose = false;
};

struct ExtractorResult {
    uint64_t frames_served = 0;
    uint64_t bytes_output = 0;
    bool archive_complete = false;
};

static ExtractorResult serve_client(int client_fd,
                                     const ExtractorOptions &opts) {
    ExtractorState state;
    ExtractorResult result;

    for (;;) {
        // 1. Request next frame.
        tcp::write_message(client_fd,
                           Message{MessageType::next_frame});

        // 2. Read response.
        auto msg = tcp::read_message(client_fd);
        if (!msg) {
            // Client disconnected cleanly.
            break;
        }

        if (msg->type == MessageType::tape_eof) {
            // End of this volume — flush current slice if any.
            if (!state.slice_payload.empty()) {
                write_slice_payload(state.slice_payload, opts);
                result.bytes_output += state.slice_payload.size();
                state.slice_payload.clear();
            }
            state.expected_slice_seq_num++;
            if (opts.verbose) {
                std::cerr << "extractor: volume ended, "
                          << result.frames_served
                          << " frames served so far\n";
            }
            continue;  // wait for next client to reconnect
        }

        if (msg->type == MessageType::error) {
            std::string err(reinterpret_cast<const char *>(msg->payload.data()),
                            msg->payload.size());
            throw std::runtime_error("reader error: " + err);
        }

        if (msg->type != MessageType::frame_record) {
            throw std::runtime_error(
                std::format("unexpected message type {}",
                            static_cast<int>(msg->type)));
        }

        // 3. Parse and validate.
        auto *data = reinterpret_cast<const uint8_t *>(msg->payload.data());
        FrameHeader hdr = parse_fixed_header(data, msg->payload.size());

        // Verify frame_hash.
        Hash computed = compute_frame_hash(data, msg->payload.size());
        if (computed != hdr.frame_hash) {
            std::string err = std::format(
                "frame_hash mismatch at global_seq={}",
                hdr.global_frame_seq_num);
            tcp::write_message(client_fd,
                Message{MessageType::error,
                        std::vector<std::byte>(
                            reinterpret_cast<const std::byte *>(err.data()),
                            reinterpret_cast<const std::byte *>(err.data()) + err.size())});
            throw std::runtime_error(err);
        }

        // Validate sequence numbers.
        std::string error_msg;
        if (validate_frame(hdr, state, error_msg) ==
            ValidationResult::reject) {
            tcp::write_message(client_fd,
                Message{MessageType::error,
                        std::vector<std::byte>(
                            reinterpret_cast<const std::byte *>(error_msg.data()),
                            reinterpret_cast<const std::byte *>(error_msg.data()) + error_msg.size())});
            throw std::runtime_error(error_msg);
        }

        // 4. Ack.
        std::vector<std::byte> ack_payload(8);
        uint64_t ack_seq = hdr.global_frame_seq_num;
        for (int i = 0; i < 8; ++i)
            ack_payload[i] = static_cast<std::byte>(
                (ack_seq >> (i * 8)) & 0xffu);
        tcp::write_message(client_fd,
                           Message{MessageType::ack_frame, ack_payload});

        result.frames_served++;

        // 5. Handle by channel type.
        if (hdr.channel_type == ChannelType::ARCHIVE_END) {
            if (has_frame_flag_clean_end(hdr.flags)) {
                // Flush any remaining payload.
                if (!state.slice_payload.empty()) {
                    write_slice_payload(state.slice_payload, opts);
                    result.bytes_output += state.slice_payload.size();
                }
                result.archive_complete = true;
                if (opts.verbose) {
                    std::cerr << "extractor: archive complete, "
                              << result.frames_served
                              << " frames, " << result.bytes_output
                              << " bytes output\n";
                }
                return result;
            } else {
                throw std::runtime_error(
                    "archive_end frame without CLEAN_END");
            }
        }

        // Detect new slice — flush previous slice payload.
        if (hdr.logical_slice_seq_num != state.current_slice_num &&
            !state.slice_payload.empty()) {
            write_slice_payload(state.slice_payload, opts);
            result.bytes_output += state.slice_payload.size();
            state.slice_payload.clear();
        }
        state.current_slice_num = hdr.logical_slice_seq_num;

        // Accumulate ch_content payload.
        if (hdr.channel_type == ChannelType::CH_CONTENT &&
            hdr.frame_payload_size > 0) {
            const uint8_t *payload_start = data + fixed_header_size;
            state.slice_payload.insert(state.slice_payload.end(),
                                       payload_start,
                                       payload_start + hdr.frame_payload_size);
        }
    }

    return result;
}
```

### Step 3: Write `write_slice_payload` helper

```cpp
static void write_slice_payload(const std::vector<uint8_t> &payload,
                                const ExtractorOptions &opts) {
    if (payload.empty())
        return;
    if (opts.output_path.empty()) {
        // stdout
        std::cout.write(reinterpret_cast<const char *>(payload.data()),
                        static_cast<std::streamsize>(payload.size()));
    } else {
        // file — open/append
        static std::ofstream out_file;
        static bool opened = false;
        if (!opened) {
            out_file.open(opts.output_path, std::ios::binary);
            if (!out_file) {
                throw std::runtime_error(
                    "cannot open output file: " + opts.output_path);
            }
            opened = true;
        }
        out_file.write(reinterpret_cast<const char *>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
    }
}
```

### Step 4: Write `run_tcp_extractor()` main loop

```cpp
uint64_t run_tcp_extractor(const ExtractorOptions &opts) {
    tcp::Address addr = tcp::parse_address(opts.listen_address);

    int server_fd;
    if (addr.is_unix) {
        server_fd = create_unix_listen_socket(addr.path);
    } else {
        server_fd = create_tcp_listen_socket(addr.host, addr.port);
    }

    if (opts.verbose) {
        std::cerr << "extractor listening on " << opts.listen_address << "\n";
    }

    uint64_t total_frames = 0;
    uint64_t total_bytes = 0;

    for (;;) {
        int client_fd = accept_client(server_fd, addr.is_unix);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("accept failed");
        }

        try {
            ExtractorResult r = serve_client(client_fd, opts);
            ::close(client_fd);
            total_frames += r.frames_served;
            total_bytes += r.bytes_output;
            if (r.archive_complete) {
                std::cerr << std::format(
                    "extractor: archive complete, {} frames, {} bytes\n",
                    total_frames, total_bytes);
                break;
            }
        } catch (const std::exception &e) {
            std::cerr << "extractor: " << e.what() << "\n";
            ::close(client_fd);
            // Keep listening for reconnection (multi-volume).
        }
    }

    ::close(server_fd);
    return total_frames;
}
```

### Step 5: Add to `include/neotape/extractor.hpp`

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace neotape {

struct ExtractorOptions {
    std::string listen_address; // "tcp://0.0.0.0:9124" or "unix:///path"
    std::string output_path;    // empty = stdout
    bool verbose = false;
};

// Blocks until archive is complete or fatal error.
// Returns total frames validated.
uint64_t run_tcp_extractor(const ExtractorOptions &opts);

} // namespace neotape
```

### Step 6: Add `bin/neotape-extractor` to Makefile

```makefile
# In EXE list: append `bin/neotape-extractor`
EXE = ... bin/neotape-extractor

# New link rule:
$(BINDIR)/neotape-extractor : src/neotape_extractor.cpp src/neotape_format.o src/neotape_tcp_protocol.o src/neotape_common.o | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(filter %.o,$^) -o $@ $(LDLIBS)
```

### Step 7: Build and verify compilation

```bash
make -j$(nproc)
```

**Expected:** `bin/neotape-extractor` produced, no warnings.

### Step 8: Commit

```bash
git add src/neotape_extractor.cpp include/neotape/extractor.hpp Makefile
git commit -m "feat: add neotape-extractor server with frame validation"
```

---

## Task 3: Extractor CLI entry point

**Files:**
- Create: `src/neotape_extractor_cmd.cpp`
- Modify: `Makefile`

**Goal:** `getopt_long` CLI for the extractor, mirroring the archiver's pattern.

### Step 1: Write `src/neotape_extractor_cmd.cpp`

```cpp
#include "neotape/extractor.hpp"
#include "neotape/common.hpp"

#include <cstdlib>
#include <format>
#include <getopt.h>
#include <iostream>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << std::format("neotape-extractor: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << std::format(
        "usage: {} --listen <tcp://host:port|unix://path>\n"
        "       [-o <output-file>] [-v]\n",
        prog);
}

struct CliOptions {
    std::string listen_address;
    std::string output_path;
    bool verbose = false;
};

CliOptions parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    CliOptions opts;
    int c;
    while ((c = getopt_long(argc, argv, "l:o:vh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'o':
            opts.output_path = optarg;
            break;
        case 'v':
            opts.verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.listen_address.empty())
        fail("--listen is required");
    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        CliOptions cli = parse_args(argc, argv);
        neotape::ExtractorOptions opts;
        opts.listen_address = cli.listen_address;
        opts.output_path = cli.output_path;
        opts.verbose = cli.verbose;

        neotape::run_tcp_extractor(opts);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
```

### Step 2: Update Makefile to use the new entry point

```makefile
# Link rule uses src/neotape_extractor_cmd.o instead of compiling
# neotape_extractor.cpp directly:
$(BINDIR)/neotape-extractor : src/neotape_extractor_cmd.o src/neotape_extractor.o src/neotape_format.o src/neotape_tcp_protocol.o src/neotape_common.o | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)
```

### Step 3: Build and verify

```bash
make -j$(nproc)
bin/neotape-extractor --help
```

**Expected:** Usage printed.

### Step 4: Commit

```bash
git add src/neotape_extractor_cmd.cpp Makefile
git commit -m "feat: add neotape-extractor CLI entry point"
```

---

## Task 4: Rewrite `neotape-read` as TCP client

**Files:**
- Rewrite: `src/neotape_read_cmd.cpp`
- Modify: `Makefile` (no change needed, existing rule still works)

**Goal:** Replace the raw-record-copier with a TCP client that connects to
the extractor, reads frames on demand, and forwards them.

### Step 1: Complete rewrite

```cpp
#include "neotape/common.hpp"
#include "neotape/format.hpp"
#include "neotape/tape.hpp"
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace neotape;
using namespace neotape::tcp;

namespace {

using std::format;
using std::string;
using std::vector;

struct Options {
    string source;       // "tape:/dev/nst0" or "spool:./dir"
    string connect_addr; // "tcp://host:port" or "unix:///path"
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-read: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format("usage: {} --source <tape:/dev/nst0|spool:./dir>\n"
                        "       --connect <tcp://host:port|unix://path>\n",
                        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"connect", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:c:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = optarg;
            break;
        case 'c':
            opts.connect_addr = optarg;
            break;
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (opts.source.empty()) fail("--source is required");
    if (opts.connect_addr.empty()) fail("--connect is required");
    return opts;
}

int connect_to_server(const string &addr_str) {
    Address addr = parse_address(addr_str);
    int fd;
    if (addr.is_unix) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) fail("socket(AF_UNIX) failed");
        struct sockaddr_un sa {};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, addr.path.c_str(), sizeof(sa.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&sa),
                      sizeof(sa)) < 0) {
            fail(format("connect to {} failed: {}", addr.path,
                        std::strerror(errno)));
        }
    } else {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) fail("socket(AF_INET) failed");
        struct addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (::getaddrinfo(addr.host.c_str(), addr.port.c_str(), &hints,
                          &res) != 0) {
            fail(format("getaddrinfo failed for {}:{}", addr.host, addr.port));
        }
        if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            ::freeaddrinfo(res);
            fail(format("connect to {}:{} failed: {}", addr.host, addr.port,
                        std::strerror(errno)));
        }
        ::freeaddrinfo(res);
    }
    return fd;
}

// Source abstraction — reads NeoTape records from tape or spool.
class RecordReader {
  public:
    virtual ~RecordReader() = default;
    virtual std::optional<vector<std::byte>> read_record() = 0;
    virtual bool hit_eof() = 0;
};

class SpoolReader final : public RecordReader {
  public:
    explicit SpoolReader(mt::SpoolTapeDevice *dev) : dev_(dev) {}

    std::optional<vector<std::byte>> read_record() override {
        // Read header-sized chunk first.
        vector<std::byte> pending;
        while (pending.size() < fixed_header_size) {
            if (dev_->fd() < 0) return std::nullopt;
            std::byte tmp[65536];
            ssize_t n = ::read(dev_->fd(), tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EIO) {
                    dev_->space_fwd_filemark(1);
                    pending.clear();
                    continue;
                }
                throw std::runtime_error(
                    format("read: {}", std::strerror(errno)));
            }
            if (n == 0) {
                dev_->space_fwd_filemark(1);
                pending.clear();
                continue;
            }
            pending.insert(pending.end(), tmp, tmp + n);
        }
        FrameHeader header = parse_fixed_header(
            reinterpret_cast<const uint8_t *>(pending.data()),
            fixed_header_size);
        size_t record_size = decoded_block_size(header);
        while (pending.size() < record_size) {
            if (dev_->fd() < 0)
                throw std::runtime_error("truncated record in spool");
            std::byte tmp[65536];
            ssize_t n = ::read(dev_->fd(), tmp, sizeof(tmp));
            if (n <= 0)
                throw std::runtime_error("truncated record in spool");
            pending.insert(pending.end(), tmp, tmp + n);
        }
        vector<std::byte> record(pending.begin(),
                                 pending.begin() + record_size);
        return record;
    }

    bool hit_eof() override { return dev_->fd() < 0; }

  private:
    mt::SpoolTapeDevice *dev_;
};

class TapeReader final : public RecordReader {
  public:
    explicit TapeReader(mt::TapeDevice *dev)
        : dev_(dev), buffer_(max_block_size) {}

    std::optional<vector<std::byte>> read_record() override {
        ssize_t n = ::read(dev_->fd(), buffer_.data(), buffer_.size());
        if (n < 0) {
            if (errno == EIO) {
                dev_->space_fwd_filemark(1);
                return std::nullopt;
            }
            throw std::runtime_error(
                format("read: {}", std::strerror(errno)));
        }
        if (n == 0) {
            if (dev_->status().eod()) return std::nullopt; // EOF
            dev_->space_fwd_filemark(1);
            return std::nullopt;
        }
        return vector<std::byte>(buffer_.data(), buffer_.data() + n);
    }

    bool hit_eof() override { return dev_->status().eod(); }

  private:
    mt::TapeDevice *dev_;
    vector<std::byte> buffer_;
};

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);

        // Open source.
        std::unique_ptr<mt::TapeDevice> source_dev;
        bool is_tape = false;
        if (opts.source.rfind("tape:", 0) == 0) {
            auto dev = std::make_unique<mt::TapeDevice>(opts.source.substr(5),
                                                         false);
            int flags = ::fcntl(dev->fd(), F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(dev->fd(), F_SETFL, flags & ~O_NONBLOCK);
            dev->rewind();
            source_dev = std::move(dev);
            is_tape = true;
        } else if (opts.source.rfind("spool:", 0) == 0) {
            string spool_path = opts.source.substr(6);
            if (!std::filesystem::exists(spool_path))
                fail("source spool directory does not exist: " + spool_path);
            source_dev = std::make_unique<mt::SpoolTapeDevice>(
                std::filesystem::path(spool_path), false);
        } else {
            fail("source must be tape:<device> or spool:<dir>");
        }

        // Create reader.
        std::unique_ptr<RecordReader> reader;
        if (is_tape) {
            reader = std::make_unique<TapeReader>(source_dev.get());
        } else {
            reader = std::make_unique<SpoolReader>(
                dynamic_cast<mt::SpoolTapeDevice *>(source_dev.get()));
        }

        // Connect to extractor.
        int conn_fd = connect_to_server(opts.connect_addr);

        uint64_t frames_sent = 0;
        bool eof_sent = false;

        // Protocol loop: wait for next_frame, send frame_record or tape_eof.
        for (;;) {
            auto msg = read_message(conn_fd);
            if (!msg) {
                std::cerr << "neotape-read: extractor disconnected\n";
                break;
            }

            if (msg->type == MessageType::error) {
                string err(reinterpret_cast<const char *>(msg->payload.data()),
                           msg->payload.size());
                ::close(conn_fd);
                fail("extractor error: " + err);
            }

            if (msg->type == MessageType::ack_frame) {
                // Just received ack — extractor will send next_frame next.
                continue;
            }

            if (msg->type != MessageType::next_frame) {
                ::close(conn_fd);
                fail("unexpected message type from extractor");
            }

            if (eof_sent) {
                // Already sent tape_eof, disconnect.
                ::close(conn_fd);
                break;
            }

            // Read next record from source.
            auto record = reader->read_record();

            // Check EOF.
            if (!record && reader->hit_eof()) {
                write_message(conn_fd, Message{MessageType::tape_eof});
                eof_sent = true;
                continue;
            }

            if (!record) {
                // Filemark — just retry reading.
                continue;
            }

            write_message(conn_fd,
                          Message{MessageType::frame_record, *record});
            frames_sent++;
        }

        std::cerr << format("neotape-read: {} frames sent\n", frames_sent);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
}
```

### Step 2: Build and smoke-test

```bash
make -j$(nproc)
```

**Expected:** `bin/neotape-read` builds cleanly.

### Step 3: Commit

```bash
git add src/neotape_read_cmd.cpp
git commit -m "feat: rewrite neotape-read as TCP extractor client"
```

---

## Task 5: Integration smoke test

**Files:**
- Create: `tests/smoke_tcp_extract.sh`

**Goal:** End-to-end test: archiver produces archive to spool, extractor reads
it back via TCP, output matches original.

### Step 1: Write the test

```bash
#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Create test input.
mkdir "$tmp/input"
echo "hello world" > "$tmp/input/hello.txt"
echo "another file" > "$tmp/input/bar.txt"

# Run archiver in local (non-server) mode → pax output.
bin/neotape-archiver -C "$tmp/input" -f "$tmp/archive.pax" hello.txt bar.txt

# Store original content for comparison.
cat "$tmp/input/hello.txt" "$tmp/input/bar.txt" > "$tmp/original.cat"

# ---- Write phase: archiver server → writer → spool ----
archiver_sock="unix://$tmp/archiver.sock"

bin/neotape-archiver --listen "$archiver_sock" \
    --archive-name smoke-extract -C "$tmp/input" hello.txt bar.txt &
archiver_pid=$!

# Wait for socket.
for i in $(seq 1 30); do
    [ -S "$tmp/archiver.sock" ] && break
    sleep 0.1
done

bin/neotape-write --source "$archiver_sock" --target "spool:$tmp/out" --erase
wait "$archiver_pid"

# ---- Read phase: extractor server ← reader ← spool ----
extractor_sock="unix://$tmp/extractor.sock"

bin/neotape-extractor --listen "$extractor_sock" -o "$tmp/extracted.pax" &
extractor_pid=$!

for i in $(seq 1 30); do
    [ -S "$tmp/extractor.sock" ] && break
    sleep 0.1
done

bin/neotape-read --source "spool:$tmp/out" --connect "$extractor_sock"
wait "$extractor_pid"

# Verify output matches.
if cmp "$tmp/archive.pax" "$tmp/extracted.pax"; then
    echo "smoke_tcp_extract: ok"
else
    echo "smoke_tcp_extract: FAIL - output mismatch"
    exit 1
fi
```

### Step 2: Run the test

```bash
sh tests/smoke_tcp_extract.sh
```

**Expected:** `smoke_tcp_extract: ok`

### Step 3: Add to `make test`

```makefile
# In the test recipe, add:
	sh tests/smoke_tcp_extract.sh
```

### Step 4: Commit

```bash
git add tests/smoke_tcp_extract.sh Makefile
git commit -m "test: add TCP extractor smoke test"
```

---

## Task 6: Update CLI appendix

**Files:**
- Modify: `docs/spec/12-appendix-cli.md`

**Step 1:** Add extractor/reader entries after the existing archiver pipeline
section.

### Step 2: Commit

```bash
git add docs/spec/12-appendix-cli.md
git commit -m "docs: add extractor/reader to CLI appendix"
```

---

## Task 7: Multi-volume smoke test

**Files:**
- Create: `tests/smoke_tcp_extract_multi.sh`

**Goal:** Verify the extractor handles multi-volume (reader disconnect/reconnect)
correctly across two spool directories.

### Step 1: Write the test

Adapt `tests/smoke_tcp_archive_multi.sh` — produce two volumes, read both
through the extractor, verify output matches.

### Step 2: Run and commit

```bash
sh tests/smoke_tcp_extract_multi.sh
git add tests/smoke_tcp_extract_multi.sh Makefile
git commit -m "test: add multi-volume extractor smoke test"
```

---

## Task 8: Final integration — run all tests

```bash
make -j$(nproc) && make test
```

**Expected:** All 6 tests pass.

---

## Summary

| Task | Files | Description |
|---|---|---|
| 1 | `tcp_protocol.hpp`, `13-tcp-protocol.md` | Protocol roles doc |
| 2 | `neotape_extractor.cpp`, `extractor.hpp`, `Makefile` | Extractor core |
| 3 | `neotape_extractor_cmd.cpp`, `Makefile` | Extractor CLI |
| 4 | `neotape_read_cmd.cpp` | Reader rewrite |
| 5 | `smoke_tcp_extract.sh`, `Makefile` | Integration smoke test |
| 6 | `12-appendix-cli.md` | CLI appendix update |
| 7 | `smoke_tcp_extract_multi.sh`, `Makefile` | Multi-volume test |
| 8 | — | Final test run |
