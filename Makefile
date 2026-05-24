CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O3 -flto -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -flto -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/mt-pax bin/neotape bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape bin/test_cli bin/neotape-init
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
CLI_OBJ = src/neotape_cli.o
COMMAND_STUBS_OBJ = src/neotape_command_stubs.o
INIT_CMD_OBJ = src/neotape_init.cmd.o

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

.PHONY: all clean countline format test generate tidy

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

src/%.o : src/%.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_init.cmd.o : src/neotape_init.cpp Makefile
	$(CXX) $(CXXFLAGS) -DNEOTAPE_NO_STANDALONE_MAIN -c $< -o $@

$(BINDIR)/mt-pax : src/mt-pax.cpp $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape : src/neotape.cpp $(CLI_OBJ) $(INIT_CMD_OBJ) $(COMMAND_STUBS_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) $(INIT_CMD_OBJ) $(COMMAND_STUBS_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

$(FORMAT_GEN_OBJ): $(GENERATED_CPP) $(GENERATED_HPP) Makefile
	$(CXX) $(CXXFLAGS) -c $(GENERATED_CPP) -o $@

$(BINDIR)/neotape-write : src/neotape_write.cpp $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-inspect : src/neotape_inspect.cpp $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-plan : src/neotape_plan.cpp $(COMMON_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(COMMON_OBJ) -o $@

$(BINDIR)/neotape-cat-volumes : src/neotape_cat_volumes.cpp src/neotape_reader.o $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< src/neotape_reader.o $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

# neotape-init
$(BINDIR)/neotape-init : src/neotape_init.cpp $(CLI_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

# Test binary
$(BINDIR)/test_tape : tests/test_tape.cpp $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(TAPE_OBJ) $(NAV_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/test_cli : tests/test_cli.cpp $(CLI_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(CLI_OBJ) -o $@

test: $(BINDIR)/test_tape $(BINDIR)/test_cli
	$(BINDIR)/test_tape
	$(BINDIR)/test_cli

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
