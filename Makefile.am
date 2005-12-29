## Process this file with automake to produce Makefile.in

bin_PROGRAMS = gphotofs
gphotofs_SOURCES = gphotofs.c
gphotofs_CPPFLAGS = -DFUSE_USE_VERSION=22 $(FUSE_CFLAGS)
gphotofs_LDADD = $(FUSE_LIBS)
