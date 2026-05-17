CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/pax bin/neotape-write bin/neotape-inspect bin/neotape-plan
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build
FORMAT_OBJ = $(BUILDDIR)/neotape_format.o

include 3rdparty/blake3.mk
include 3rdparty/crc32c.mk

.PHONY: all clean countline

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

$(BUILDDIR)/%.o : src/%.cpp Makefile | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BINDIR)/pax : src/pax.cpp Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BINDIR)/neotape-write : src/neotape_write.cpp $(FORMAT_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-inspect : src/neotape_inspect.cpp $(FORMAT_OBJ) Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< $(FORMAT_OBJ) -o $@ $(LDLIBS)

$(BINDIR)/neotape-plan : src/neotape_plan.cpp Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BINDIR)/% : src/%.c Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(FORMAT_OBJ) $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
