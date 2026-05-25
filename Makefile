CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/mt-pax bin/neotape bin/neotape-inspect bin/test_tape bin/test_cli bin/test_restore_validation
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
FORMAT_OBJ = src/neotape_format.o
FORMAT_GEN_OBJ = src/neotape_format_generated.o
COMMON_OBJ = src/neotape_common.o
BOUNDEDBUF_OBJ = src/neotape_bounded_buffer.o
PAX_WRITER_OBJ = src/neotape_pax_writer.o
TAPE_OBJ = src/neotape_tape.o
NAV_OBJ  = src/neotape_tape_navigator.o
TAPE_WRITER_OBJ = src/neotape_tape_writer.o
RESTORE_VALIDATION_OBJ = src/neotape_restore_validation.o
CLI_OBJ = src/neotape_cli.o
COMMAND_STUBS_OBJ = src/neotape_command_stubs.o
INIT_CMD_OBJ = src/neotape_init_cmd.o
PLAN_CMD_OBJ = src/neotape_plan_cmd.o
WRITE_CMD_OBJ = src/neotape_write_cmd.o
READ_CMD_OBJ = src/neotape_read_cmd.o

include 3rdparty/blake3.mk
include 3rdparty/crc32c.mk

# ── Codegen ────────────────────────────────────────────────────────────
GENERATOR     = scripts/generate_neotape_parsers.py
GENERATED_HPP = include/neotape/format_generated.hpp
GENERATED_CPP = src/neotape_format_generated.cpp
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_FILES = $(filter-out $(GENERATED_HPP) $(GENERATED_CPP), \
	$(wildcard src/*.cpp include/neotape/*.hpp include/neotape/*.h))
CLANG_TIDY ?= clang-tidy
CLANG_TIDY_FILES = $(filter-out src/neotape_format_generated.cpp, $(wildcard src/*.cpp))

$(GENERATED_HPP) $(GENERATED_CPP): $(GENERATOR) scripts/neotape_header_defs.py
	python3 $(GENERATOR)

.PHONY: all clean countline format test test_pax_cli test_tape_backup_wiring test_inspect_diagnostic test_file_backed_tape generate tidy

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

src/%.o : src/%.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_init_cmd.o : src/neotape_init_cmd.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_plan_cmd.o : src/neotape_plan_cmd.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_write_cmd.o : src/neotape_write_cmd.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_read_cmd.o : src/neotape_read_cmd.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/mt-pax : src/mt-pax.cpp $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape : src/neotape.cpp $(CLI_OBJ) $(INIT_CMD_OBJ) $(PLAN_CMD_OBJ) $(WRITE_CMD_OBJ) $(READ_CMD_OBJ) $(COMMAND_STUBS_OBJ) src/neotape_reader.o $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) $(RESTORE_VALIDATION_OBJ) $(PAX_WRITER_OBJ) $(BOUNDEDBUF_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) $(INIT_CMD_OBJ) $(PLAN_CMD_OBJ) $(WRITE_CMD_OBJ) $(READ_CMD_OBJ) $(COMMAND_STUBS_OBJ) src/neotape_reader.o $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) $(RESTORE_VALIDATION_OBJ) $(PAX_WRITER_OBJ) $(BOUNDEDBUF_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

$(FORMAT_GEN_OBJ): $(GENERATED_CPP) $(GENERATED_HPP) Makefile
	$(CXX) $(CXXFLAGS) -c $(GENERATED_CPP) -o $@

$(BINDIR)/neotape-inspect : src/neotape_inspect.cpp $(CLI_OBJ) $(TAPE_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) $(TAPE_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) -o $@ $(LDLIBS)

# Test binary
$(BINDIR)/test_tape : tests/test_tape.cpp $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) $(CLI_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) $(CLI_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/test_cli : tests/test_cli.cpp $(CLI_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) -o $@

$(BINDIR)/test_restore_validation : tests/test_restore_validation.cpp $(RESTORE_VALIDATION_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(RESTORE_VALIDATION_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

test: $(BINDIR)/test_tape $(BINDIR)/test_cli $(BINDIR)/test_restore_validation $(BINDIR)/neotape $(BINDIR)/neotape-inspect
	$(BINDIR)/test_tape
	$(BINDIR)/test_cli
	$(BINDIR)/test_restore_validation
	sh tests/smoke_restore_validation.sh
	sh tests/smoke_pax_backup_restore.sh
	sh tests/smoke_inspect_diagnostic.sh
	sh tests/smoke_tape_backup_wiring.sh

test_pax_cli: $(BINDIR)/neotape
	sh tests/smoke_pax_backup_restore.sh

test_tape_backup_wiring: $(BINDIR)/neotape
	sh tests/smoke_tape_backup_wiring.sh

test_inspect_diagnostic: $(BINDIR)/neotape $(BINDIR)/neotape-inspect
	sh tests/smoke_inspect_diagnostic.sh

$(BINDIR)/% : src/%.c Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
format:
	$(CLANG_FORMAT) -i $(CLANG_FORMAT_FILES)
tidy:
	$(CLANG_TIDY) $(CLANG_TIDY_FILES) -- $(CXXFLAGS)
clean:
	-rm -f ${EXE} ${BINDIR}/*.o src/*.o tests/*.o $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
