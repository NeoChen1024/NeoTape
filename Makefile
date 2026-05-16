CC	= cc
CXX	= c++
.DEFAULT_GOAL := all
INCS	= -Iinclude -I3rdparty/BLAKE3/c -Llib
CFLAGS	= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= lib/libb3sum.a -larchive
EXE	= bin/pax
BINDIR	= bin
LIBDIR	= lib
BUILDDIR= build

include 3rdparty/blake3.mk

.PHONY: all clean countline

all: ${EXE}

$(BINDIR) $(LIBDIR) $(BUILDDIR)/blake3:
	mkdir -p $@

$(BINDIR)/% : src/%.cpp Makefile $(B3LIB) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BINDIR)/% : src/%.c Makefile $(B3LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
clean:
	-rm -f ${EXE} ${BINDIR}/*.o $(B3LIB) $(B3OBJ)
