SIGNIFYDIR = 3rdparty/signify
SIGNIFYLIB = $(LIBDIR)/libsignify.a
SIGNIFYFLAGS = -Iinclude/signify -isystem include/signify/libbsd/bsd \
	       -include $(SIGNIFYDIR)/compat.h -DLIBBSD_OVERLAY \
	       -DBUNDLED_BZERO -Dtypeof=__typeof__
SIGNIFYSRC = \
	$(SIGNIFYDIR)/base64.c \
	$(SIGNIFYDIR)/bcrypt_pbkdf.c \
	$(SIGNIFYDIR)/blowfish.c \
	$(SIGNIFYDIR)/crypto_api.c \
	$(SIGNIFYDIR)/explicit_bzero.c \
	$(SIGNIFYDIR)/fe25519.c \
	$(SIGNIFYDIR)/libbsd/readpassphrase.c \
	$(SIGNIFYDIR)/mod_ed25519.c \
	$(SIGNIFYDIR)/mod_ge25519.c \
	$(SIGNIFYDIR)/sc25519.c \
	$(SIGNIFYDIR)/sha2.c \
	$(SIGNIFYDIR)/timingsafe_bcmp.c
SIGNIFYOBJ = $(SIGNIFYSRC:$(SIGNIFYDIR)/%.c=$(BUILDDIR)/signify/%.o)

$(SIGNIFYLIB): $(SIGNIFYOBJ) | $(LIBDIR)
	$(AR) rcs $@ $^

$(BUILDDIR)/signify/%.o: $(SIGNIFYDIR)/%.c Makefile 3rdparty/signify.mk
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SIGNIFYFLAGS) -c $< -o $@
