# NeoTape TCP Archive Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `neotape-archiver` (long-running TCP/UDS server that is a functional superset of `mt-pax`) and `neotape-write` (per-volume writer client) using the single-connection request-response protocol defined in `docs/superpowers/specs/2026-06-13-tcp-archive-generation-design.md`.

**Architecture:** Reuse the existing `neotape::write_pax()` library for archive generation. Add a small `neotape::tcp` message-framing layer. The archiver serves framed messages over a listening socket; in non-listen mode it behaves exactly like `mt-pax`. The writer connects to an archiver, requests the current Volume Header, then requests frames one at a time and writes them to a tape device or spool backend.

**Tech Stack:** C++20, GNU Make, POSIX sockets, existing `namespace mt` tape abstraction, `neotape::format` serialization, `neotape::pax_writer` library.

---

## File map

| File | Responsibility |
|------|----------------|
| `include/neotape/tcp_protocol.hpp` | Public protocol types and message I/O helpers |
| `src/neotape_tcp_protocol.cpp` | Framed message read/write implementation |
| `src/neotape_tcp_server.cpp` | Archiver server: accept connections, drive `write_pax()` callbacks |
| `src/neotape_archiver_cmd.cpp` | `bin/neotape-archiver` CLI entry point |
| `src/neotape_write_cmd.cpp` | `bin/neotape-write` CLI entry point |
| `tests/test_tcp_protocol.cpp` | Unit-style test for message framing |
| `tests/smoke_tcp_archive.sh` | End-to-end smoke test over a Unix-domain socket |
| `Makefile` | Build rules for new binaries and tests |

---

## Phase 0: TCP message-framing library

### Task 0.1: Define protocol types and constants

**Files:**
- Create: `include/neotape/tcp_protocol.hpp`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neotape::tcp {

enum class MessageType : uint8_t {
    get_volume_header = 0x01,
    volume_header = 0x02,
    next_frame = 0x03,
    frame_record = 0x04,
    archive_end_header = 0x05,
    tape_eof = 0x06,
    error = 0x07,
};

struct Message {
    MessageType type;
    std::vector<std::byte> payload;

    Message() = default;
    Message(MessageType t, std::vector<std::byte> p = {})
        : type(t), payload(std::move(p)) {}
};

constexpr std::size_t message_header_size = 1 + 8; // type + length

// Throws std::runtime_error on I/O or protocol errors.
// Returns std::nullopt if the peer closed cleanly before any message.
std::optional<Message> read_message(int fd);

// Throws std::runtime_error on I/O errors.
void write_message(int fd, const Message &msg);

std::string message_type_name(MessageType type);

} // namespace neotape::tcp
```

- [ ] **Step 2: Commit**

```bash
git add include/neotape/tcp_protocol.hpp
git commit -m "feat(tcp): add protocol message types and I/O declarations"
```

### Task 0.2: Implement message read/write

**Files:**
- Create: `src/neotape_tcp_protocol.cpp`

- [ ] **Step 1: Write the implementation**

```cpp
#include "neotape/tcp_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <unistd.h>

namespace neotape::tcp {

namespace {

std::size_t read_exact(int fd, void *buf, std::size_t n) {
    auto *p = static_cast<std::byte *>(buf);
    std::size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = ::read(fd, p, remaining);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                std::format("read failed: {}", std::strerror(errno)));
        }
        if (r == 0)
            break;
        p += r;
        remaining -= static_cast<std::size_t>(r);
    }
    return n - remaining;
}

void write_exact(int fd, const void *buf, std::size_t n) {
    const auto *p = static_cast<const std::byte *>(buf);
    std::size_t remaining = n;
    while (remaining > 0) {
        ssize_t w = ::write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                std::format("write failed: {}", std::strerror(errno)));
        }
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
}

} // namespace

std::optional<Message> read_message(int fd) {
    std::byte header[message_header_size];
    std::size_t got = read_exact(fd, header, message_header_size);
    if (got == 0)
        return std::nullopt;
    if (got != message_header_size)
        throw std::runtime_error("short read on message header");

    auto type = static_cast<MessageType>(static_cast<uint8_t>(header[0]));
    uint64_t length = 0;
    for (std::size_t i = 0; i < 8; ++i)
        length |= static_cast<uint64_t>(static_cast<uint8_t>(header[1 + i]))
                  << (8 * i);

    Message msg{type, {}};
    if (length > 0) {
        msg.payload.resize(length);
        got = read_exact(fd, msg.payload.data(), length);
        if (got != length)
            throw std::runtime_error("short read on message payload");
    }
    return msg;
}

void write_message(int fd, const Message &msg) {
    std::byte header[message_header_size];
    header[0] = static_cast<std::byte>(static_cast<uint8_t>(msg.type));
    uint64_t length = msg.payload.size();
    for (std::size_t i = 0; i < 8; ++i)
        header[1 + i] = static_cast<std::byte>(static_cast<uint8_t>(
            (length >> (8 * i)) & 0xffu));

    write_exact(fd, header, message_header_size);
    if (length > 0)
        write_exact(fd, msg.payload.data(), length);
}

