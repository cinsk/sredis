AC_INIT([sredis], [1.0])
AM_INIT_AUTOMAKE([foreign])
AC_PROG_CC

AC_CONFIG_MACRO_DIR([m4])

LT_INIT

AC_ARG_WITH([hiredis-include-path],
        [AS_HELP_STRING([--with-hiredis-include-path],
        [location of hiredis headers, defaults to /usr/include])],
        [HIREDIS_CFLAGS="-I$withval"],
        [HIREDIS_CFLAGS=''])
AC_SUBST([HIREDIS_CFLAGS])

AC_ARG_WITH([hiredis-lib-path],
        [AS_HELP_STRING([--with-hiredis-lib-path],
        [location of hiredis headers, defaults to /usr/lib])],
        [HIREDIS_LIBS="-L$withval -lhiredis"],
        [HIREDIS_LIBS='-lhiredis'])
AC_SUBST([HIREDIS_LIBS])

AC_CONFIG_SRCDIR([sredis.c])

AC_CONFIG_FILES([sredis.pc
                 Makefile])
        
AC_OUTPUT
