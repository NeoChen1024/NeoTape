# Remove Standalone NeoTape Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove obsolete standalone NeoTape command binaries so all integrated commands run through `bin/neotape` subcommands.

**Architecture:** Command implementations become command modules with `_cmd.cpp` filenames and callable `neotape_*_main()` entry points only. The Makefile links those modules into `bin/neotape` and no longer builds `bin/neotape-init`, `bin/neotape-plan`, `bin/neotape-write`, or `bin/neotape-cat-volumes`.

**Tech Stack:** C++20, GNU Make, shell smoke tests.

---

### Task 1: Rename Command Modules

**Files:**
- Move: `src/neotape_init.cpp` to `src/neotape_init_cmd.cpp`
- Move: `src/neotape_plan.cpp` to `src/neotape_plan_cmd.cpp`
- Move: `src/neotape_write.cpp` to `src/neotape_write_cmd.cpp`
- Move: `src/neotape_cat_volumes.cpp` to `src/neotape_read_cmd.cpp`

- [ ] **Step 1: Move files with git**

Run:
```bash
git mv src/neotape_init.cpp src/neotape_init_cmd.cpp
git mv src/neotape_plan.cpp src/neotape_plan_cmd.cpp
git mv src/neotape_write.cpp src/neotape_write_cmd.cpp
git mv src/neotape_cat_volumes.cpp src/neotape_read_cmd.cpp
```

- [ ] **Step 2: Verify moved files are tracked as renames**

Run: `git status --short`

Expected: rename entries for the four command source files, plus pre-existing unrelated modified files.

### Task 2: Remove Standalone Main Entrypoints

**Files:**
- Modify: `src/neotape_init_cmd.cpp`
- Modify: `src/neotape_plan_cmd.cpp`
- Modify: `src/neotape_write_cmd.cpp`
- Modify: `src/neotape_read_cmd.cpp`

- [ ] **Step 1: Delete standalone `main()` guards**

Remove blocks like:
```cpp
#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) { return neotape_init_main(argc, argv); }
#endif
```

For `src/neotape_read_cmd.cpp`, remove:
```cpp
#ifndef NEOTAPE_NO_STANDALONE_MAIN
int main(int argc, char **argv) {
    return neotape_cat_volumes_legacy_main(argc, argv);
}
#endif
```

- [ ] **Step 2: Confirm no standalone main guards remain in command modules**

Run: `rg 'NEOTAPE_NO_STANDALONE_MAIN|neotape_cat_volumes_legacy_main|int main' src/neotape_*_cmd.cpp`

Expected: no `NEOTAPE_NO_STANDALONE_MAIN`; no standalone `int main` in command modules. `neotape_cat_volumes_legacy_main` may be removed if unused.

### Task 3: Update Makefile Build Targets

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Remove obsolete standalone binaries from `EXE`**

Change `EXE` from including:
```make
bin/neotape-write bin/neotape-plan bin/neotape-cat-volumes bin/neotape-init
```

To only command-integrated outputs:
```make
EXE = bin/mt-pax bin/neotape bin/neotape-inspect bin/test_tape bin/test_cli bin/test_restore_validation
```

- [ ] **Step 2: Point command object rules at renamed files**

Use:
```make
src/neotape_init_cmd.o : src/neotape_init_cmd.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

Repeat for `neotape_plan_cmd.o`, `neotape_write_cmd.o`, and `neotape_read_cmd.o`. Remove `-DNEOTAPE_NO_STANDALONE_MAIN`.

- [ ] **Step 3: Remove obsolete standalone binary rules**

Delete rules for:
```make
$(BINDIR)/neotape-write
$(BINDIR)/neotape-plan
$(BINDIR)/neotape-cat-volumes
$(BINDIR)/neotape-init
```

- [ ] **Step 4: Update object variable names**

Set:
```make
INIT_CMD_OBJ = src/neotape_init_cmd.o
PLAN_CMD_OBJ = src/neotape_plan_cmd.o
WRITE_CMD_OBJ = src/neotape_write_cmd.o
READ_CMD_OBJ = src/neotape_read_cmd.o
```

### Task 4: Update Active References

**Files:**
- Modify active smoke tests and docs found by search.

- [ ] **Step 1: Search for active old binary references**

Run: `rg 'bin/neotape-(init|plan|write|cat-volumes)|neotape-(init|plan|write|cat-volumes)' tests docs src include Makefile`

- [ ] **Step 2: Update tests and non-archived active docs**

Replace command examples as follows:
```text
bin/neotape-init ...        -> bin/neotape init ...
bin/neotape-plan ...        -> bin/neotape plan ...
bin/neotape-write ...       -> bin/neotape write ...
bin/neotape-cat-volumes ... -> bin/neotape read ... or bin/neotape restore ... depending on context
```

Do not edit archived design/plan history under `docs/superpowers/*/archived/`.

### Task 5: Remove Generated Binary Remnants and Verify

**Files:**
- Remove generated files in `bin/` only.

- [ ] **Step 1: Remove existing obsolete binaries**

Run:
```bash
rm -f bin/neotape-init bin/neotape-plan bin/neotape-write bin/neotape-cat-volumes
```

- [ ] **Step 2: Run clean full verification**

Run: `make clean && make test`

Expected: build completes and tests pass.

- [ ] **Step 3: Confirm obsolete binaries are not recreated**

Run: `ls bin/neotape-init bin/neotape-plan bin/neotape-write bin/neotape-cat-volumes`

Expected: command exits non-zero with all four paths reported missing.

- [ ] **Step 4: Inspect final diff**

Run: `git diff --stat && git diff -- Makefile src tests docs include`

Expected: only intended cleanup changes plus pre-existing unrelated edits remain.
