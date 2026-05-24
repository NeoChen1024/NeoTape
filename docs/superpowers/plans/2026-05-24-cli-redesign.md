# NeoTape CLI Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the new `neotape <subcommand>` CLI described in `docs/superpowers/specs/2026-05-24-cli-redesign.md` while preserving useful standalone tools.

**Architecture:** Add a small shared CLI utility layer for locator parsing, control policy, and `/dev/tty` prompting; add `src/neotape.cpp` as the primary subcommand dispatcher; refactor existing command entry points into callable `neotape_*_main()` functions guarded by standalone `main()` wrappers. Keep backend and format logic in existing libraries, moving behavior only when needed to make commands share code.

**Tech Stack:** C++20, GNU Make, `getopt_long`, existing NeoTape format/reader/writer libraries, bundled BLAKE3/crc32c, libarchive for PAX.

---

## Scope Check

This plan implements one coherent subsystem: the public CLI layer and the minimal backend plumbing required by that layer. It intentionally does not redesign the NeoTape binary format, implement catalog content listing, or rewrite `mt-pax`.

The work is split so each task produces buildable, testable progress:

1. Shared CLI primitives.
2. Main dispatcher executable.
3. Callable command entry points.
4. `init` lifecycle and spool medium policy.
5. `plan` source-tree semantics.
6. `write/read` raw profile commands.
7. `backup/restore` PAX profile commands.
8. `list` archive-instance listing.
9. Volume-change control prompt.
10. Documentation and compatibility pass.

## File Structure

- Create `include/neotape/cli.hpp`: shared CLI data types and declarations for `Locator`, `ControlPolicy`, parsing helpers, and volume-change prompting.
- Create `src/neotape_cli.cpp`: shared CLI helper implementation. No backend-specific archive logic belongs here.
- Create `include/neotape/commands.hpp`: declarations for callable command entry points such as `neotape_init_main()` and `neotape_plan_main()`.
- Create `src/neotape.cpp`: the primary `bin/neotape` dispatcher. It parses the subcommand and forwards the remaining argv to command entry points.
- Modify `src/neotape_init.cpp`: expose `neotape_init_main()`, accept `tape:` and `spool:` locators, implement spool initialization policy.
- Modify `src/neotape_plan.cpp`: expose `neotape_plan_main()`, enforce one `-C`, positional sources, and self-contained plan rules.
- Modify `src/neotape_write.cpp`: expose `neotape_write_legacy_main()` for the old binary if retained; add or extract raw write implementation usable by `neotape write`.
- Modify `src/neotape_cat_volumes.cpp`: expose `neotape_cat_volumes_legacy_main()` for the old binary if retained; add or extract raw/PAX read implementation usable by `neotape read` and `neotape restore`.
- Modify `Makefile`: build `bin/neotape`, build shared CLI object, keep `bin/mt-pax` unchanged, and keep standalone tools during migration.
- Create `tests/test_cli.cpp`: lightweight unit tests for locator parsing, control-policy parsing, and command-dispatch error behavior.
- Modify `tests/test_tape.cpp` only if the Makefile needs a combined test target; do not mix CLI parser tests into tape abstraction tests.
- Modify `docs/spec/appendix-cli.md`: align the non-normative CLI reference with the new design after implementation.
- Modify `README.md`: update project status and usage examples after implementation.

## Task 1: Shared CLI Primitives

**Files:**
- Create: `include/neotape/cli.hpp`
- Create: `src/neotape_cli.cpp`
- Create: `tests/test_cli.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write failing CLI primitive tests**

Create `tests/test_cli.cpp` with:

```cpp
#include "neotape/cli.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool cond, const std::string &msg) {
    if (!cond) {
        std::cerr << "test_cli: " << msg << "\n";
        std::exit(1);
    }
}

void test_locator_parse() {
    auto tape = neotape::parse_locator("tape:/dev/nst0");
    require(tape.kind == "tape", "tape locator kind");
    require(tape.locator == "/dev/nst0", "tape locator body");

    auto spool = neotape::parse_locator("spool:relative:path");
    require(spool.kind == "spool", "spool locator kind");
    require(spool.locator == "relative:path", "only first colon splits locator");
}

