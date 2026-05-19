# Tape Writer Backend

Date: 2026-05-20
Status: Spec

## Problem

`neotape-write` currently only supports `--target=spool`, writing NeoTape
archives to a filesystem spool directory. The tape device backend (Phase 6
of the roadmap) is not yet implemented. Additionally, no tool exists to
initialize a physical medium with a Medium Header.

## Solution

Add a tape device backend to `neotape-write` (new `write_tape_archive()`
in a new file), plus a new `neotape-init` tool that writes a minimal
Medium Header at BOT.

The spool writer is left untouched — no refactoring of working code.

---

## Detailed Design

### 1. CLI changes to `neotape-write`

Old: `neotape-write --target=spool -o <dir> [-f <input>]`
New: `neotape-write -f <device> [-i <input>]` (tape, auto-detected)
     `neotape-write --target=spool -o <dir> [-i <input>]` (spool, explicit)

- `-f` now means the output destination (tape device). Presence triggers
  tape mode.
- `-i` replaces the old `-f` for payload input. Default: stdin.
- `-o` remains the spool output directory (requires explicit
  `--target=spool` or absence of `-f`).
- `--target` is no longer required when `-f` is set (auto tape).

| Option | Old meaning | New meaning |
|--------|-------------|-------------|
| `-f`   | Input file  | Tape device (output) |
| `-i`   | N/A         | Input file/stdin |
| `-o`   | Spool dir   | Spool dir (unchanged) |

New tape-specific options:

| Option | Description |
|--------|-------------|
| `--init` | Write from BOT (overwrites existing data) |
| `--init-if-blank` | Only init if tape is blank |
| `--force-append` | Append even without valid archive end header |

### 2. `neotape-init` tool

New binary built from `src/neotape_init.cpp`.

```
neotape-init -f <device> [--label <text>] [--force]
```

Writes a minimal 1024-byte Medium Header + filemark at BOT.

**Flow:**
1. Open tape device with `TapeDevice(dev, read_write=true)`
2. Rewind
3. Read BOT record — check for NeoTape magic
4. If already initialized and no `--force`: fail
5. Generate `medium_uuid`, fill `MediumHeader` struct
6. Serialize to 1024-byte `HeaderBytes`, pad to `medium_header_block_size`
7. `::write(dev.fd(), bytes, block_size)` (default 64 KiB)
8. `dev.write_filemark()`

Exit codes: 0 on success, 1 on error, 2 on already-initialized.

### 3. MediumHeader format addition

New struct in `include/neotape/format.hpp`:

```cpp
struct MediumHeader {
    std::string medium_uuid;
    std::string medium_label;
    std::string initialized_at_utc;
    uint32_t medium_header_block_size = 65536;
    uint16_t medium_header_block_count = 1;
    uint16_t flags = 0;
    std::string created_by_implementation;
    std::string created_by_build_id;
    uint32_t metadata_bundle_size = 0;
    Hash metadata_bundle_blake3{};
};
```

New serialization: `HeaderBytes serialize_medium_header(const MediumHeader &h)`.
The `parse_fixed_header()` function gains handling for `HeaderType::medium`.

The Medium Header follows the same 1024-byte fixed layout as other headers,
with the magic+version+type at the standard offsets. Multi-record medium
headers and the ar metadata bundle are deferred (not MVP).

### 4. Tape writer (`src/neotape_tape_writer.cpp`)

Contains:

- `write_tape_record()` — builds a padded `volume_block_size`-byte buffer
  from a 1024-byte header + optional payload, calls `::write()` on the
  tape fd, detects ENOSPC.
- `write_tape_archive()` — main function: opens device, positions via
  navigator, writes volume header, runs framing loop, writes archive end.

**`write_tape_archive` flow:**

```
1. Open TapeDevice with opts.tape_device, read_write=true
2. dev.set_block_size(opts.volume_block_size)
3. If opts.init_mode: dev.rewind()
   Else:
     mt::nav::TapeNavigator nav(dev);
     auto r = nav.locate_append_position(
         opts.force_append ? mt::nav::AppendPolicy::force
                           : mt::nav::AppendPolicy::strict);
     If r.condition == blank && !opts.init_if_blank: fail
     If r.condition == corrupt_tail && !opts.force_append: fail
4. Create WriterState (same struct, shared with spool)
5. Write Volume Header:
   ::write(dev.fd(), vh_bytes, block_size)  → check ENOSPC
   dev.write_filemark()
6. Main loop (identical framing to spool):
   - Read input into frame-size chunks
   - Build FrameHeader, serialize
   - ::write(dev.fd(), frame_bytes, block_size)  → check ENOSPC
   - No filemark between frames within a slice
   - On slice boundary: write END frame, then dev.write_filemark()
   - On ENOSPC:
       a. If END frame was committed: prompt "Insert volume N, press Enter"
       b. If not committed: retry on next volume
       c. ::close(dev) / open new device or wait for same device
       d. Write Volume Header on new tape
       e. Continue
7. Write Archive End Header:
   ::write(dev.fd(), ae_bytes, block_size) → check ENOSPC
   dev.write_filemark()
8. Close device
```

**ENOSPC handling:**

After `::write()` returns -1 with `errno == ENOSPC`:
- Print to stderr: "End of tape reached, volume_seq_num=N"
- Print to stderr: "Insert next volume and press Enter"
- Read a line from stdin (blocking wait)
- For `TapeDevice`, the fd is reused. The user swaps tapes behind
  the device node (or uses a tape changer).
- `dev.rewind()` + `dev.space_to_eod()` are not called — the drive
  is at the end and the tape may have been ejected. The user inserts
  a fresh tape. We write a new Volume Header at BOT of the new tape.

### 5. Modification to `src/neotape_write.cpp`

- Accept `-i` flag for input (keep `-f` for backward compat as well,
  but with deprecation warning, or just switch cleanly).
- Add tape-specific option parsing (`--init`, `--init-if-blank`,
  `--force-append`).
- Route to `write_tape_archive()` when tape mode is detected.

### 6. Build integration

Add to `Makefile`:

```makefile
TAPE_WRITER_OBJ = $(BUILDDIR)/neotape_tape_writer.o
INIT_OBJ = $(BUILDDIR)/neotape_init.o

$(TAPE_WRITER_OBJ) : src/neotape_tape_writer.cpp Makefile | $(BUILDDIR)

$(BINDIR)/neotape-init : src/neotape_init.cpp $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-write : ... $(TAPE_WRITER_OBJ) ...
```

---

## File Inventory

| File | Action |
|------|--------|
| `include/neotape/format.hpp` | Modify: add `MediumHeader` struct |
| `src/neotape_format.cpp` | Modify: add `serialize_medium_header()`, Medium type in parser |
| `src/neotape_tape_writer.cpp` | Create: `write_tape_archive()` + helpers |
| `src/neotape_init.cpp` | Create: `neotape-init` CLI |
| `src/neotape_write.cpp` | Modify: CLI changes, tape routing |
| `Makefile` | Modify: build rules for new files |
| `tests/test_tape.cpp` | Modify: add tape writer tests against `FileBackedTapeDevice` |

---

## Verification

1. `make clean && make -j "$(nproc)"` succeeds
2. `make test` passes (10 existing + new tape writer tests)
3. `neotape-init -f /tmp/tape.img` creates a valid Medium Header
4. `neotape-write -f /tmp/tape.img -i /dev/null` writes Volume Header + Archive End
5. Spool mode still works: `neotape-write --target=spool -o /tmp/spool`
