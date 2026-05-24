# Makefile Src Objects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store NeoTape project object files beside their sources under `src/` and simplify repeated Makefile object rules.

**Architecture:** Keep third-party submodule build output under the existing `build/` paths because those rules are owned by included Makefiles. Replace project object variables from `build/*.o` to `src/*.o`, use one `src/%.o: src/%.cpp` rule for source objects, and keep only the `tests/tape_test_device.o` special rule for the non-`src/` object.

**Tech Stack:** GNU Make, C++20, existing bundled BLAKE3/crc32c libraries.

---

### Task 1: Simplify Project Object Rules

**Files:**
- Modify: `Makefile`

- [x] **Step 1: Update object variable locations**

Change project object variables from `$(BUILDDIR)/name.o` to `src/name.o`. Keep `BUILDDIR=build` for third-party artifacts.

- [x] **Step 2: Replace source object compile rule**

Use this rule for project C++ objects:

```make
src/%.o : src/%.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

- [x] **Step 3: Remove redundant explicit source object rules**

Remove explicit compile rules for `neotape_tape.o`, `neotape_tape_navigator.o`, and `neotape_tape_writer.o` because the pattern rule covers them.

- [x] **Step 4: Move reader object reference to `src/`**

Change `$(BUILDDIR)/neotape_reader.o` dependencies and link command references to `src/neotape_reader.o`.

- [x] **Step 5: Keep the test object special rule**

Compile `tests/tape_test_device.cpp` to `tests/tape_test_device.o` because it is not under `src/`:

```make
tests/tape_test_device.o : tests/tape_test_device.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

- [x] **Step 6: Simplify clean**

Make `clean` remove executables, `src/*.o`, `tests/*.o`, and third-party artifacts: `$(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)`.

- [x] **Step 7: Verify clean build**

Run: `make clean`
Expected: exits successfully, removing generated binaries and object files.

Run: `make -j "$(nproc)"`
Expected: exits successfully and produces the binaries listed in `EXE`.

Run: `make test`
Expected: `bin/test_tape` runs successfully.