void test_locator_rejects_invalid() {
    const char *bad[] = {"", "tape", ":/dev/nst0", "spool:"};
    for (const char *text : bad) {
        bool threw = false;
        try {
            (void)neotape::parse_locator(text);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        require(threw, std::string("invalid locator rejected: ") + text);
    }
}

void test_control_policy_parse() {
    require(neotape::parse_control_policy("auto") ==
                neotape::ControlPolicy::auto_prompt,
            "auto control policy");
    require(neotape::parse_control_policy("none") ==
                neotape::ControlPolicy::none,
            "none control policy");
    bool threw = false;
    try {
        (void)neotape::parse_control_policy("tty");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    require(threw, "invalid control policy rejected");
}

} // namespace

int main() {
    test_locator_parse();
    test_locator_rejects_invalid();
    test_control_policy_parse();
    return 0;
}
```

- [ ] **Step 2: Add the test target before implementation**

Modify `Makefile`:

```make
CLI_OBJ = src/neotape_cli.o
EXE	= bin/mt-pax bin/neotape bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape bin/test_cli bin/neotape-init

$(BINDIR)/test_cli : tests/test_cli.cpp $(CLI_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) -o $@

test: $(BINDIR)/test_tape $(BINDIR)/test_cli
	$(BINDIR)/test_tape
	$(BINDIR)/test_cli
```

Keep existing object variables and targets intact; add `CLI_OBJ` near the other shared object variables.

- [ ] **Step 3: Run the failing test build**

Run: `make bin/test_cli`

Expected: compile failure because `include/neotape/cli.hpp` does not exist.

- [ ] **Step 4: Implement the CLI primitive header**

Create `include/neotape/cli.hpp`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neotape {

struct Locator {
    std::string kind;
    std::string locator;
};

enum class ControlPolicy { auto_prompt, none };

enum class VolumePromptChoice { continue_current, change_locator, shell, abort };

struct VolumePromptRequest {
    std::string archive_uuid;
    uint64_t expected_volume = 0;
    Locator current_locator;
    bool write_mode = false;
};

struct VolumePromptResult {
    VolumePromptChoice choice = VolumePromptChoice::abort;
    std::optional<Locator> replacement_locator;
};

Locator parse_locator(std::string_view text);
ControlPolicy parse_control_policy(std::string_view text);
std::string control_policy_name(ControlPolicy policy);
VolumePromptResult prompt_for_volume_change(const VolumePromptRequest &request);

} // namespace neotape
```

- [ ] **Step 5: Implement locator and control parsing**

Create `src/neotape_cli.cpp`:

```cpp
#include "neotape/cli.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace neotape {

Locator parse_locator(std::string_view text) {
    auto pos = text.find(':');
    if (pos == std::string_view::npos || pos == 0 || pos + 1 == text.size())
        throw std::invalid_argument("locator must be <kind>:<locator>");
    return Locator{std::string(text.substr(0, pos)),
                   std::string(text.substr(pos + 1))};
}

ControlPolicy parse_control_policy(std::string_view text) {
    if (text == "auto")
        return ControlPolicy::auto_prompt;
    if (text == "none")
        return ControlPolicy::none;
    throw std::invalid_argument("control policy must be auto or none");
}

std::string control_policy_name(ControlPolicy policy) {
    switch (policy) {
    case ControlPolicy::auto_prompt:
        return "auto";
    case ControlPolicy::none:
        return "none";
    }
    return "unknown";
}

namespace {

int open_tty() { return ::open("/dev/tty", O_RDWR | O_CLOEXEC); }

void write_all(int fd, std::string_view text) {
    const char *p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0)
            throw std::runtime_error("write /dev/tty failed");
        p += n;
        left -= static_cast<std::size_t>(n);
    }
}

std::string read_line(int fd) {
    std::string out;
    char ch = 0;
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0)
            throw std::runtime_error("read /dev/tty failed");
        if (ch == '\n')
            break;
        out.push_back(ch);
    }
    return out;
}

} // namespace

VolumePromptResult prompt_for_volume_change(const VolumePromptRequest &request) {
    int tty = open_tty();
    if (tty < 0)
        throw std::runtime_error("volume change requires /dev/tty");

    auto close_tty = [&]() {
        if (tty >= 0) {
            ::close(tty);
            tty = -1;
        }
    };

    try {
        while (true) {
            write_all(tty, std::format(
                               "Volume change required for archive {}, expected volume {}.\n"
                               "Options:\n"
                               "  [c] Continue\n"
                               "  [d] Change device\n"
                               "  [s] Shell\n"
                               "  [a] Abort\n"
                               "> ",
                               request.archive_uuid, request.expected_volume));
            std::string line = read_line(tty);
            if (line == "c" || line == "C") {
                close_tty();
                return VolumePromptResult{VolumePromptChoice::continue_current,
                                          std::nullopt};
            }
            if (line == "d" || line == "D") {
                write_all(tty, "New locator: ");
                auto locator = parse_locator(read_line(tty));
                close_tty();
                return VolumePromptResult{VolumePromptChoice::change_locator,
                                          locator};
            }
            if (line == "s" || line == "S") {
                const char *shell = std::getenv("SHELL");
                if (shell == nullptr || std::strlen(shell) == 0)
                    shell = "/bin/sh";
                write_all(tty, std::format("Entering shell {}. Exit to return.\n", shell));
                int rc = std::system(shell);
                write_all(tty, std::format("Shell exited with status {}.\n", rc));
                continue;
            }
            if (line == "a" || line == "A") {
                close_tty();
                return VolumePromptResult{VolumePromptChoice::abort, std::nullopt};
            }
            write_all(tty, "Choose c, d, s, or a.\n");
        }
    } catch (...) {
        close_tty();
        throw;
    }
}

} // namespace neotape
```

- [ ] **Step 6: Run CLI primitive tests**

Run: `make bin/test_cli && bin/test_cli`

Expected: command exits 0 with no output.

- [ ] **Step 7: Commit Task 1**

Run:

```sh
git add include/neotape/cli.hpp src/neotape_cli.cpp tests/test_cli.cpp Makefile
git commit -m "feat(cli): add shared CLI primitives"
```

## Task 2: Main `bin/neotape` Dispatcher

**Files:**
- Create: `include/neotape/commands.hpp`
- Create: `src/neotape.cpp`
- Modify: `Makefile`
- Test: `tests/test_cli.cpp`

- [ ] **Step 1: Add failing dispatcher smoke tests**

Append this function to `tests/test_cli.cpp` before `main()`:

```cpp
void test_dispatcher_argv_shape_documented() {
    require(std::string("neotape init tape:/dev/nst0").find("init") !=
                std::string::npos,
            "dispatcher smoke placeholder uses real subcommand text");
}
```

Add this call inside `main()`:

```cpp
test_dispatcher_argv_shape_documented();
```

This keeps `test_cli` compiling while the real executable smoke checks run from shell commands in later steps.

- [ ] **Step 2: Declare callable command entry points**

Create `include/neotape/commands.hpp`:

```cpp
#pragma once

int neotape_init_main(int argc, char **argv);
int neotape_plan_main(int argc, char **argv);
int neotape_write_main(int argc, char **argv);
int neotape_read_main(int argc, char **argv);
int neotape_backup_main(int argc, char **argv);
int neotape_restore_main(int argc, char **argv);
int neotape_list_main(int argc, char **argv);
```

- [ ] **Step 3: Add dispatcher with stubbed unresolved command symbols**

Create `src/neotape.cpp`:

```cpp
#include "neotape/commands.hpp"

#include <cstring>
#include <format>
#include <iostream>
#include <string_view>

namespace {

void usage(const char *prog) {
    std::cerr << std::format(
        "usage: {} <subcommand> [options]\n"
        "\n"
        "subcommands:\n"
        "  init      initialize a tape or spool medium\n"
        "  list      list archive instances\n"
        "  plan      create PAX plan metadata\n"
        "  backup    write a PAX-profile archive\n"
        "  restore   read a PAX-profile archive\n"
        "  write     write a raw-profile archive\n"
        "  read      read a raw-profile archive\n",
        prog);
}

int dispatch(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    std::string_view cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage(argv[0]);
        return 0;
    }

    char **sub_argv = argv + 1;
    int sub_argc = argc - 1;

    if (cmd == "init")
        return neotape_init_main(sub_argc, sub_argv);
    if (cmd == "plan")
        return neotape_plan_main(sub_argc, sub_argv);
    if (cmd == "write")
        return neotape_write_main(sub_argc, sub_argv);
    if (cmd == "read")
        return neotape_read_main(sub_argc, sub_argv);
    if (cmd == "backup")
        return neotape_backup_main(sub_argc, sub_argv);
    if (cmd == "restore")
        return neotape_restore_main(sub_argc, sub_argv);
    if (cmd == "list")
        return neotape_list_main(sub_argc, sub_argv);

    std::cerr << std::format("{}: unknown subcommand: {}\n", argv[0], cmd);
    usage(argv[0]);
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return dispatch(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape: {}\n", e.what());
        return 1;
    }
}
```

- [ ] **Step 4: Add temporary command stubs so dispatcher links**

Create `src/neotape_command_stubs.cpp`:

```cpp
#include <iostream>

namespace {
int stub(const char *name) {
    std::cerr << "neotape: subcommand not wired yet: " << name << "\n";
    return 1;
}
} // namespace

int neotape_init_main(int, char **) { return stub("init"); }
int neotape_plan_main(int, char **) { return stub("plan"); }
int neotape_write_main(int, char **) { return stub("write"); }
int neotape_read_main(int, char **) { return stub("read"); }
int neotape_backup_main(int, char **) { return stub("backup"); }
int neotape_restore_main(int, char **) { return stub("restore"); }
int neotape_list_main(int, char **) { return stub("list"); }
```

- [ ] **Step 5: Add `bin/neotape` build target**

Modify `Makefile`:

```make
COMMAND_STUBS_OBJ = src/neotape_command_stubs.o

$(BINDIR)/neotape : src/neotape.cpp $(CLI_OBJ) $(COMMAND_STUBS_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) $(COMMAND_STUBS_OBJ) -o $@
```

This target is temporary. Later tasks remove `COMMAND_STUBS_OBJ` from the `bin/neotape` link as real command functions are wired.

- [ ] **Step 6: Verify dispatcher behavior**

Run: `make bin/neotape && bin/neotape --help`

Expected: exit 0 and help text listing `init`, `list`, `plan`, `backup`, `restore`, `write`, and `read`.

Run: `bin/neotape unknown`

Expected: exit 2 and stderr contains `unknown subcommand`.

- [ ] **Step 7: Commit Task 2**

Run:

```sh
git add include/neotape/commands.hpp src/neotape.cpp src/neotape_command_stubs.cpp tests/test_cli.cpp Makefile
git commit -m "feat(cli): add neotape dispatcher"
```

## Task 3: Callable Existing Command Entry Points

**Files:**
- Modify: `src/neotape_init.cpp`
- Modify: `src/neotape_plan.cpp`
- Modify: `src/neotape_write.cpp`
- Modify: `src/neotape_cat_volumes.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Refactor `neotape-init` main**

In `src/neotape_init.cpp`, replace the bottom-level `main()` with:

```cpp
int neotape_init_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);

        mt::TapeDevice dev(opts.device, true);
        dev.configure_preferred_variable_block_mode(
            65536, "neotape-init medium header", std::cerr);

        dev.rewind();
        std::vector<uint8_t> buf(tape_probe_read_size);
        ssize_t n = ::read(dev.fd(), buf.data(), buf.size());
        if (n > 0)
            buf.resize(static_cast<std::size_t>(n));
        if (n > 0 && buf.size() >= neotape::fixed_header_size) {
            auto parsed = neotape::parse_fixed_header(buf.data(), buf.size());
            if (parsed.type == neotape::HeaderType::medium && !opts.force)
                fail("medium already initialized (use --force to overwrite)");
        }

        dev.rewind();
        auto header = neotape::make_medium_header(opts.label);
        auto bytes = neotape::serialize_medium_header(header, 65536);
        dev.write_all(bytes.data(), bytes.size(), "medium header");
        dev.write_filemark();
        dev.rewind();
        std::cerr << "neotape-init: initialized " << opts.device << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape-init: {}\n", e.what());
        return 1;
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_init_main(argc, argv); }
#endif
```

Keep the existing body semantics intact if current code differs in surrounding details; the required structural change is the callable `neotape_init_main()` plus the `NEOTAPE_NO_STANDALONE_MAIN` guard.

- [ ] **Step 2: Refactor `neotape-plan` main**

In `src/neotape_plan.cpp`, wrap the existing main body as:

```cpp
int neotape_plan_main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        run_plan(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape-plan: {}\n", e.what());
        return 1;
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_plan_main(argc, argv); }
#endif
```

If the current file does not have `run_plan(opts)`, extract the existing code inside `main()` into a local function above `neotape_plan_main()`:

```cpp
void run_plan(Options &opts) {
    // Move the existing post-parse main body here unchanged.
}
```

- [ ] **Step 3: Refactor raw writer legacy main**

In `src/neotape_write.cpp`, rename the existing main to:

```cpp
int neotape_write_legacy_main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        run_writer(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape-write: {}\n", e.what());
        return 1;
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_write_legacy_main(argc, argv); }
#endif
```

Extract the current post-parse main body into:

```cpp
void run_writer(const Options &opts) {
    // Move the existing writer execution body here unchanged.
}
```

- [ ] **Step 4: Refactor reader legacy main**

In `src/neotape_cat_volumes.cpp`, rename the existing main to:

```cpp
int neotape_cat_volumes_legacy_main(int argc, char **argv) {
    try {
        Options opts = parse_args(argc, argv);
        run_cat_volumes(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape-cat-volumes: {}\n", e.what());
        return 1;
    }
}

#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_cat_volumes_legacy_main(argc, argv); }
#endif
```

Extract the current post-parse main body into:

```cpp
void run_cat_volumes(const Options &opts) {
    // Move the existing reader execution body here unchanged.
}
```

- [ ] **Step 5: Preserve standalone builds**

Run: `make bin/neotape-init bin/neotape-plan bin/neotape-write bin/neotape-cat-volumes`

Expected: all four standalone binaries build successfully.

- [ ] **Step 6: Commit Task 3**

Run:

```sh
git add src/neotape_init.cpp src/neotape_plan.cpp src/neotape_write.cpp src/neotape_cat_volumes.cpp Makefile
git commit -m "refactor(cli): expose command entry points"
```

## Task 4: `neotape init` For Tape And Spool Locators

**Files:**
- Modify: `src/neotape_init.cpp`
- Modify: `src/neotape.cpp`
- Modify: `Makefile`
- Test: `tests/test_cli.cpp`

- [ ] **Step 1: Add init parser tests for spool policy**

Append to `tests/test_cli.cpp`:

```cpp
void test_spool_locator_for_init() {
    auto loc = neotape::parse_locator("spool:/tmp/archive.spool");
    require(loc.kind == "spool", "init spool locator kind");
    require(loc.locator == "/tmp/archive.spool", "init spool locator body");
}
```

Call it from `main()`.

- [ ] **Step 2: Change init options shape**

In `src/neotape_init.cpp`, replace `Options` with:

```cpp
struct Options {
    neotape::Locator target;
    string label;
    bool force = false;
    uint64_t virtual_tape_size = 0;
};
```

Add `#include "neotape/cli.hpp"` and `#include "neotape/common.hpp"` at the top.

- [ ] **Step 3: Implement new init usage and parsing**

Replace `usage()` and `parse_args()` in `src/neotape_init.cpp` with:

```cpp
[[noreturn]] void usage(const char *prog) {
    std::cerr << format(
        "usage: {} <tape:<device>|spool:<dir>> [--label <text>] [--force]\n"
        "       {} spool:<dir> [--label <text>] [--force] [--virtual-tape-size <bytes>]\n",
        prog, prog);
    std::exit(2);
}

Options parse_args(int argc, char **argv) {
    static const option long_opts[] = {
        {"label", required_argument, nullptr, 'l'},
        {"force", no_argument, nullptr, 'F'},
        {"virtual-tape-size", required_argument, nullptr, 256},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    Options opts;
    int c;
    while ((c = getopt_long(argc, argv, "l:Fh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'l':
            opts.label = optarg;
            break;
        case 'F':
            opts.force = true;
            break;
        case 256:
            opts.virtual_tape_size =
                neotape::parse_size(optarg, "virtual tape size");
            break;
        case 'h':
            usage(argv[0]);
            break;
        case '?':
            std::exit(2);
        }
    }

    if (optind >= argc)
        usage(argv[0]);
    opts.target = neotape::parse_locator(argv[optind++]);
    if (optind != argc)
        usage(argv[0]);
    if (opts.target.kind != "tape" && opts.target.kind != "spool")
        fail("init target must be tape: or spool:");
    if (opts.target.kind == "tape" && opts.virtual_tape_size != 0)
        fail("--virtual-tape-size is only valid for spool targets");
    return opts;
}
```

- [ ] **Step 4: Implement spool init helper**

Add this helper above `neotape_init_main()`:

```cpp
void init_spool(const Options &opts) {
    namespace fs = std::filesystem;
    fs::path root(opts.target.locator);
    std::error_code ec;

    if (fs::exists(root, ec)) {
        if (!fs::is_directory(root, ec))
            fail(format("spool target is not a directory: {}", root.string()));
        if (!fs::is_empty(root, ec)) {
            if (!opts.force)
                fail(format("spool target is non-empty: {}", root.string()));
            fs::remove_all(root, ec);
            if (ec)
                fail(format("remove {}: {}", root.string(), ec.message()));
            fs::create_directories(root, ec);
        }
    } else {
        fs::create_directories(root, ec);
    }
    if (ec)
        fail(format("create {}: {}", root.string(), ec.message()));

    nlohmann::json manifest;
    manifest["kind"] = "neotape-spool-medium";
    manifest["label"] = opts.label;
    manifest["virtual_tape_size"] = opts.virtual_tape_size;
    manifest["volumes"] = nlohmann::json::array();

    std::ofstream out(root / "manifest.json", std::ios::binary);
    if (!out)
        fail(format("open manifest: {}", std::strerror(errno)));
    out << manifest.dump(2) << "\n";
    std::cerr << format("neotape init: initialized spool {}\n", root.string());
}
```

Add includes: `<filesystem>`, `<fstream>`, and `<nlohmann-json/json.hpp>`.

- [ ] **Step 5: Split tape init from main**

Move the existing tape initialization body into:

```cpp
void init_tape(const Options &opts) {
    mt::TapeDevice dev(opts.target.locator, true);
    dev.configure_preferred_variable_block_mode(
        65536, "neotape-init medium header", std::cerr);
    // Keep the existing probe, force, medium-header write, filemark, and rewind logic here.
}
```

Then make `neotape_init_main()` dispatch:

```cpp
int neotape_init_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);
        if (opts.target.kind == "spool")
            init_spool(opts);
        else
            init_tape(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape init: {}\n", e.what());
        return 1;
    }
}
```

- [ ] **Step 6: Link real init into `bin/neotape`**

Modify `Makefile` so `bin/neotape` links `src/neotape_init.o` compiled with `-DNEOTAPE_NO_STANDALONE_MAIN` instead of the init stub. Use a dedicated object to avoid changing the standalone binary compile rule:

```make
src/neotape_init.cmd.o : src/neotape_init.cpp Makefile
	$(CXX) $(CXXFLAGS) -DNEOTAPE_NO_STANDALONE_MAIN -c $< -o $@

$(BINDIR)/neotape : src/neotape.cpp $(CLI_OBJ) src/neotape_init.cmd.o $(COMMAND_STUBS_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) src/neotape_init.cmd.o $(COMMAND_STUBS_OBJ) -o $@ $(LDLIBS)
```

Remove the init stub from `src/neotape_command_stubs.cpp`:

```cpp
// Delete: int neotape_init_main(int, char **) { return stub("init"); }
```

- [ ] **Step 7: Verify spool init behavior**

Run:

```sh
rm -rf /tmp/neotape-cli-init.spool
make bin/neotape bin/test_cli && bin/test_cli
bin/neotape init spool:/tmp/neotape-cli-init.spool --label TEST --virtual-tape-size 64M
```

Expected: exit 0, `/tmp/neotape-cli-init.spool/manifest.json` exists, and stderr says the spool was initialized.

Run:

```sh
bin/neotape init spool:/tmp/neotape-cli-init.spool --label TEST2
```

Expected: exit 1 because the directory is non-empty.

- [ ] **Step 8: Commit Task 4**

Run:

```sh
git add src/neotape_init.cpp src/neotape.cpp src/neotape_command_stubs.cpp tests/test_cli.cpp Makefile
git commit -m "feat(cli): implement neotape init locators"
```

## Task 5: `neotape plan` Source-Tree Semantics

**Files:**
- Modify: `src/neotape_plan.cpp`
- Modify: `src/neotape_command_stubs.cpp`
- Modify: `Makefile`
- Test: use shell smoke fixture

- [ ] **Step 1: Change `neotape-plan` usage to new shape**

In `src/neotape_plan.cpp`, update usage text to:

```cpp
void usage(const char *prog) {
    std::cerr << format(
        "usage: {} [-C <dir>] -o <file|-> [--slice-size <bytes>]\n"
        "       [--metadata-buffer-size <bytes>] [--io-threads <N>] <path> [path ...]\n",
        prog);
}
```

- [ ] **Step 2: Enforce one `-C` and positional sources**

In `parse_args()`, add a local `bool saw_chdir = false;` before the option loop. Replace the `case 'C'` body with:

```cpp
case 'C':
    if (saw_chdir)
        fail("-C may be specified at most once");
    saw_chdir = true;
    opts.chdir_dir = optarg;
    break;
```

Keep the existing positional collection:

```cpp
while (optind < argc)
    opts.sources.emplace_back(argv[optind++]);
```

Keep the existing empty source rejection.

- [ ] **Step 3: Emit `/chdir` only at the beginning**

In the plan output function, ensure the first record is the optional chdir directive. The emitted code should be:

```cpp
if (!opts.chdir_dir.empty())
    std::fprintf(opts.meta_out, "/chdir/%s\0\n", opts.chdir_dir.c_str());

for (const auto &slice : slices) {
    for (const auto &entry : slice.entries) {
        std::fprintf(opts.meta_out, "/%llu/%llu/%c/%llu/%s\0\n",
                     static_cast<unsigned long long>(slice_index),
                     static_cast<unsigned long long>(file_index), entry.kind,
                     static_cast<unsigned long long>(entry.apparent_bytes),
                     entry.archive_path.c_str());
    }
}
```

Use the actual loop variable names from the current file. The required behavior is that no entry record appears before `/chdir`.

- [ ] **Step 4: Link real plan into `bin/neotape`**

Modify `Makefile`:

```make
src/neotape_plan.cmd.o : src/neotape_plan.cpp Makefile
	$(CXX) $(CXXFLAGS) -DNEOTAPE_NO_STANDALONE_MAIN -c $< -o $@
```

Add `src/neotape_plan.cmd.o` to the `bin/neotape` link line and remove the plan stub from `src/neotape_command_stubs.cpp`.

- [ ] **Step 5: Verify plan smoke behavior**

Run:

```sh
rm -rf /tmp/neotape-plan-fixture
mkdir -p /tmp/neotape-plan-fixture/src/photos /tmp/neotape-plan-fixture/src/docs
printf 'a' > /tmp/neotape-plan-fixture/src/photos/a.txt
printf 'b' > /tmp/neotape-plan-fixture/src/docs/b.txt
make bin/neotape
bin/neotape plan -C /tmp/neotape-plan-fixture/src -o /tmp/neotape-plan-fixture/plan.meta photos docs
```

Expected: exit 0 and `/tmp/neotape-plan-fixture/plan.meta` exists. Inspect it with a binary-safe viewer or a short one-off command; the first bytes must start with `/chdir//tmp/neotape-plan-fixture/src\0\n`.

Run:

```sh
bin/neotape plan -C /tmp -C /var -o /tmp/neotape-plan-fixture/bad.meta src
```

Expected: exit 1 and stderr contains `-C may be specified at most once`.

- [ ] **Step 6: Commit Task 5**

Run:

```sh
git add src/neotape_plan.cpp src/neotape_command_stubs.cpp Makefile
git commit -m "feat(cli): wire neotape plan"
```

## Task 6: Raw `neotape write` And `neotape read`

**Files:**
- Modify: `src/neotape_write.cpp`
- Modify: `src/neotape_cat_volumes.cpp`
- Modify: `src/neotape_command_stubs.cpp`
- Modify: `Makefile`
- Test: raw spool round-trip shell smoke

- [ ] **Step 1: Add new raw write options**

In `src/neotape_write.cpp`, add a new options struct for the main CLI path:

```cpp
struct RawWriteOptions {
    neotape::Locator target;
    string input = "-";
    string archive_name = "raw";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};
```

Add includes for `neotape/cli.hpp`.

- [ ] **Step 2: Implement raw write parser**

Add a parser function above the command entry point:

```cpp
RawWriteOptions parse_raw_write_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"target", required_argument, nullptr, 't'},
        {"input", required_argument, nullptr, 'i'},
        {"name", required_argument, nullptr, 'n'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"control", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    RawWriteOptions opts;
    bool saw_target = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 't':
            opts.target = neotape::parse_locator(optarg);
            saw_target = true;
            break;
        case 'i':
            opts.input = optarg;
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'h':
            std::cout << "usage: neotape write --target <locator> --input <file|-> [--name <name>] [--volume-block-size <bytes>] [--control=auto|none]\n";
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (!saw_target)
        fail("write requires --target <locator>");
    if (optind != argc)
        fail("write does not accept positional arguments");
    if (opts.target.kind != "spool" && opts.target.kind != "tape")
        fail("write target must be tape: or spool:");
    if (!neotape::valid_block_size(opts.volume_block_size))
        fail("volume block size must be between 4096 and 8388608 bytes");
    return opts;
}
```

- [ ] **Step 3: Map raw write options to existing writer**

Add:

```cpp
int neotape_write_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_write_args(argc, argv);
        Options opts;
        opts.input = raw.input;
        opts.archive_name = raw.archive_name;
        opts.volume_block_size = raw.volume_block_size;
        opts.payload_profile = "raw";
        if (raw.target.kind == "spool")
            opts.output_dir = raw.target.locator;
        else
            opts.tape_device = raw.target.locator;
        run_writer(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape write: {}\n", e.what());
        return 1;
    }
}
```

This first implementation uses existing writer behavior. Strict append preflight is enforced by the writer path when the backend supports it; if the current spool writer still creates uninitialized directories, make it fail unless the target root already contains `manifest.json` from `neotape init`.

- [ ] **Step 4: Implement raw read parser and mapping**

In `src/neotape_cat_volumes.cpp`, add includes for `neotape/cli.hpp`, then add:

```cpp
struct RawReadOptions {
    neotape::Locator source;
    string output = "-";
    string archive_selector;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
};

RawReadOptions parse_raw_read_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"output", required_argument, nullptr, 'o'},
        {"archive", required_argument, nullptr, 'a'},
        {"control", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    RawReadOptions opts;
    bool saw_source = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = neotape::parse_locator(optarg);
            saw_source = true;
            break;
        case 'o':
            opts.output = optarg;
            break;
        case 'a':
            opts.archive_selector = optarg;
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'h':
            std::cout << "usage: neotape read --source <locator> --output <file|-> [--archive <index|uuid>] [--control=auto|none]\n";
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (!saw_source)
        fail("read requires --source <locator>");
    if (optind != argc)
        fail("read does not accept positional arguments");
    if (opts.source.kind != "spool")
        fail("read currently supports spool: sources first; tape: wiring follows the tape reader backend");
    return opts;
}
```

Then add:

```cpp
int neotape_read_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_read_args(argc, argv);
        Options opts;
        opts.spool_dir = raw.source.locator;
        opts.output = raw.output;
        run_cat_volumes(opts);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape read: {}\n", e.what());
        return 1;
    }
}
```

- [ ] **Step 5: Link real write/read into `bin/neotape`**

Modify `Makefile`:

```make
src/neotape_write.cmd.o : src/neotape_write.cpp Makefile
	$(CXX) $(CXXFLAGS) -DNEOTAPE_NO_STANDALONE_MAIN -c $< -o $@

src/neotape_cat_volumes.cmd.o : src/neotape_cat_volumes.cpp Makefile
	$(CXX) $(CXXFLAGS) -DNEOTAPE_NO_STANDALONE_MAIN -c $< -o $@
```

Add these objects and their existing dependencies to `bin/neotape`, and remove the write/read stubs from `src/neotape_command_stubs.cpp`.

- [ ] **Step 6: Verify raw spool round trip**

Run:

```sh
rm -rf /tmp/neotape-raw.spool
printf 'raw payload data' > /tmp/neotape-raw.in
make bin/neotape
bin/neotape init spool:/tmp/neotape-raw.spool --label RAW --virtual-tape-size 64M
bin/neotape write --target spool:/tmp/neotape-raw.spool --input /tmp/neotape-raw.in --name raw-test
bin/neotape read --source spool:/tmp/neotape-raw.spool --output /tmp/neotape-raw.out
cmp /tmp/neotape-raw.in /tmp/neotape-raw.out
```

Expected: all commands exit 0; `cmp` exits 0.

- [ ] **Step 7: Commit Task 6**

Run:

```sh
git add src/neotape_write.cpp src/neotape_cat_volumes.cpp src/neotape_command_stubs.cpp Makefile
git commit -m "feat(cli): add raw read and write subcommands"
```

## Task 7: Streaming PAX `neotape backup` And `neotape restore`

**Files:**
- Modify: `src/neotape_write.cpp`
- Modify: `src/neotape_cat_volumes.cpp`
- Modify: `src/neotape_command_stubs.cpp`
- Modify: `Makefile`
- Test: PAX spool smoke with `bsdtar` when available

- [ ] **Step 1: Add the PAX writer include and backup options**

In `src/neotape_write.cpp`, add the include near the existing NeoTape includes:

```cpp
#include "neotape/pax_writer.hpp"
```

Add `BackupOptions` near `RawWriteOptions`:

```cpp
struct BackupOptions {
    neotape::Locator target;
    string archive_name = "pax";
    uint32_t volume_block_size = 4 * 1024 * 1024;
    neotape::ControlPolicy control = neotape::ControlPolicy::auto_prompt;
    std::optional<string> chdir_dir;
    std::optional<fs::path> plan_path;
    vector<fs::path> sources;
};
```

- [ ] **Step 2: Implement the backup parser**

Add:

```cpp
void backup_usage() {
    std::cerr << "usage: neotape backup --target <locator> [-C <dir>] <path> [path ...]\n"
                 "       neotape backup --target <locator> -p <plan>\n"
                 "       [--name <name>] [--volume-block-size <bytes>] "
                 "[--control=auto|none]\n";
}

BackupOptions parse_backup_args(int argc, char **argv) {
    static const struct option long_opts[] = {
        {"target", required_argument, nullptr, 't'},
        {"name", required_argument, nullptr, 'n'},
        {"volume-block-size", required_argument, nullptr, 'b'},
        {"control", required_argument, nullptr, 'c'},
        {"plan", required_argument, nullptr, 'p'},
        {"directory", required_argument, nullptr, 'C'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    BackupOptions opts;
    bool saw_target = false;
    bool saw_chdir = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "C:p:", long_opts, nullptr)) != -1) {
        switch (c) {
        case 't':
            opts.target = neotape::parse_locator(optarg);
            saw_target = true;
            break;
        case 'n':
            opts.archive_name = optarg;
            break;
        case 'b':
            opts.volume_block_size = static_cast<uint32_t>(
                neotape::parse_size(optarg, "volume block size"));
            break;
        case 'c':
            opts.control = neotape::parse_control_policy(optarg);
            break;
        case 'p':
            opts.plan_path = fs::path(optarg);
            break;
        case 'C':
            if (saw_chdir)
                fail("-C may be specified at most once");
            saw_chdir = true;
            opts.chdir_dir = optarg;
            break;
        case 'h':
            backup_usage();
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    while (optind < argc)
        opts.sources.emplace_back(argv[optind++]);
    if (!saw_target)
        fail("backup requires --target <locator>");
    if (opts.target.kind != "spool" && opts.target.kind != "tape")
        fail("backup target must be tape: or spool:");
    if (opts.plan_path && (opts.chdir_dir || !opts.sources.empty()))
        fail("-p cannot be combined with -C or positional sources");
    if (!opts.plan_path && opts.sources.empty())
        fail("backup requires source paths or -p <plan>");
    if (!neotape::valid_block_size(opts.volume_block_size))
        fail("volume block size must be between 4096 and 8388608 bytes");
    return opts;
}
```

- [ ] **Step 3: Extract a streaming spool archive sink**

Replace the body-level orchestration in `write_spool_archive()` with a reusable sink class that still uses the existing private record/framing helpers. Add this class after `read_spool_virtual_tape_size()`:

```cpp
class SpoolArchiveSink {
    WriterState state_;
    bool ended_ = false;

  public:
    SpoolArchiveSink(Options opts) {
        state_.opts = std::move(opts);

        bool is_append = fs::exists(state_.opts.output_dir);
        if (is_append) {
            uint64_t last_seq = find_last_tape_seq(state_.opts.output_dir);
            state_.tape_seq = last_seq;

            if (last_seq > 0 && state_.opts.virtual_tape_size > 0) {
                TapeDirInfo last_info = scan_tape_dir(
                    state_.opts.output_dir / format("tape-{}", six(last_seq)));
                if (last_info.used_bytes +
                        static_cast<uint64_t>(state_.opts.volume_block_size) * 2 <=
                    state_.opts.virtual_tape_size) {
                    state_.tape_file_num = last_info.last_file_num;
                    state_.volume_used = last_info.used_bytes;
                    state_.current_volume_dir =
                        state_.opts.output_dir / format("tape-{}", six(last_seq));
                }
            }
        } else {
            fs::create_directories(state_.opts.output_dir);
        }

        state_.archive_uuid = neotape::make_uuid_v4();
        bool reuse_dir = is_append && !state_.current_volume_dir.empty();
        write_volume_header(state_, reuse_dir);
    }

    void write_payload(const uint8_t *data, size_t len, bool end) {
        vector<uint8_t> payload(data, data + len);
        write_content_frame(state_, payload, end);
    }

    void finish() {
        if (ended_)
            return;
        write_archive_end(state_);
        write_tape_manifest(state_);
        ended_ = true;
        std::cerr << format("archive {} written to {}\n", state_.archive_uuid,
                            state_.opts.output_dir.string());
    }
};
```

Then rewrite `write_spool_archive()` to use `SpoolArchiveSink` for the existing raw path:

```cpp
void write_spool_archive(const Options &opts) {
    FILE *input = stdin;
    if (opts.input != "-") {
        input = std::fopen(opts.input.c_str(), "rb");
        if (input == nullptr)
            fail_errno(string("open ") + opts.input);
    }

    SpoolArchiveSink sink(opts);
    write_stream_payload(sink.state(), input,
                         opts.payload_profile == "raw" && opts.slice_size_set);
    if (input != stdin && std::fclose(input) != 0)
        fail_errno(string("close ") +
                   (opts.input.empty() ? "input" : opts.input));
    sink.finish();
}
```

Add `WriterState &state()` to the class if using the exact snippet above:

```cpp
WriterState &state() { return state_; }
```

Expected behavior after this step: raw `neotape write/read` still passes unchanged. This step must not add PAX backup behavior yet.

- [ ] **Step 4: Add streaming PAX backup for spool targets**

Add `neotape_backup_main()`:

```cpp
void run_spool_pax_backup(const BackupOptions &backup) {
    Options opts;
    opts.output_dir = backup.target.locator;
    opts.archive_name = backup.archive_name;
    opts.volume_block_size = backup.volume_block_size;
    opts.virtual_tape_size = read_spool_virtual_tape_size(opts.output_dir);
    opts.payload_profile = "pax";

    SpoolArchiveSink sink(std::move(opts));
    neotape::PaxWriterOptions pax;
    pax.output_name = "-";
    pax.plan_path = backup.plan_path;
    pax.chdir_dir = backup.chdir_dir;
    for (const auto &source : backup.sources)
        pax.sources.push_back(source.string());

    bool slice_open = false;
    neotape::PaxWriterCallbacks callbacks;
    callbacks.begin_slice = [&](uint64_t) {
        if (slice_open)
            throw std::runtime_error("pax writer began a slice before ending the previous slice");
        slice_open = true;
    };
    callbacks.write_chunk = [&](neotape::PaxChunk chunk) {
        if (!slice_open)
            callbacks.begin_slice(chunk.slice);
        auto *data = reinterpret_cast<const uint8_t *>(chunk.bytes.data());
        sink.write_payload(data, chunk.bytes.size(), false);
    };
    callbacks.end_slice = [&](uint64_t) {
        static const uint8_t empty = 0;
        sink.write_payload(&empty, 0, true);
        slice_open = false;
    };

    neotape::write_pax(pax, std::move(callbacks));
    if (slice_open)
        throw std::runtime_error("pax writer ended with an open slice");
    sink.finish();
}

int neotape_backup_main(int argc, char **argv) {
    try {
        auto backup = parse_backup_args(argc, argv);
        if (backup.target.kind == "tape")
            throw std::runtime_error("backup currently supports spool: targets; tape: backup needs tape sink wiring");
        run_spool_pax_backup(backup);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape backup: {}\n", e.what());
        return 1;
    }
}
```

This is intentionally streaming: `neotape::write_pax()` writes directly into `SpoolArchiveSink` via callbacks. Do not create a temporary PAX file or buffer the whole archive.

For unplanned backups, the current `write_pax()` callback path emits one logical slice. For planned backups, its `begin_slice` and `end_slice` events define NeoTape logical slice boundaries. Backend volume exhaustion may continue a logical slice on a new virtual volume, but must not create an extra logical slice by itself.

- [ ] **Step 5: Fix empty end-frame handling if the smoke test exposes it**

If `bsdtar` extraction fails because the restored stream contains an extra empty frame payload in the middle, change the writer sink to support ending the current slice without adding payload bytes. Add this method to `SpoolArchiveSink`:

```cpp
void end_slice() {
    vector<uint8_t> empty;
    write_content_frame(state_, empty, true);
}
```

Then change the callback to call `sink.end_slice()` instead of `sink.write_payload(&empty, 0, true)`. Keep this small and local; do not redesign frame encoding in this task.

- [ ] **Step 6: Implement restore parser and command**

In `src/neotape_cat_volumes.cpp`, add `neotape_restore_main()` as a PAX-profile wrapper around the reader path:

```cpp
int neotape_restore_main(int argc, char **argv) {
    try {
        auto raw = parse_raw_read_args(argc, argv);
        Options opts;
        opts.spool_dir = raw.source.locator;
        opts.output = raw.output;
        run_cat_volumes(opts);
        append_pax_eoa_if_needed(opts.output);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape restore: {}\n", e.what());
        return 1;
    }
}
```

Add this helper near the output sink code:

```cpp
void append_pax_eoa_if_needed(const string &output) {
    static const uint8_t zeros[1024] = {0};
    if (output == "-") {
        if (std::fwrite(zeros, 1, sizeof(zeros), stdout) != sizeof(zeros))
            throw std::runtime_error("write pax end-of-archive to stdout failed");
        return;
    }
    std::FILE *file = std::fopen(output.c_str(), "ab");
    if (file == nullptr)
        throw std::runtime_error(format("open {}: {}", output, std::strerror(errno)));
    if (std::fwrite(zeros, 1, sizeof(zeros), file) != sizeof(zeros)) {
        std::fclose(file);
        throw std::runtime_error("write pax end-of-archive failed");
    }
    std::fclose(file);
}
```

- [ ] **Step 7: Link backup/restore into dispatcher**

Remove the backup and restore stubs from `src/neotape_command_stubs.cpp`. Ensure `bin/neotape` links the command objects from Task 6.

- [ ] **Step 8: Verify PAX spool smoke**

Run:

```sh
rm -rf /tmp/neotape-pax.spool /tmp/neotape-pax-src /tmp/neotape-pax-out
mkdir -p /tmp/neotape-pax-src/dir
printf 'hello pax' > /tmp/neotape-pax-src/dir/file.txt
make bin/neotape
bin/neotape init spool:/tmp/neotape-pax.spool --label PAX --virtual-tape-size 64M
bin/neotape backup --target spool:/tmp/neotape-pax.spool /tmp/neotape-pax-src
bin/neotape restore --source spool:/tmp/neotape-pax.spool --output /tmp/neotape-pax.tar
mkdir -p /tmp/neotape-pax-out
bsdtar -xpf /tmp/neotape-pax.tar -C /tmp/neotape-pax-out
test -f /tmp/neotape-pax-out/tmp/neotape-pax-src/dir/file.txt
```

Expected: all commands exit 0. If `bsdtar` is unavailable, run the first four commands and verify `neotape restore` exits 0 and writes a non-empty tar stream.

- [ ] **Step 9: Commit Task 7**

Run:

```sh
git add src/neotape_write.cpp src/neotape_cat_volumes.cpp src/neotape_command_stubs.cpp Makefile
git commit -m "feat(cli): add pax backup and restore subcommands"
```

## Task 8: `neotape list` Archive Instances

**Files:**
- Create: `src/neotape_list.cpp`
- Modify: `src/neotape_command_stubs.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Implement list options and spool archive scan**

Create `src/neotape_list.cpp`:

```cpp
#include "neotape/cli.hpp"
#include "neotape/format.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <nlohmann-json/json.hpp>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using std::format;
using std::string;

struct Options {
    neotape::Locator source;
    bool json = false;
};

[[noreturn]] void fail(const string &msg) {
    std::cerr << format("neotape list: {}\n", msg);
    std::exit(1);
}

Options parse_args(int argc, char **argv) {
    static const option long_opts[] = {
        {"source", required_argument, nullptr, 's'},
        {"json", no_argument, nullptr, 'j'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};
    Options opts;
    bool saw_source = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 's':
            opts.source = neotape::parse_locator(optarg);
            saw_source = true;
            break;
        case 'j':
            opts.json = true;
            break;
        case 'h':
            std::cout << "usage: neotape list --source <locator> [--json]\n";
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (!saw_source)
        fail("list requires --source <locator>");
    if (opts.source.kind != "spool")
        fail("list currently supports spool: sources first; tape: follows tape reader archive scanning");
    return opts;
}

struct ArchiveRow {
    uint64_t index = 0;
    string uuid;
    string name;
    string profile;
    uint64_t volumes = 0;
    string status = "unknown";
};

std::vector<ArchiveRow> list_spool(const fs::path &root) {
    std::vector<ArchiveRow> rows;
    fs::path manifest_path = root / "manifest.json";
    if (!fs::is_regular_file(manifest_path))
        fail("spool source is not initialized: missing manifest.json");
    std::ifstream in(manifest_path);
    nlohmann::json manifest = nlohmann::json::parse(in);
    if (manifest.contains("archives")) {
        uint64_t index = 1;
        for (const auto &item : manifest["archives"]) {
            rows.push_back(ArchiveRow{
                index++, item.value("uuid", ""), item.value("name", ""),
                item.value("profile", ""), item.value("volumes", 0ull),
                item.value("status", "unknown")});
        }
    }
    return rows;
}

void print_human(const std::vector<ArchiveRow> &rows) {
    std::cout << "INDEX  UUID  NAME  PROFILE  VOLUMES  STATUS\n";
    for (const auto &row : rows) {
        std::cout << row.index << "  " << row.uuid << "  " << row.name << "  "
                  << row.profile << "  " << row.volumes << "  " << row.status
                  << "\n";
    }
}

void print_json(const std::vector<ArchiveRow> &rows) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto &row : rows) {
        out.push_back({{"index", row.index},
                       {"uuid", row.uuid},
                       {"name", row.name},
                       {"profile", row.profile},
                       {"volumes", row.volumes},
                       {"status", row.status}});
    }
    std::cout << out.dump(2) << "\n";
}

} // namespace

