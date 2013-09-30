## Process this file with automake to produce Makefile.in

SUBDIRS = gphoto-m4

ACLOCAL_AMFLAGS = -I m4 -I gphoto-m4

EXTRA_DIST = test-gphotofs.sh

bin_PROGRAMS = gphotofs
gphotofs_SOURCES = gphotofs.c
gphotofs_CPPFLAGS = \
	$(AM_CPPFLAGS) $(CPPFLAGS) \
	-DFUSE_USE_VERSION=25 $(FUSE_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(LIBGPHOTO2_CFLAGS)
gphotofs_LDADD = \
	$(FUSE_LIBS) $(GLIB_LIBS) \
	$(LIBGPHOTO2_LIBS)

TESTS_ENVIRONMENT = testdir=test-realdir srcdir=$(srcdir) mountpoint=test-mp
TESTS = # test-gphotofs.sh