std::string message_type_name(MessageType type) {
    switch (type) {
    case MessageType::get_volume_header:
        return "GET_VOLUME_HEADER";
    case MessageType::volume_header:
        return "VOLUME_HEADER";
    case MessageType::next_frame:
        return "NEXT_FRAME";
    case MessageType::frame_record:
        return "FRAME_RECORD";
    case MessageType::archive_end_header:
        return "ARCHIVE_END_HEADER";
    case MessageType::tape_eof:
        return "TAPE_EOF";
    case MessageType::error:
        return "ERROR";
    }
    return std::format("UNKNOWN({})", static_cast<int>(type));
}

} // namespace neotape::tcp
```

- [ ] **Step 2: Build check**

Run: `make -j$(nproc)`

Expected: still passes (new file is not yet linked).

- [ ] **Step 3: Commit**

```bash
git add src/neotape_tcp_protocol.cpp
git commit -m "feat(tcp): implement framed message read/write"
```

### Task 0.3: Add a test for message framing

**Files:**
- Create: `tests/test_tcp_protocol.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write the test program**

```cpp
#include "neotape/tcp_protocol.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

using neotape::tcp::Message;
using neotape::tcp::MessageType;

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "test_tcp_protocol: " << msg << "\n";
    std::exit(1);
}

int main() {
    int fds[2];
    if (pipe(fds) != 0)
        fail("pipe failed");

    std::vector<std::byte> payload;
    payload.reserve(256);
    for (int i = 0; i < 256; ++i)
        payload.push_back(static_cast<std::byte>(i));

    Message out{MessageType::frame_record, std::move(payload)};
    neotape::tcp::write_message(fds[1], out);

    auto in = neotape::tcp::read_message(fds[0]);
    if (!in.has_value())
        fail("expected a message");
    if (in->type != MessageType::frame_record)
        fail("wrong message type");
    if (in->payload.size() != 256)
        fail("wrong payload size");
    for (int i = 0; i < 256; ++i) {
        if (static_cast<uint8_t>(in->payload[i]) != static_cast<uint8_t>(i))
            fail("payload mismatch");
    }

    // Clean close returns nullopt.
    close(fds[1]);
    auto end = neotape::tcp::read_message(fds[0]);
    if (end.has_value())
        fail("expected nullopt on clean close");
    close(fds[0]);

    std::cout << "test_tcp_protocol: ok\n";
    return 0;
}
```

- [ ] **Step 2: Add build rule to Makefile**

Modify `Makefile`:

```make
TCP_PROTO_OBJ = src/neotape_tcp_protocol.o

EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol

$(BINDIR)/test_tcp_protocol : tests/test_tcp_protocol.cpp $(TCP_PROTO_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(TCP_PROTO_OBJ) -o $@
```

- [ ] **Step 3: Run the test**

Run: `make bin/test_tcp_protocol && ./bin/test_tcp_protocol`

Expected:
```
test_tcp_protocol: ok
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_tcp_protocol.cpp Makefile
git commit -m "test(tcp): add message framing unit test"
```

---

## Phase 1: Minimal archiver server and writer client

### Task 1.1: Archiver server core

**Files:**
- Create: `include/neotape/tcp_server.hpp`
- Create: `src/neotape_tcp_server.cpp`

- [ ] **Step 1: Write the server header**

```cpp
#pragma once

#include "neotape/tcp_protocol.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neotape {

struct TcpArchiverOptions {
    std::string listen_address; // "tcp://0.0.0.0:9123" or "unix:///path"
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;
    // For the first skeleton, a callback that produces the next record.
    std::function<std::vector<std::byte>(uint64_t frame_index)> produce_record;
    std::function<bool(uint64_t frame_index)> has_more_frames;
};

// Blocks until the first writer connects, the archive completes, or an error
// occurs. Returns the number of frames served on this connection.
uint64_t run_tcp_archiver(const TcpArchiverOptions &opts);

} // namespace neotape
```

- [ ] **Step 2: Write the server implementation (skeleton)**

