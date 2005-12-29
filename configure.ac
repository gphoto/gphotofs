AC_INIT(gphotofs, 0.0.99)
AM_INIT_AUTOMAKE(AC_PACKAGE_NAME, AC_PACKAGE_VERSION)
AM_CONFIG_HEADER(config.h)

AC_PROG_CC

PKG_CHECK_MODULES(FUSE, fuse >= 2.2 \
                        glib-2.0 >= 2.6 \
                        libgphoto2 >= 2.1)
AC_SUBST(FUSE_CFLAGS)
AC_SUBST(FUSE_LIBS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
