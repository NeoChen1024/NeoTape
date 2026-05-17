CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -Llib -I/usr/local/include -Lusr/local/lib
CFLAGS	= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a lib/libcrc32c.a -larchive
EXE	= bin/pax
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build

include 3rdparty/blake3.mk
include 3rdparty/crc32c.mk

.PHONY: all clean countline

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR)/blake3 $(BUILDDIR)/crc32c:
	mkdir -p $@

$(BINDIR)/% : src/%.cpp Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BINDIR)/% : src/%.c Makefile $(B3LIB) $(CRC32CLIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(B3LIB) $(B3OBJ) $(CRC32CLIB) $(CRC32COBJ)
