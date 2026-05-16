B3DIR	= 3rdparty/BLAKE3/c
B3ARCH	:= $(shell uname -m)
B3FLAGS	=
B3BASESRC = $(B3DIR)/blake3.c $(B3DIR)/blake3_dispatch.c $(B3DIR)/blake3_portable.c
B3SIMDSRC =

ifneq (,$(filter x86_64 amd64 i386 i686,$(B3ARCH)))
B3SIMDSRC += $(B3DIR)/blake3_sse2.c
B3SIMDSRC += $(B3DIR)/blake3_sse41.c
B3SIMDSRC += $(B3DIR)/blake3_avx2.c
B3SIMDSRC += $(B3DIR)/blake3_avx512.c
else ifneq (,$(filter aarch64 arm64,$(B3ARCH)))
B3SIMDSRC += $(B3DIR)/blake3_neon.c
else
B3FLAGS	+= -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0
endif

B3SRC	= $(B3BASESRC) $(B3SIMDSRC)
B3OBJ	= $(B3SRC:$(B3DIR)/%.c=$(BUILDDIR)/blake3/%.o)
B3LIB	= $(LIBDIR)/libb3sum.a

$(B3LIB): $(B3OBJ) | $(LIBDIR)
	$(AR) rcs $@ $^

$(BUILDDIR)/blake3/%.o: $(B3DIR)/%.c Makefile 3rdparty/blake3.mk | $(BUILDDIR)/blake3
	$(CC) $(CFLAGS) $(B3FLAGS) -c $< -o $@

$(BUILDDIR)/blake3/blake3_sse2.o: $(B3DIR)/blake3_sse2.c Makefile 3rdparty/blake3.mk | $(BUILDDIR)/blake3
	$(CC) $(CFLAGS) $(B3FLAGS) -msse2 -c $< -o $@

$(BUILDDIR)/blake3/blake3_sse41.o: $(B3DIR)/blake3_sse41.c Makefile 3rdparty/blake3.mk | $(BUILDDIR)/blake3
	$(CC) $(CFLAGS) $(B3FLAGS) -msse4.1 -c $< -o $@

$(BUILDDIR)/blake3/blake3_avx2.o: $(B3DIR)/blake3_avx2.c Makefile 3rdparty/blake3.mk | $(BUILDDIR)/blake3
	$(CC) $(CFLAGS) $(B3FLAGS) -mavx2 -c $< -o $@

$(BUILDDIR)/blake3/blake3_avx512.o: $(B3DIR)/blake3_avx512.c Makefile 3rdparty/blake3.mk | $(BUILDDIR)/blake3
	$(CC) $(CFLAGS) $(B3FLAGS) -mavx512f -mavx512vl -c $< -o $@
