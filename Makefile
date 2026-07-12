# shellcheck disable=SC1007,SC2037,SC2046,SC2068,SC2091,SC2283
CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
DEPFLAGS = -MMD -MP
CFLAGS	= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(DEPFLAGS) $(INCS)
CXXFLAGS= -O2 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(DEPFLAGS) $(INCS)
SYS_LIBS = -larchive
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
OUTPUTDIR= output
BOT_BUNDLE = $(OUTPUTDIR)/bot.tar

include 3rdparty/blake3.mk
include 3rdparty/isa-l.mk
include 3rdparty/signify.mk

THIRDPARTY_LIBS = $(B3LIB) $(ISALLIB) $(SIGNIFYLIB)
CORE_LIBS = $(B3LIB)
SIGNED_LIBS = $(B3LIB) $(SIGNIFYLIB)

MAIN_BINS = \
	$(BINDIR)/mt-pax \
	$(BINDIR)/neotape-plan \
	$(BINDIR)/neotape-archiver \
	$(BINDIR)/neotape-write \
	$(BINDIR)/neotape-read \
	$(BINDIR)/neotape-extractor \
	$(BINDIR)/neotape-raw-store \
	$(BINDIR)/neotape-inspect \
	$(BINDIR)/neotape-scan

TEST_BINS = \
	$(BINDIR)/test_pax_pipeline \
	$(BINDIR)/test_tcp_protocol \
	$(BINDIR)/test_format \
	$(BINDIR)/test_validate \
	$(BINDIR)/test_signature

EXE = $(MAIN_BINS) $(TEST_BINS)

UNIT_TESTS = \
	$(BINDIR)/test_pax_pipeline \
	$(BINDIR)/test_tcp_protocol \
	$(BINDIR)/test_format \
	$(BINDIR)/test_validate \
	$(BINDIR)/test_signature

SMOKE_TESTS = \
	tests/smoke_mt_pax_pipeline.sh \
	tests/smoke_tcp_archive.sh \
	tests/smoke_tcp_archive_multi.sh \
	tests/smoke_raw_store.sh \
	tests/smoke_mt_pax_parity.sh \
	tests/smoke_inspect.sh \
	tests/smoke_scan.sh \
	tests/smoke_tcp_extract.sh \
	tests/smoke_signed_tcp_extract.sh \
	tests/smoke_writer_auth_fail.sh \
	tests/smoke_tcp_plan_extract.sh \
	tests/smoke_tcp_extract_multi.sh \
	tests/smoke_plan_hardlink.sh

