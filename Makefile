CC	= cc
CXX	= c++
INCS	= -Iinclude -Llib
CFLAGS	= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c17 -march=native -pedantic $(INCS)
CXXFLAGS= -O3 -g -Wall -Wextra -pipe -fPIE -fPIC -std=c++20 -march=native -pedantic $(INCS)
LDLIBS	= -larchive
EXE	= bin/pax
BINDIR	= bin
.PHONY: all clean countline

all: ${EXE}

$(BINDIR):
	mkdir -p $@

$(BINDIR)/% : src/%.cpp Makefile | $(BINDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BINDIR)/% : src/%.c Makefile | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)


countline:
	scc .
clean:
	-rm -f ${EXE} ${BINDIR}/*.o
