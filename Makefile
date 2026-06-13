CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/neotape-archiver
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
FORMAT_OBJ = src/neotape_format.o
FORMAT_GEN_OBJ = src/neotape_format_generated.o
COMMON_OBJ = src/neotape_common.o
BOUNDEDBUF_OBJ = src/neotape_bounded_buffer.o
PAX_WRITER_OBJ = src/neotape_pax_writer.o
TAPE_OBJ = src/neotape_tape.o
PLAN_CMD_OBJ = src/neotape_plan_cmd.o
TCP_PROTO_OBJ = src/neotape_tcp_protocol.o
TCP_SERVER_OBJ = src/neotape_tcp_server.o
ARCHIVER_CMD_OBJ = src/neotape_archiver_cmd.o

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

.PHONY: all clean countline format test test_pax_cli tidy

all: ${EXE} $(TAPE_OBJ)

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

src/%.o : src/%.cpp Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/mt-pax : src/mt-pax.cpp $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(PAX_WRITER_OBJ) $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-plan : $(PLAN_CMD_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(PLAN_CMD_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

$(FORMAT_GEN_OBJ): $(GENERATED_CPP) $(GENERATED_HPP) Makefile
	$(CXX) $(CXXFLAGS) -c $(GENERATED_CPP) -o $@

$(BINDIR)/test_pax_pipeline : tests/test_pax_pipeline.cpp include/neotape/closable_queue.hpp Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BINDIR)/test_tcp_protocol : tests/test_tcp_protocol.cpp $(TCP_PROTO_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(TCP_PROTO_OBJ) -o $@

$(BINDIR)/neotape-archiver : $(ARCHIVER_CMD_OBJ) $(TCP_SERVER_OBJ) $(TCP_PROTO_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(ARCHIVER_CMD_OBJ) $(TCP_SERVER_OBJ) $(TCP_PROTO_OBJ) $(FORMAT_OBJ) $(FORMAT_GEN_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

test: $(BINDIR)/test_pax_pipeline $(BINDIR)/test_tcp_protocol $(BINDIR)/mt-pax $(BINDIR)/neotape-plan
	$(BINDIR)/test_pax_pipeline
	$(BINDIR)/test_tcp_protocol
	sh tests/smoke_mt_pax_pipeline.sh

test_pax_cli: $(BINDIR)/mt-pax
	sh tests/smoke_mt_pax_pipeline.sh

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