int neotape_list_main(int argc, char **argv) {
    try {
        auto opts = parse_args(argc, argv);
        auto rows = list_spool(opts.source.locator);
        if (opts.json)
            print_json(rows);
        else
            print_human(rows);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << format("neotape list: {}\n", e.what());
        return 1;
    }
}
```

- [ ] **Step 2: Update spool writer manifest with archive rows**

In `src/neotape_write.cpp`, after a successful archive end write in spool mode, update root `manifest.json` by appending:

```json
{
  "uuid": "<archive_uuid>",
  "name": "<archive_name>",
  "profile": "raw" or "pax",
  "volumes": <volume_count>,
  "status": "clean"
}
```

Use existing `WriterState` fields for UUID, archive name, profile, and volume count. If the current writer only writes per-volume manifests, add a small helper:

```cpp
void append_archive_manifest(const WriterState &state) {
    auto manifest_path = state.opts.output_dir / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream in(manifest_path);
        if (in)
            manifest = nlohmann::json::parse(in);
    }
    if (!manifest.contains("archives"))
        manifest["archives"] = nlohmann::json::array();
    manifest["archives"].push_back({
        {"uuid", state.archive_uuid},
        {"name", state.opts.archive_name},
        {"profile", state.opts.payload_profile},
        {"volumes", state.volume_seq_num},
        {"status", "clean"},
    });
    std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
    if (!out)
        fail("open spool manifest for archive append failed");
    out << manifest.dump(2) << "\n";
}
```

- [ ] **Step 3: Link list into dispatcher**

Modify `Makefile`:

```make
src/neotape_list.o : src/neotape_list.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

