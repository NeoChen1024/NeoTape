# shellcheck disable=SC1007,SC2037,SC2046,SC2068,SC2091,SC2283
CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a -larchive
EXE	= bin/mt-pax bin/neotape-plan bin/test_pax_pipeline bin/test_tcp_protocol bin/test_format bin/test_validate bin/neotape-archiver bin/neotape-write bin/neotape-read bin/neotape-extractor bin/neotape-raw-store bin/neotape-inspect bin/neotape-scan
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
# All .o are built from src/*.cpp via the pattern rule below.
# Header dependencies are listed where they matter.

include 3rdparty/blake3.mk

CLANG_FORMAT ?= clang-format
SRC_FILES = $(wildcard src/*.cpp include/neotape/*.hpp)
CLANG_TIDY ?= clang-tidy

.PHONY: all clean countline fix test test_pax_cli compile_commands

compile_commands.json:
	python3 scripts/gen_compile_commands.py

compile_commands: compile_commands.json

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3:
	mkdir -p $@

src/%.o : src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/neotape_format.o: include/neotape/format.hpp

# ── binaries ──

$(BINDIR)/mt-pax : src/mt-pax.o src/neotape_pax_writer.o src/neotape_common.o src/neotape_bounded_buffer.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-plan : src/neotape_plan_cmd.o src/neotape_format.o src/neotape_common.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/test_pax_pipeline : tests/test_pax_pipeline.cpp include/neotape/closable_queue.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BINDIR)/test_tcp_protocol : tests/test_tcp_protocol.cpp src/neotape_tcp_protocol.o | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(filter %.o,$^) -o $@

$(BINDIR)/test_format : tests/test_format.cpp src/neotape_format.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(filter %.o,$^) -o $@ $(LDLIBS)

$(BINDIR)/test_validate : tests/test_validate.cpp src/neotape_validate.o src/neotape_format.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(filter %.o,$^) -o $@ $(LDLIBS)

$(BINDIR)/neotape-archiver : src/neotape_archiver_cmd.o src/neotape_tcp_server.o src/neotape_volume_server.o src/neotape_tcp_protocol.o src/neotape_format.o src/neotape_frame_builder.o src/neotape_socket_util.o src/neotape_common.o src/neotape_pax_writer.o src/neotape_bounded_buffer.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-raw-store : src/neotape_raw_store_cmd.o src/neotape_volume_server.o src/neotape_tcp_protocol.o src/neotape_format.o src/neotape_frame_builder.o src/neotape_socket_util.o src/neotape_common.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-write : src/neotape_write_cmd.o src/neotape_tcp_protocol.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o src/neotape_socket_util.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-read : src/neotape_read_cmd.o src/neotape_tcp_protocol.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o src/neotape_socket_util.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-extractor : src/neotape_extractor_cmd.o src/neotape_extractor.o src/neotape_validate.o src/neotape_format.o src/neotape_tcp_protocol.o src/neotape_common.o src/neotape_socket_util.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-inspect : src/neotape_inspect_cmd.o src/neotape_validate.o src/neotape_format.o src/neotape_tape.o src/neotape_common.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BINDIR)/neotape-scan : src/neotape_scan_cmd.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

test: $(BINDIR)/test_pax_pipeline $(BINDIR)/test_tcp_protocol $(BINDIR)/test_format $(BINDIR)/test_validate $(BINDIR)/mt-pax $(BINDIR)/neotape-plan $(BINDIR)/neotape-archiver $(BINDIR)/neotape-raw-store $(BINDIR)/neotape-write $(BINDIR)/neotape-read $(BINDIR)/neotape-inspect $(BINDIR)/neotape-scan
	$(BINDIR)/test_pax_pipeline
	$(BINDIR)/test_tcp_protocol
	$(BINDIR)/test_format
	$(BINDIR)/test_validate
	sh tests/smoke_mt_pax_pipeline.sh
	sh tests/smoke_tcp_archive.sh
	sh tests/smoke_tcp_archive_multi.sh
	sh tests/smoke_raw_store.sh
	sh tests/smoke_mt_pax_parity.sh
	sh tests/smoke_inspect.sh
	sh tests/smoke_scan.sh
	sh tests/smoke_tcp_extract.sh
	sh tests/smoke_tcp_plan_extract.sh
	sh tests/smoke_tcp_extract_multi.sh
	sh tests/smoke_plan_hardlink.sh

test_pax_cli: $(BINDIR)/mt-pax
	sh tests/smoke_mt_pax_pipeline.sh

$(BINDIR)/% : src/%.c $(B3LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
fix:
	$(CLANG_TIDY) --fix $(SRC_FILES) -- $(CXXFLAGS)
	$(CLANG_FORMAT) -i $(SRC_FILES)
clean:
	-rm -f ${EXE} ${BINDIR}/*.o src/*.o tests/*.o $(B3LIB) $(B3OBJ)