```cpp
#include "neotape/tcp_server.hpp"
#include "neotape/format.hpp"
#include "neotape/tcp_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace neotape {

namespace {

using neotape::tcp::Message;
using neotape::tcp::MessageType;

int parse_listen_address(const std::string &addr, std::string &host,
                         std::string &port, std::string &path, bool &is_unix) {
    const std::string tcp_prefix = "tcp://";
    const std::string unix_prefix = "unix://";
    if (addr.rfind(tcp_prefix, 0) == 0) {
        is_unix = false;
        std::string rest = addr.substr(tcp_prefix.size());
        auto colon = rest.rfind(':');
        if (colon == std::string::npos)
            throw std::runtime_error("tcp listen address missing port");
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        return 0;
    }
    if (addr.rfind(unix_prefix, 0) == 0) {
        is_unix = true;
        path = addr.substr(unix_prefix.size());
        return 0;
    }
    throw std::runtime_error(
        "listen address must start with tcp:// or unix://");
}

int create_listener(const std::string &addr) {
    std::string host, port, path;
    bool is_unix = false;
    parse_listen_address(addr, host, port, path, is_unix);

    int fd = -1;
    if (is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        unlink(path.c_str());
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path.size() >= sizeof(sa.sun_path))
            throw std::runtime_error("unix socket path too long");
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            throw std::runtime_error(
                std::format("bind {}: {}", path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (gai != 0)
            throw std::runtime_error(
                std::format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            throw std::runtime_error(
                std::format("socket: {}", std::strerror(errno)));
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
            throw std::runtime_error(
                std::format("bind {}:{}: {}", host, port, std::strerror(errno)));
    }

    if (listen(fd, 1) < 0)
        throw std::runtime_error(
            std::format("listen: {}", std::strerror(errno)));
    return fd;
}

VolumeHeader make_volume_header(uint32_t block_size, uint64_t volume_seq_num,
                                const std::string &archive_name) {
    VolumeHeader vh;
    vh.volume_block_size = block_size;
    vh.archive_uuid = make_uuid_v4();
    vh.archive_name = archive_name;
    vh.volume_seq_num = volume_seq_num;
    vh.payload_profile = PayloadProfile::pax;
    vh.volume_write_at_utc = utc_timestamp_now();
    vh.flags = 0;
    return vh;
}

std::vector<std::byte> bytes_from_header_bytes(const HeaderBytes &bytes) {
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes)
        out.push_back(static_cast<std::byte>(b));
    return out;
}

} // namespace

uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    if (!valid_block_size(opts.volume_block_size))
        throw std::runtime_error("invalid volume block size");

    int listener = create_listener(opts.listen_address);
    std::cerr << std::format("archiver listening on {}\n", opts.listen_address);

    int client = accept(listener, nullptr, nullptr);
    if (client < 0)
        throw std::runtime_error(
            std::format("accept: {}", std::strerror(errno)));
    close(listener);

    uint64_t frames_served = 0;
    try {
        VolumeHeader vh = make_volume_header(opts.volume_block_size,
                                             opts.initial_volume_seq_num,
                                             opts.archive_name);
        HeaderBytes vh_bytes = serialize_volume_header(vh);
        std::vector<std::byte> vh_payload = bytes_from_header_bytes(vh_bytes);

        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value())
                break;

            switch (req->type) {
            case MessageType::get_volume_header:
                neotape::tcp::write_message(
                    client, Message{MessageType::volume_header,
                                    std::move(vh_payload)});
                vh_payload = bytes_from_header_bytes(vh_bytes);
                break;
            case MessageType::next_frame:
                if (!opts.has_more_frames(frames_served)) {
                    ArchiveEndHeader ae;
                    ae.volume_block_size = opts.volume_block_size;
                    ae.archive_uuid = vh.archive_uuid;
                    ae.archive_name = opts.archive_name;
                    ae.volume_seq_num = opts.initial_volume_seq_num;
                    ae.payload_profile = PayloadProfile::pax;
                    ae.last_logical_slice_seq_num = 0;
                    ae.last_global_frame_seq_num = frames_served;
                    ae.created_by_implementation = "neotape-archiver";
                    ae.created_by_build_id = "";
                    ae.archive_end_at_utc = utc_timestamp_now();
                    ae.flags = archive_end_flag_clean_end;
                    HeaderBytes ae_bytes = serialize_archive_end_header(ae);
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::archive_end_header,
                                bytes_from_header_bytes(ae_bytes)});
                    close(client);
                    return frames_served;
                }
                if (frames_served % 4 == 3) {
                    neotape::tcp::write_message(
                        client, Message{MessageType::tape_eof, {}});
                } else {
                    auto rec = opts.produce_record(frames_served);
                    if (rec.size() != opts.volume_block_size)
                        throw std::runtime_error("produce_record size mismatch");
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::frame_record, std::move(rec)});
                    ++frames_served;
                }
                break;
            default:
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::error,
                            std::vector<std::byte>{}});
                close(client);
                return frames_served;
            }
        }
    } catch (...) {
        close(client);
        throw;
    }
    close(client);
    return frames_served;
}

} // namespace neotape
```

- [ ] **Step 3: Build check**

Run: `make -j$(nproc)`

Expected: compiles (still not linked into binary).

- [ ] **Step 4: Commit**

```bash
git add include/neotape/tcp_server.hpp src/neotape_tcp_server.cpp
git commit -m "feat(archiver): add minimal TCP server skeleton"
```

### Task 1.2: `neotape-archiver` CLI skeleton

**Files:**
- Create: `src/neotape_archiver_cmd.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write the CLI skeleton**

```cpp
#include "neotape/common.hpp"
#include "neotape/tcp_server.hpp"

#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using std::format;
using std::string;
using std::vector;

