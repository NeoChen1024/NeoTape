CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/pax bin/mt-pax bin/neotape-write bin/neotape-inspect bin/neotape-plan bin/neotape-cat-volumes bin/test_tape bin/neotape-init
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
FORMAT_OBJ = $(BUILDDIR)/neotape_format.o
COMMON_OBJ = $(BUILDDIR)/neotape_common.o
BOUNDEDBUF_OBJ = $(BUILDDIR)/neotape_bounded_buffer.o
TAPE_OBJ = $(BUILDDIR)/neotape_tape.o
NAV_OBJ  = $(BUILDDIR)/neotape_tape_navigator.o
TEST_DEVICE_OBJ = $(BUILDDIR)/neotape_tape_test_device.o
TAPE_WRITER_OBJ = $(BUILDDIR)/neotape_tape_writer.o

include 3rdparty/blake3.mk
include 3rdparty/crc32c.mk

.PHONY: all clean countline test

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

$(BUILDDIR)/%.o : src/%.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/pax : src/pax.cpp $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(COMMON_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/mt-pax : src/mt-pax.cpp $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(COMMON_OBJ) $(BOUNDEDBUF_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-write : src/neotape_write.cpp $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TAPE_WRITER_OBJ) -o $@ $(LDLIBS)

$(TAPE_OBJ) : src/neotape_tape.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(NAV_OBJ) : src/neotape_tape_navigator.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TAPE_WRITER_OBJ) : src/neotape_tape_writer.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_DEVICE_OBJ) : tests/tape_test_device.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/neotape-inspect : src/neotape_inspect.cpp $(FORMAT_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-plan : src/neotape_plan.cpp $(COMMON_OBJ) Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(COMMON_OBJ) -o $@

$(BINDIR)/neotape-cat-volumes : src/neotape_cat_volumes.cpp $(BUILDDIR)/neotape_reader.o $(FORMAT_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(BUILDDIR)/neotape_reader.o $(FORMAT_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

# neotape-init
$(BINDIR)/neotape-init : src/neotape_init.cpp $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(TAPE_OBJ) $(COMMON_OBJ) -o $@ $(LDLIBS)

# Test binary
$(BINDIR)/test_tape : tests/test_tape.cpp $(FORMAT_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) -o $@ $(LDLIBS)

test: $(BINDIR)/test_tape
	$(BINDIR)/test_tape

$(BINDIR)/% : src/%.c Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(FORMAT_OBJ) $(COMMON_OBJ) $(TAPE_OBJ) $(NAV_OBJ) $(TEST_DEVICE_OBJ) $(TAPE_WRITER_OBJ) $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