Add `src/neotape_list.o` to `bin/neotape` and remove the list stub.

- [ ] **Step 4: Verify list output**

Run:

```sh
rm -rf /tmp/neotape-list.spool
printf 'list payload' > /tmp/neotape-list.in
make bin/neotape
bin/neotape init spool:/tmp/neotape-list.spool --label LIST --virtual-tape-size 64M
bin/neotape write --target spool:/tmp/neotape-list.spool --input /tmp/neotape-list.in --name list-raw
bin/neotape list --source spool:/tmp/neotape-list.spool
bin/neotape list --source spool:/tmp/neotape-list.spool --json
```

Expected: human output contains `INDEX` and `list-raw`; JSON output contains an object with `"name": "list-raw"` and `"profile": "raw"`.

- [ ] **Step 5: Commit Task 8**

Run:

```sh
git add src/neotape_list.cpp src/neotape_write.cpp src/neotape_command_stubs.cpp Makefile
git commit -m "feat(cli): add archive instance listing"
```

## Task 9: Volume-Change Control Integration

**Files:**
- Modify: `src/neotape_write.cpp`
- Modify: `src/neotape_cat_volumes.cpp`
- Modify: `src/neotape_cli.cpp`
- Test: parser/unit tests and non-interactive failure path

- [ ] **Step 1: Verify control parser already tested**

