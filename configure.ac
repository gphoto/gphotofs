AC_PREREQ(2.59)
AC_INIT([gphoto filesystem], [0.5], [gphoto-devel@lists.sourceforge.net], [gphotofs])
AC_CONFIG_SRCDIR([gphotofs.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([auto-aux])
dnl AC_CONFIG_MACRO_DIR([auto-m4])
AM_INIT_AUTOMAKE([gnu 1.8 dist-bzip2])

if test ! -d "$srcdir/m4m"; then
AC_MSG_ERROR([
You are missing the m4m/ directory in your top
$PACKAGE_TARNAME source directory.

You are probably using an ill-maintained SVN tree.
Running

    cd $srcdir
    svn up

and re-running autogen.sh might help.
])
fi

GP_CHECK_SHELL_ENVIRONMENT

AC_PROG_CC
AC_PROG_CPP
AM_PROG_LIBTOOL
AC_PROG_INSTALL
AM_PROG_CC_C_O

GP_PKG_CONFIG

PKG_CHECK_MODULES([FUSE], [fuse >= 2.5])
AC_SUBST([FUSE_CFLAGS])
AC_SUBST([FUSE_LIBS])

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.6])
AC_SUBST([GLIB_CFLAGS])
AC_SUBST([GLIB_LIBS])

GP_CHECK_LIBRARY([LIBGPHOTO2], [libgphoto2], [>= 2.5])

ALL_LINGUAS=""
GETTEXT_PACKAGE="gphotofs"
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE",
                   [Define to the Gettext package name.])
AM_GLIB_GNU_GETTEXT

AC_CONFIG_FILES([
Makefile
m4m/Makefile
])
AC_OUTPUT
GP_CONFIG_OUTPUT