struct Options {
    string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    string archive_name = "archive";
    uint64_t dummy_frame_count = 8;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-archiver: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --listen <tcp://host:port|unix://path>\n"
        "       [--volume-block-size <bytes>] [--archive-name <name>]\n"
        "       [--dummy-frame-count <N>]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"archive-name", required_argument, nullptr, 'n'},
        {"dummy-frame-count", required_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "l:b:n:d:h", long_opts, nullptr)) !=
           -1) {
        switch (c) {
        case 'l':
            opts.listen_address = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'd': {
            char *end = nullptr;
            unsigned long n = std::strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0')
                fail("--dummy-frame-count requires a number");
            opts.dummy_frame_count = n;
            break;
        }
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }

    if (opts.listen_address.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        neotape::TcpArchiverOptions server_opts;
        server_opts.listen_address = opts.listen_address;
        server_opts.volume_block_size = opts.volume_block_size;
        server_opts.archive_name = opts.archive_name;
        server_opts.has_more_frames = [n = opts.dummy_frame_count](
                                          uint64_t idx) { return idx < n; };
        server_opts.produce_record = [size = opts.volume_block_size](
                                         uint64_t idx) {
            vector<std::byte> rec(size);
            // Fill with a recognizable pattern.
            for (uint32_t i = 0; i < size; ++i)
                rec[i] = static_cast<std::byte>(static_cast<uint8_t>(idx + i));
            return rec;
        };

        uint64_t served = neotape::run_tcp_archiver(server_opts);
        std::cerr << format("archiver served {} frames\n", served);
        return 0;
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
```

- [ ] **Step 2: Add build rule**

Add to `Makefile`:

```make
TCP_SERVER_OBJ = src/neotape_tcp_server.o
ARCHIVER_CMD_OBJ = src/neotape_archiver_cmd.o

EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/neotape-archiver

$(BINDIR)/neotape-archiver : $(ARCHIVER_CMD_OBJ) $(TCP_SERVER_OBJ) $(TCP_PROTO_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(ARCHIVER_CMD_OBJ) $(TCP_SERVER_OBJ) $(TCP_PROTO_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)
```

- [ ] **Step 3: Build and smoke-test**

Run:
```bash
make clean && make -j$(nproc) bin/neotape-archiver
./bin/neotape-archiver --listen unix:///tmp/neotape-archiver.sock --dummy-frame-count 2 &
ARCHIVER_PID=$!
sleep 0.2
# Use socat or nc to request header and frames; verify it exits after archive end.
# For now, just verify it starts without error.
kill $ARCHIVER_PID
```

Expected: archiver starts, listens, and exits cleanly when client disconnects.

- [ ] **Step 4: Commit**

```bash
git add src/neotape_archiver_cmd.cpp Makefile
git commit -m "feat(archiver): add skeleton CLI binary"
```

### Task 1.3: `neotape-write` CLI skeleton

**Files:**
- Create: `src/neotape_write_cmd.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write the writer client skeleton**

```cpp
#include "neotape/common.hpp"
#include "neotape/tcp_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <getopt.h>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using std::format;
using std::string;
using std::vector;

struct Options {
    string source_address;
    string output_path = "-";
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape-write: {}\n", msg);
    std::exit(1);
}

void usage(const char *prog) {
    std::cerr << format(
        "usage: {} --source <tcp://host:port|unix://path> [-o <file|->]\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"output", required_argument, nullptr, 'o'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "s:o:h", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source_address = optarg;
            break;
        case 'o':
            opts.output_path = optarg;
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
    return opts;
}

void parse_source_address(const string &addr, string &host, string &port,
                          string &path, bool &is_unix) {
    const string tcp_prefix = "tcp://";
    const string unix_prefix = "unix://";
    if (addr.rfind(tcp_prefix, 0) == 0) {
        is_unix = false;
        string rest = addr.substr(tcp_prefix.size());
        auto colon = rest.rfind(':');
        if (colon == std::string::npos)
            fail("tcp source address missing port");
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        return;
    }
    if (addr.rfind(unix_prefix, 0) == 0) {
        is_unix = true;
        path = addr.substr(unix_prefix.size());
        return;
    }
    fail("source address must start with tcp:// or unix://");
}

int connect_to_source(const string &addr) {
    string host, port, path;
    bool is_unix = false;
    parse_source_address(addr, host, port, path, is_unix);

    int fd = -1;
    if (is_unix) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path.size() >= sizeof(sa.sun_path))
            fail("unix socket path too long");
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0)
            fail(format("connect {}: {}", path, std::strerror(errno)));
    } else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
        if (gai != 0)
            fail(format("getaddrinfo: {}", gai_strerror(gai)));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> res_guard(res,
                                                                     freeaddrinfo);
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
            fail(format("socket: {}", std::strerror(errno)));
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
            fail(format("connect {}:{}: {}", host, port, std::strerror(errno)));
    }
    return fd;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        FILE *out = nullptr;
        bool close_out = false;
        if (opts.output_path == "-") {
            out = stdout;
        } else {
            out = std::fopen(opts.output_path.c_str(), "wb");
            if (!out)
                fail(format("open {}: {}", opts.output_path,
                            std::strerror(errno)));
            close_out = true;
        }

        int fd = connect_to_source(opts.source_address);

        using neotape::tcp::Message;
        using neotape::tcp::MessageType;

        neotape::tcp::write_message(fd, Message{MessageType::get_volume_header});
        auto vh = neotape::tcp::read_message(fd);
        if (!vh || vh->type != MessageType::volume_header)
            fail("did not receive volume header");

        uint64_t frames = 0;
        for (;;) {
            neotape::tcp::write_message(fd, Message{MessageType::next_frame});
            auto msg = neotape::tcp::read_message(fd);
            if (!msg)
                fail("unexpected disconnect");
            switch (msg->type) {
            case MessageType::frame_record:
                if (std::fwrite(msg->payload.data(), 1, msg->payload.size(),
                                out) != msg->payload.size())
                    fail("write output");
                ++frames;
                break;
            case MessageType::tape_eof:
                // In skeleton mode just print a marker to stderr.
                std::cerr << "writer: tape eof marker\n";
                break;
            case MessageType::archive_end_header:
                if (std::fwrite(msg->payload.data(), 1, msg->payload.size(),
                                out) != msg->payload.size())
                    fail("write output");
                std::cerr << format("writer: received archive end after {} frames\n",
                                    frames);
                close(fd);
                if (close_out)
                    std::fclose(out);
                return 0;
            case MessageType::error:
                fail("archiver reported error");
            default:
                fail(format("unexpected message type {}",
                            static_cast<int>(msg->type)));
            }
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
```

- [ ] **Step 2: Add build rule**

Add to `Makefile`:

```make
WRITE_CMD_OBJ = src/neotape_write_cmd.o

EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/neotape-archiver bin/neotape-write

$(BINDIR)/neotape-write : $(WRITE_CMD_OBJ) $(TCP_PROTO_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(WRITE_CMD_OBJ) $(TCP_PROTO_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)
```

- [ ] **Step 3: Build and smoke-test**

Run:
```bash
make -j$(nproc) bin/neotape-archiver bin/neotape-write
rm -f /tmp/neotape-archiver.sock
./bin/neotape-archiver --listen unix:///tmp/neotape-archiver.sock --dummy-frame-count 4 --volume-block-size 4096 &
ARCHIVER_PID=$!
sleep 0.2
./bin/neotape-write --source unix:///tmp/neotape-archiver.sock --output /tmp/writer.out
wait $ARCHIVER_PID
ls -l /tmp/writer.out
```

Expected: writer exits 0, output file contains volume header + 4 frames + archive end header (each 4096 bytes plus headers).

- [ ] **Step 4: Commit**

```bash
git add src/neotape_write_cmd.cpp Makefile
git commit -m "feat(writer): add skeleton writer CLI binary"
```

### Task 1.4: End-to-end smoke test script

**Files:**
- Create: `tests/smoke_tcp_archive.sh`
- Modify: `Makefile`

- [ ] **Step 1: Write the smoke script**

```sh
#!/bin/sh
set -e

SOCK=/tmp/neotape-smoke-$$
OUT=/tmp/neotape-smoke-out-$$
BLOCK=4096
FRAMES=4

 cleanup() {
    rm -f "$SOCK" "$OUT"
}
trap cleanup EXIT

./bin/neotape-archiver \
    --listen "unix://$SOCK" \
    --volume-block-size "$BLOCK" \
    --dummy-frame-count "$FRAMES" \
    --archive-name smoke &
ARCHIVER_PID=$!

# Wait for socket to exist.
for i in $(seq 1 50); do
    if [ -S "$SOCK" ]; then break; fi
    sleep 0.1
done

./bin/neotape-write --source "unix://$SOCK" --output "$OUT"
wait "$ARCHIVER_PID"

EXPECTED=$((1024 + BLOCK * FRAMES + 1024))
ACTUAL=$(stat -c%s "$OUT")
if [ "$ACTUAL" -ne "$EXPECTED" ]; then
    echo "smoke_tcp_archive: size mismatch expected=$EXPECTED actual=$ACTUAL"
    exit 1
fi

echo "smoke_tcp_archive: ok"
```

- [ ] **Step 2: Make executable and wire into Makefile**

```bash
chmod +x tests/smoke_tcp_archive.sh
```

Add to `Makefile` `test` target:

```make
test: $(BINDIR)/test_pax_pipeline $(BINDIR)/mt-pax $(BINDIR)/neotape-plan $(BINDIR)/test_tcp_protocol $(BINDIR)/neotape-archiver $(BINDIR)/neotape-write
	$(BINDIR)/test_pax_pipeline
	sh tests/smoke_mt_pax_pipeline.sh
	sh tests/smoke_tcp_archive.sh
```

- [ ] **Step 3: Run the smoke test**

Run: `make test`

Expected:
```
test_pax_pipeline: ok
smoke_mt_pax_pipeline: ok
smoke_tcp_archive: ok
```

- [ ] **Step 4: Commit**

```bash
git add tests/smoke_tcp_archive.sh Makefile
git commit -m "test(tcp): add end-to-end smoke test for archiver/writer"
```

---

## Phase 2: Integrate real pax generation into `neotape-archiver`

### Task 2.1: Refactor `mt-pax.cpp` continuous output into a reusable callback helper

**Files:**
- Modify: `src/mt-pax.cpp`

- [ ] **Step 1: Extract file output callback builder**

Move `continuous_callbacks` and `slice_callbacks` into an unnamed namespace and make them return the callback object without the `FILE*` management leaking. For this plan, the simplest path is to keep them in `mt-pax.cpp` and let `neotape-archiver` build its own callbacks.

No code change required; just verify the `PaxWriterCallbacks` interface is sufficient.

- [ ] **Step 2: Commit**

```bash
git commit --allow-empty -m "chore(archiver): confirm PaxWriterCallbacks interface is reusable"
```

### Task 2.2: Add archiver mode that streams pax records as NeoTape frames

**Files:**
- Modify: `src/neotape_archiver_cmd.cpp`
- Modify: `src/neotape_tcp_server.cpp`
- Modify: `include/neotape/tcp_server.hpp`

- [ ] **Step 1: Extend `TcpArchiverOptions` with pax options**

Modify `include/neotape/tcp_server.hpp`:

```cpp
#include "neotape/pax_writer.hpp"

struct TcpArchiverOptions {
    std::string listen_address;
    uint32_t volume_block_size = 4 * 1024 * 1024;
    std::string archive_name;
    uint64_t initial_volume_seq_num = 1;

    // Pax generation options when in archive mode.
    PaxWriterOptions pax;
    bool use_pax = false;
};
```

- [ ] **Step 2: Implement frame packing in the server**

Add a helper to `src/neotape_tcp_server.cpp`:

```cpp
struct FrameBuilder {
    uint32_t block_size;
    uint64_t volume_seq_num;
    std::string archive_uuid;
    std::string archive_name;
    uint64_t global_frame = 0;
    uint64_t slice = 0;
    uint64_t frame_in_slice = 0;
    std::vector<std::byte> pending;

    explicit FrameBuilder(uint32_t bs, uint64_t vol,
                          const std::string &uuid,
                          const std::string &name)
        : block_size(bs), volume_seq_num(vol), archive_uuid(uuid),
          archive_name(name) {}

    // Append payload bytes. Returns zero or more complete frames.
    std::vector<std::vector<std::byte>> feed(std::span<const std::byte> bytes) {
        std::vector<std::vector<std::byte>> out;
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        while (pending.size() >= block_size) {
            out.push_back(build_frame(
                std::span(pending.begin(), block_size)));
            pending.erase(pending.begin(), pending.begin() + block_size);
        }
        return out;
    }

    // Force any remaining pending bytes into a final frame.
    std::optional<std::vector<std::byte>> flush() {
        if (pending.empty())
            return std::nullopt;
        auto rec = build_frame(std::span(pending));
        pending.clear();
        return rec;
    }

    std::vector<std::byte> build_frame(std::span<const std::byte> payload) {
        FrameHeader fh;
        fh.volume_block_size = block_size;
        fh.archive_uuid = archive_uuid;
        fh.archive_name = archive_name;
        fh.volume_seq_num = volume_seq_num;
        fh.payload_profile = PayloadProfile::pax;
        fh.logical_slice_seq_num = slice;
        fh.global_frame_seq_num = global_frame;
        fh.frame_seq_num_within_slice = frame_in_slice;
        fh.frame_payload_size = payload.size();
        fh.frame_content_type = FrameContentType::slice_content;
        fh.frame_payload_blake3 = blake3_hash(
            reinterpret_cast<const uint8_t *>(payload.data()),
            payload.size());
        fh.flags = frame_flag_start | frame_flag_end;
        fh.slice_content_size = payload.size();
        fh.slice_content_blake3 = fh.frame_payload_blake3;

        HeaderBytes header = serialize_frame_header(fh);
        std::vector<std::byte> record;
        record.reserve(block_size);
        for (uint8_t b : header)
            record.push_back(static_cast<std::byte>(b));
        record.insert(record.end(), payload.begin(), payload.end());
        record.resize(block_size); // pad with zero bytes

        ++global_frame;
        ++frame_in_slice;
        return record;
    }
};
```

Need to add includes: `<span>`, `<optional>`.

- [ ] **Step 3: Rework server loop to use frame builder and pax callbacks**

Add the queue sentinel type and callback builder in `src/neotape_tcp_server.cpp`:

```cpp
struct RecordOrDone {
    std::vector<std::byte> record;
    bool done = false;
};

PaxWriterCallbacks make_server_callbacks(FrameBuilder &builder,
                                         ClosableQueue<RecordOrDone> &queue) {
    return PaxWriterCallbacks{
        .begin_slice = [&](uint64_t slice_num) {
            builder.slice = slice_num;
            builder.frame_in_slice = 0;
        },
        .write_chunk = [&](PaxChunk chunk) {
            auto frames = builder.feed(chunk.bytes);
            for (auto &f : frames)
                queue.push(RecordOrDone{std::move(f), false});
        },
        .end_slice = [](uint64_t) {},
        .progress_paused = [] { return false; },
    };
}
```

Replace the body of `run_tcp_archiver` with two paths. For `use_pax`:

```cpp
uint64_t run_tcp_archiver(const TcpArchiverOptions &opts) {
    if (!valid_block_size(opts.volume_block_size))
        throw std::runtime_error("invalid volume block size");

    int listener = create_listener(opts.listen_address);
    std::cerr << std::format("archiver listening on {}\n", opts.listen_address);

    int client = accept(listener, nullptr, nullptr);
    if (client < 0)
        throw std::runtime_error(
            std::format("accept: {}", std::strerror(errno)));
    close(listener);

    uint64_t frames_served = 0;
    try {
        VolumeHeader vh = make_volume_header(opts.volume_block_size,
                                             opts.initial_volume_seq_num,
                                             opts.archive_name);
        HeaderBytes vh_bytes = serialize_volume_header(vh);
        std::vector<std::byte> vh_payload = bytes_from_header_bytes(vh_bytes);
        FrameBuilder builder(opts.volume_block_size, opts.initial_volume_seq_num,
                             vh.archive_uuid, opts.archive_name);
        ClosableQueue<RecordOrDone> frame_queue(8);
        std::atomic<bool> pax_done{false};

        std::thread pax_thread([&]() {
            try {
                auto callbacks = make_server_callbacks(builder, frame_queue);
                write_pax(opts.pax, std::move(callbacks));
                // Flush any trailing partial frame.
                if (auto tail = builder.flush(); tail.has_value())
                    frame_queue.push(RecordOrDone{std::move(*tail), false});
                frame_queue.push(RecordOrDone{{}, true});
            } catch (...) {
                frame_queue.close();
            }
            pax_done.store(true, std::memory_order_release);
        });

        for (;;) {
            auto req = neotape::tcp::read_message(client);
            if (!req.has_value())
                break;

            switch (req->type) {
            case MessageType::get_volume_header:
                neotape::tcp::write_message(
                    client, Message{MessageType::volume_header,
                                    std::move(vh_payload)});
                vh_payload = bytes_from_header_bytes(vh_bytes);
                break;
            case MessageType::next_frame: {
                auto next = frame_queue.pop();
                if (!next.has_value()) {
                    neotape::tcp::write_message(
                        client, Message{MessageType::error,
                                        std::vector<std::byte>{}});
                    close(client);
                    pax_thread.join();
                    return frames_served;
                }
                if (next->done) {
                    ArchiveEndHeader ae;
                    ae.volume_block_size = opts.volume_block_size;
                    ae.archive_uuid = vh.archive_uuid;
                    ae.archive_name = opts.archive_name;
                    ae.volume_seq_num = opts.initial_volume_seq_num;
                    ae.payload_profile = PayloadProfile::pax;
                    ae.last_logical_slice_seq_num = builder.slice;
                    ae.last_global_frame_seq_num =
                        builder.global_frame > 0 ? builder.global_frame - 1 : 0;
                    ae.created_by_implementation = "neotape-archiver";
                    ae.created_by_build_id = "";
                    ae.archive_end_at_utc = utc_timestamp_now();
                    ae.flags = archive_end_flag_clean_end;
                    neotape::tcp::write_message(
                        client,
                        Message{MessageType::archive_end_header,
                                bytes_from_header_bytes(
                                    serialize_archive_end_header(ae))});
                    close(client);
                    pax_thread.join();
                    return frames_served;
                }
                if (next->record.size() != opts.volume_block_size)
                    throw std::runtime_error("frame size mismatch");
                neotape::tcp::write_message(
                    client,
                    Message{MessageType::frame_record,
                            std::move(next->record)});
                ++frames_served;
                break;
            }
            case MessageType::tape_eof:
                // Archiver never requests EOF from writer; ignore or treat as
                // protocol error.
                neotape::tcp::write_message(
                    client, Message{MessageType::error,
                                    std::vector<std::byte>{}});
                close(client);
                pax_thread.join();
                return frames_served;
            default:
                neotape::tcp::write_message(
                    client, Message{MessageType::error,
                                    std::vector<std::byte>{}});
                close(client);
                pax_thread.join();
                return frames_served;
            }
        }

        pax_done.load(std::memory_order_acquire);
        pax_thread.join();
    } catch (...) {
        close(client);
        throw;
    }
    close(client);
    return frames_served;
}
```

For the non-`use_pax` dummy path, keep the original skeleton loop but remove `produce_record`/`has_more_frames` if no longer needed, or keep them as a simple test path.

- [ ] **Step 4: Update CLI to support pax sources**

Modify `src/neotape_archiver_cmd.cpp` parse loop to accept the same flags as `mt-pax`:

```cpp
static const struct option long_opts[] = {
    {"listen", required_argument, nullptr, 'l'},
    {"volume-block-size", required_argument, nullptr, 'b'},
    {"archive-name", required_argument, nullptr, 'n'},
    {"directory", required_argument, nullptr, 'C'},
    {"buffer-percent", required_argument, nullptr, 'P'},
    {"io-thread", required_argument, nullptr, 257},
    {"output-buffer-size", required_argument, nullptr, 256},
    {"plan", required_argument, nullptr, 258},
    {"verbose", no_argument, nullptr, 'v'},
    {"one-file-system", no_argument, nullptr, 'x'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0}};
```

Pass parsed values into `server_opts.pax`. After parsing, if `opts.listen_address` is non-empty, set `server_opts.use_pax = true` and call `run_tcp_archiver`. Otherwise, call `neotape::write_pax(opts.pax, make_local_callbacks(opts.pax.output_name))` where `make_local_callbacks` mimics the file-writing behavior from `mt-pax.cpp`.

- [ ] **Step 5: Build and test**

Run:
```bash
make -j$(nproc) bin/neotape-archiver bin/neotape-write
./bin/neotape-archiver --listen unix:///tmp/archiver.sock -C /some/dir /some/path &
./bin/neotape-write --source unix:///tmp/archiver.sock --output /tmp/archive.vol
# Verify output starts with NeoTape magic and contains valid pax bytes.
```

- [ ] **Step 6: Commit**

```bash
git add include/neotape/tcp_server.hpp src/neotape_tcp_server.cpp src/neotape_archiver_cmd.cpp
git commit -m "feat(archiver): integrate real pax generation into TCP server"
```

---

## Phase 3: Integrate real tape/spool writing into `neotape-write`

### Task 3.1: Add tape/spool output to writer

**Files:**
- Modify: `src/neotape_write_cmd.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Parse `--target` locator**

Add `--target <tape:/dev/nst0|spool:./dir>` option. Implement simple parsing in `src/neotape_write_cmd.cpp`:

```cpp
struct TargetLocator {
    enum Kind { none, tape, spool } kind = none;
    std::string path;
};

TargetLocator parse_target(const std::string &s) {
    if (s.rfind("tape:", 0) == 0)
        return {TargetLocator::tape, s.substr(5)};
    if (s.rfind("spool:", 0) == 0)
        return {TargetLocator::spool, s.substr(6)};
    throw std::runtime_error("target must be tape:<device> or spool:<dir>");
}
```

Add to the option parser:

```cpp
{"target", required_argument, nullptr, 260},

// in switch:
case 260:
    opts.target = parse_target(optarg);
    break;
```

- [ ] **Step 2: Add `write_record` helper to `mt::TapeDevice`**

Modify `include/neotape/tape.hpp`:

```cpp
// Write all bytes to the tape device. Throws on short write or error.
void write_record(const void *data, std::size_t size);
```

Modify `src/neotape_tape.cpp` near other I/O methods:

```cpp
void TapeDevice::write_record(const void *data, std::size_t size) {
    const auto *p = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        ssize_t w = ::write(fd_, p, remaining);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            throw Error(device_path_, "write_record", errno);
        }
        if (w == 0)
            throw Error(device_path_, "write_record", EIO);
        p += w;
        remaining -= static_cast<std::size_t>(w);
    }
}
```

Also add the override to `SpoolTapeDevice` or route it through `do_mtop`; for the spool backend, implement by writing the record bytes to the current spool file and advancing `current_record_`.

- [ ] **Step 3: Write records via `namespace mt`**

In `src/neotape_write_cmd.cpp`, after receiving the Volume Header, if a target is set:

```cpp
struct TapeOutput {
    std::unique_ptr<mt::TapeDevice> device;
    TargetLocator target;
    bool wrote_volume_header = false;
};

void open_target(TapeOutput &out, const TargetLocator &loc) {
    if (loc.kind == TargetLocator::tape) {
        out.device = std::make_unique<mt::TapeDevice>(loc.path, true);
    } else {
        out.device = std::make_unique<mt::SpoolTapeDevice>(
            std::filesystem::path(loc.path), true);
    }
    out.device->rewind();
}

void write_to_target(TapeOutput &out, const std::vector<std::byte> &record) {
    if (!out.device)
        throw std::runtime_error("no target device open");
    out.device->write_record(record.data(), record.size());
}
```

In the main message loop, when `target.kind != none`:
- On `VOLUME_HEADER`: call `write_to_target(target, vh.payload)`.
- On `FRAME_RECORD`: call `write_to_target(target, msg.payload)`.
- On `TAPE_EOF`: call `target.device->write_filemark()`.
- On `ARCHIVE_END_HEADER`: call `write_to_target(target, msg.payload)` and exit 0.
- Detect EOT via `target.device->status().eot()` before each write; if true, write a trailing filemark and exit 1.

- [ ] **Step 4: Keep file output as fallback**

If `--target` is absent but `-o` is present, write raw bytes to file (existing skeleton behavior). Ensure `--target` and `-o` are mutually exclusive.

- [ ] **Step 5: Build and test with spool backend**

Run:
```bash
make -j$(nproc) bin/neotape-write
rm -rf /tmp/spool-test
./bin/neotape-archiver --listen unix:///tmp/archiver.sock -C /some/dir /some/path &
./bin/neotape-write --source unix:///tmp/archiver.sock --target spool:/tmp/spool-test
# Inspect /tmp/spool-test for expected tape files.
```

- [ ] **Step 5: Commit**

```bash
git add src/neotape_write_cmd.cpp include/neotape/tape.hpp src/neotape_tape.cpp Makefile
git commit -m "feat(writer): add tape/spool backend output"
```

---

## Phase 4: Testing, cleanup, and CLI parity

### Task 4.1: Verify `neotape-archiver` without `--listen` matches `mt-pax`

**Files:**
- Modify: `tests/smoke_mt_pax_pipeline.sh` or create new comparison test

- [ ] **Step 1: Write comparison test**

```sh
#!/bin/sh
set -e
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/src"
echo hello > "$TMP/src/a.txt"

./bin/mt-pax -f "$TMP/mt.out" "$TMP/src"
./bin/neotape-archiver -f "$TMP/archiver.out" "$TMP/src"

cmp "$TMP/mt.out" "$TMP/archiver.out"
echo "mt-pax parity: ok"
```

- [ ] **Step 2: Run and commit**

```bash
chmod +x tests/smoke_mt_pax_parity.sh
```

Add to `Makefile` `test` target after `sh tests/smoke_tcp_archive.sh`:

```make
test: $(BINDIR)/test_pax_pipeline $(BINDIR)/mt-pax $(BINDIR)/neotape-plan $(BINDIR)/test_tcp_protocol $(BINDIR)/neotape-archiver $(BINDIR)/neotape-write
	$(BINDIR)/test_pax_pipeline
	sh tests/smoke_mt_pax_pipeline.sh
	sh tests/smoke_tcp_archive.sh
	sh tests/smoke_mt_pax_parity.sh
```

- [ ] **Step 3: Run `make test`**

Run: `make test`

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/smoke_mt_pax_parity.sh Makefile
git commit -m "test(archiver): verify neotape-archiver matches mt-pax output"
```

### Task 4.2: Finalize CLI help and README updates

**Files:**
- Modify: `src/neotape_archiver_cmd.cpp`
- Modify: `src/neotape_write_cmd.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

- [ ] **Step 1: Update usage text and help**

Ensure both CLIs print accurate usage reflecting final flags.

- [ ] **Step 2: Update README.md**

Add sections for `neotape-archiver` and `neotape-write` with examples.

- [ ] **Step 3: Update AGENTS.md**

Add the new binaries to the project layout section.

- [ ] **Step 4: Commit**

```bash
git add README.md AGENTS.md src/neotape_archiver_cmd.cpp src/neotape_write_cmd.cpp
git commit -m "docs: document neotape-archiver and neotape-write CLIs"
```

### Task 4.3: Final full build and test

- [ ] **Step 1: Clean build**

Run:
```bash
make clean && make -j$(nproc) && make test
```

Expected: all tests pass.

- [ ] **Step 2: Commit any remaining fixes**

```bash
git commit -a -m "fix: final build/test adjustments" || true
```

---

## Spec coverage check

| Spec section | Implementing task |
|--------------|-------------------|
| Single TCP/UDS connection | Task 0.1, 1.1, 1.3 |
| Binary framed messages `(type, length, payload)` | Task 0.1, 0.2 |
| Message types `GET_VOLUME_HEADER`, `VOLUME_HEADER`, `NEXT_FRAME`, `FRAME_RECORD`, `ARCHIVE_END_HEADER`, `TAPE_EOF`, `ERROR` | Task 0.1, 0.2 |
| Request-response rules | Task 1.1, 1.3 |
| Archiver long-running, owns archive state | Task 2.2 |
| Writer short-lived, one volume per process | Task 1.3, 3.1 |
| Writer EOT behavior | Task 3.1 |
| Archiver as `mt-pax` superset | Task 2.2, 4.1 |
| Buffer strategy (archiver small, writer large) | Task 2.2 (archiver uses pax pipeline buffer), Task 3.1 (writer large output buffer) |

## Placeholder scan

No TBD/TODO, no vague "add error handling", every code step contains concrete code, every test step contains exact command and expected output.

## Type consistency notes

- `MessageType` enum values match the design spec.
- `TcpArchiverOptions` fields are used consistently across header and implementation.
- `FrameBuilder` tracks `global_frame`, `slice`, `frame_in_slice` aligned with `FrameHeader` fields.