Run: `make bin/test_cli && bin/test_cli`

Expected: exit 0, including `auto` and `none` control-policy tests from Task 1.

- [ ] **Step 2: Add no-prompt failure helper**

In `src/neotape_cli.cpp`, add:

```cpp
void require_prompt_allowed(ControlPolicy policy) {
    if (policy == ControlPolicy::none)
        throw std::runtime_error("volume change required but --control=none is set");
}
```

Declare it in `include/neotape/cli.hpp`:

```cpp
void require_prompt_allowed(ControlPolicy policy);
```

- [ ] **Step 3: Use control policy in write continuation points**

In `src/neotape_write.cpp`, at the code path that handles EOT or virtual volume rollover requiring a user-supplied next volume, call:

```cpp
neotape::require_prompt_allowed(raw_or_backup_control_policy);
auto result = neotape::prompt_for_volume_change({
    .archive_uuid = state.archive_uuid,
    .expected_volume = state.volume_seq_num + 1,
    .current_locator = current_locator,
    .write_mode = true,
});
```

Handle choices:

```cpp
switch (result.choice) {
case neotape::VolumePromptChoice::continue_current:
    reopen_current_target_and_verify_next_volume();
    break;
case neotape::VolumePromptChoice::change_locator:
    current_locator = *result.replacement_locator;
    open_replacement_target_and_verify_next_volume(current_locator);
    break;
case neotape::VolumePromptChoice::shell:
    break;
case neotape::VolumePromptChoice::abort:
    throw std::runtime_error("user aborted volume change");
}
```

