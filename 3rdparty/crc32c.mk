CRC32CDIR	= 3rdparty/crc32c
CRC32CARCH	:= $(shell uname -m)
CRC32CBASE	= $(CRC32CDIR)/src/crc32c.cc $(CRC32CDIR)/src/crc32c_portable.cc
CRC32CSIMD	=

ifneq (,$(filter x86_64 amd64,$(CRC32CARCH)))
CRC32CSIMD	+= $(CRC32CDIR)/src/crc32c_sse42.cc
else ifneq (,$(filter aarch64 arm64,$(CRC32CARCH)))
CRC32CSIMD	+= $(CRC32CDIR)/src/crc32c_arm64.cc
endif

CRC32CSRC	= $(CRC32CBASE) $(CRC32CSIMD)
CRC32COBJ	= $(CRC32CSRC:$(CRC32CDIR)/src/%.cc=$(BUILDDIR)/crc32c/%.o)
CRC32CLIB	= $(LIBDIR)/libcrc32c.a

$(CRC32CLIB): $(CRC32COBJ) | $(LIBDIR)
	$(AR) rcs $@ $^

$(BUILDDIR)/crc32c/%.o: $(CRC32CDIR)/src/%.cc Makefile 3rdparty/crc32c.mk | $(BUILDDIR)/crc32c
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/crc32c/crc32c_sse42.o: $(CRC32CDIR)/src/crc32c_sse42.cc Makefile 3rdparty/crc32c.mk | $(BUILDDIR)/crc32c
	$(CXX) $(CXXFLAGS) -msse4.2 -c $< -o $@