CLANG_FORMAT ?= clang-format
SRC_FILES = $(wildcard src/*.cpp include/neotape/*.hpp)
CLANG_TIDY ?= clang-tidy

.PHONY: all clean countline fix test test-build test-unit test-smoke test_pax_cli compile_commands bot_bundle

compile_commands.json:
	python3 scripts/gen_compile_commands.py

compile_commands: compile_commands.json

all: $(THIRDPARTY_LIBS) $(EXE)

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(OUTPUTDIR):
	mkdir -p $@

src/%.o : src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

define make_cxx_binary
$(1): $$($(2)) $$($(3)) | $(BINDIR)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(SYS_LIBS)
endef

define make_cpp_test
$(1): $(2) $$($(3)) $$($(4)) | $(BINDIR)
	$$(CXX) $$(CXXFLAGS) $$< $$(filter %.o,$$^) -o $$@ $$($(5))
endef

MT_PAX_OBJS = src/mt-pax.o src/neotape_pax_writer.o src/neotape_common.o src/neotape_bounded_buffer.o
NEOTAPE_PLAN_OBJS = src/neotape_plan_cmd.o src/neotape_format.o src/neotape_common.o
NEOTAPE_ARCHIVER_OBJS = src/neotape_archiver_cmd.o src/neotape_tcp_server.o src/neotape_volume_server.o src/neotape_tcp_protocol.o src/neotape_format.o src/neotape_frame_builder.o src/neotape_signature.o src/neotape_socket_util.o src/neotape_common.o src/neotape_pax_writer.o src/neotape_bounded_buffer.o
NEOTAPE_RAW_STORE_OBJS = src/neotape_raw_store_cmd.o src/neotape_volume_server.o src/neotape_tcp_protocol.o src/neotape_format.o src/neotape_frame_builder.o src/neotape_signature.o src/neotape_socket_util.o src/neotape_common.o
NEOTAPE_WRITE_OBJS = src/neotape_write_cmd.o src/neotape_signature.o src/neotape_validate.o src/neotape_tcp_protocol.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o src/neotape_socket_util.o
NEOTAPE_READ_OBJS = src/neotape_read_cmd.o src/neotape_tcp_protocol.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o src/neotape_socket_util.o
NEOTAPE_EXTRACTOR_OBJS = src/neotape_extractor_cmd.o src/neotape_extractor.o src/neotape_validate.o src/neotape_signature.o src/neotape_format.o src/neotape_tcp_protocol.o src/neotape_common.o src/neotape_socket_util.o
NEOTAPE_INSPECT_OBJS = src/neotape_inspect_cmd.o src/neotape_validate.o src/neotape_signature.o src/neotape_format.o src/neotape_tape.o src/neotape_common.o
NEOTAPE_SCAN_OBJS = src/neotape_scan_cmd.o src/neotape_tape.o src/neotape_format.o src/neotape_common.o

TEST_TCP_PROTOCOL_OBJS = src/neotape_tcp_protocol.o
TEST_FORMAT_OBJS = src/neotape_format.o
TEST_VALIDATE_OBJS = src/neotape_validate.o src/neotape_format.o
TEST_SIGNATURE_OBJS = src/neotape_frame_builder.o src/neotape_signature.o src/neotape_format.o

$(eval $(call make_cxx_binary,$(BINDIR)/mt-pax,MT_PAX_OBJS,CORE_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-plan,NEOTAPE_PLAN_OBJS,CORE_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-archiver,NEOTAPE_ARCHIVER_OBJS,SIGNED_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-raw-store,NEOTAPE_RAW_STORE_OBJS,SIGNED_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-write,NEOTAPE_WRITE_OBJS,SIGNED_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-read,NEOTAPE_READ_OBJS,CORE_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-extractor,NEOTAPE_EXTRACTOR_OBJS,SIGNED_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-inspect,NEOTAPE_INSPECT_OBJS,SIGNED_LIBS))
$(eval $(call make_cxx_binary,$(BINDIR)/neotape-scan,NEOTAPE_SCAN_OBJS,CORE_LIBS))

$(BINDIR)/test_pax_pipeline: tests/test_pax_pipeline.cpp include/neotape/closable_queue.hpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(eval $(call make_cpp_test,$(BINDIR)/test_tcp_protocol,tests/test_tcp_protocol.cpp,TEST_TCP_PROTOCOL_OBJS,CORE_LIBS,CORE_LIBS))
$(eval $(call make_cpp_test,$(BINDIR)/test_format,tests/test_format.cpp,TEST_FORMAT_OBJS,CORE_LIBS,CORE_LIBS))
$(eval $(call make_cpp_test,$(BINDIR)/test_validate,tests/test_validate.cpp,TEST_VALIDATE_OBJS,CORE_LIBS,CORE_LIBS))
$(eval $(call make_cpp_test,$(BINDIR)/test_signature,tests/test_signature.cpp,TEST_SIGNATURE_OBJS,SIGNED_LIBS,SIGNED_LIBS))

test: test-unit test-smoke

test-build: $(EXE)

test-unit: $(UNIT_TESTS)
	$(BINDIR)/test_pax_pipeline
	$(BINDIR)/test_tcp_protocol
	$(BINDIR)/test_format
	$(BINDIR)/test_validate
	$(BINDIR)/test_signature

test-smoke: $(MAIN_BINS) $(SMOKE_TESTS)
	@for test_script in $(SMOKE_TESTS); do \
		sh $$test_script; \
	done

test_pax_cli: $(BINDIR)/mt-pax
	sh tests/smoke_mt_pax_pipeline.sh

bot_bundle: $(BOT_BUNDLE)

$(BOT_BUNDLE): | $(OUTPUTDIR)
	tar -cf $@ \
		--exclude-vcs \
		--exclude='./bin' \
		--exclude='./build' \
		--exclude='./output' \
		--exclude='./compile_commands.json' \
		--exclude='*.o' \
		--exclude='*.a' \
		.

$(BINDIR)/% : src/%.c $(B3LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(CORE_LIBS) $(SYS_LIBS)


countline:
	scc .
fix:
	$(CLANG_TIDY) --fix $(SRC_FILES) -- $(CXXFLAGS)
	$(CLANG_FORMAT) -i $(SRC_FILES)
clean:
	-rm -f $(EXE) $(BINDIR)/*.o src/*.o src/*.d tests/*.o tests/*.d $(THIRDPARTY_LIBS) $(B3OBJ) $(SIGNIFYOBJ) $(BOT_BUNDLE)
	-rm -rf $(ISALBUILDDIR)

-include $(wildcard src/*.d tests/*.d)