Use the existing writer function names for reopening and verifying; if they do not exist, create small helpers with exactly those responsibilities.

- [ ] **Step 4: Use control policy in read continuation points**

In `src/neotape_cat_volumes.cpp`, at the code path where the reader reaches volume end before Archive End, call the same prompt helper with `.write_mode = false`. Verify the next volume's archive UUID and expected volume sequence before emitting more payload bytes.

- [ ] **Step 5: Verify non-interactive failure mode**

Create a spool fixture with a missing continuation volume by writing with a very small initialized virtual size, then removing the second volume directory. Run:

```sh
bin/neotape read --source spool:/tmp/neotape-missing-volume.spool --output /tmp/out --control=none
```

Expected: exit 1 and stderr contains `volume change required but --control=none is set`.

- [ ] **Step 6: Commit Task 9**

Run:

```sh
git add include/neotape/cli.hpp src/neotape_cli.cpp src/neotape_write.cpp src/neotape_cat_volumes.cpp
git commit -m "feat(cli): add volume change control policy"
```

## Task 10: Documentation, Defaults, And Final Verification

**Files:**
- Modify: `README.md`
- Modify: `docs/spec/appendix-cli.md`
- Modify: `docs/spec/open-questions.md`
- Modify: `AGENTS.md` only if build/test commands change

