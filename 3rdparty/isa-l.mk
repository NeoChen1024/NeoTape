ISALDIR = 3rdparty/isa-l
ISALBUILDDIR = $(BUILDDIR)/isa-l
ISALLIB = $(LIBDIR)/libisal.a

$(ISALLIB): $(ISALDIR)/Makefile.unx $(ISALDIR)/make.inc | $(LIBDIR)
	$(MAKE) -C $(ISALDIR) -f Makefile.unx \
		O=$(abspath $(ISALBUILDDIR)) \
		lib_name=$(abspath $@) \
		CC="$(CC)" \
		lib