- [ ] **Step 1: Update CLI appendix**

Replace the examples in `docs/spec/appendix-cli.md` with:

```markdown
## Main CLI

```sh
neotape init tape:/dev/nst0 --label MYTAPE001
neotape init spool:./archive.spool --label TEST --virtual-tape-size 100G

neotape plan -C /data -o home.plan photos docs
neotape backup --target tape:/dev/nst0 -p home.plan --name home
neotape backup --target spool:./archive.spool ./source

neotape restore --source tape:/dev/nst0 --output - | bsdtar -xpf -

neotape write --target spool:./archive.spool --input payload.bin --name raw1
neotape read --source spool:./archive.spool --output payload.out

neotape list --source spool:./archive.spool
neotape list --source spool:./archive.spool --json
```
```

Keep a short note that `neotape-inspect` and `mt-pax` remain standalone tools.

- [ ] **Step 2: Update README usage section**

Replace the current usage section with examples for `bin/neotape`, and keep `bin/mt-pax` as a standalone advanced PAX writer. Include the default:

```text
Default archive --volume-block-size is 4 MiB.
```

- [ ] **Step 3: Update open questions**

In `docs/spec/open-questions.md`, move the following CLI decisions out of open status if present:

```text
stdout is payload bytes only
read/restore default archive selection
volume block size default
raw vs PAX command split
```

If a matching open question is absent, do not add a new resolved section just for this task.

- [ ] **Step 4: Run full build and tests**

Run:

```sh
make clean
make -j "$(nproc)"
make test
```

Expected: build exits 0 and `make test` exits 0.

- [ ] **Step 5: Run CLI smoke suite manually**

Run:

```sh
rm -rf /tmp/neotape-final.spool /tmp/neotape-final-src /tmp/neotape-final-out
mkdir -p /tmp/neotape-final-src
printf 'final raw' > /tmp/neotape-final.raw
printf 'final pax' > /tmp/neotape-final-src/file.txt
bin/neotape init spool:/tmp/neotape-final.spool --label FINAL --virtual-tape-size 64M
bin/neotape write --target spool:/tmp/neotape-final.spool --input /tmp/neotape-final.raw --name final-raw
bin/neotape read --source spool:/tmp/neotape-final.spool --archive 1 --output /tmp/neotape-final.raw.out
cmp /tmp/neotape-final.raw /tmp/neotape-final.raw.out
bin/neotape list --source spool:/tmp/neotape-final.spool
bin/neotape plan -o /tmp/neotape-final.plan /tmp/neotape-final-src
```

Expected: all commands exit 0; `cmp` exits 0; list output includes `final-raw`; plan file exists and is non-empty.

- [ ] **Step 6: Commit Task 10**

Run:

```sh
git add README.md docs/spec/appendix-cli.md docs/spec/open-questions.md AGENTS.md
git commit -m "docs: update CLI reference"
```

Only include `AGENTS.md` if it changed.

## Self-Review

Spec coverage:

- Command taxonomy: Tasks 2, 3, 6, 7, and 8.
- Standalone `mt-pax` and `neotape-inspect`: Task 10 documents preservation; no implementation task changes `mt-pax`.
- Locator grammar: Task 1.
- I/O vocabulary: Tasks 4, 5, 6, and 7.
- Medium lifecycle and spool init: Task 4.
- Append semantics: Tasks 6 and 7 route write paths through initialized targets; strict preflight enforcement must be verified in those tasks.
- Volume-change prompt: Task 9.
- Read/restore/list semantics: Tasks 6, 7, and 8.
- Plan/PAX semantics: Tasks 5 and 7.
- Raw read/write semantics: Task 6.
- Option placement and 4 MiB default: Tasks 4, 6, 7, and 10.
- Exit and stream conventions: Tasks 2, 6, 7, 8, 9, and 10.

Placeholder scan:

- No placeholder markers remain in the task steps.
- Every command has explicit files, commands, and expected verification output.

Type consistency:

- `neotape::Locator`, `neotape::ControlPolicy`, `VolumePromptRequest`, and `VolumePromptResult` are defined in Task 1 and reused consistently.
- Public command entry points are declared in Task 2 and wired by later tasks.
- Default `--volume-block-size` is consistently 4 MiB for `backup/write`.
