ACLOCAL_AMFLAGS = -I m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = sredis.pc

SREDIS_VERSION = 1:0:0

lib_LTLIBRARIES = libsredis-1.0.la

libsredis_1_0_la_SOURCES = \
	sredis.h sredis.c \
	xerror.h xerror.c

libsredis_1_0_la_LDFLAGS = -version-info $(SREDIS_VERSION)
libsredis_1_0_la_LIBADD = $(HIREDIS_LIBS)
libsredis_1_0_la_CPPFLAGS = $(HIREDIS_CPPFLAGS)

include_HEADERS = sredis.h

noinst_PROGRAMS = sredis-example sredis-transaction

sredis_example_SOURCES = sredis-example.c
sredis_example_LDADD = ./libsredis-1.0.la $(HIREDIS_LIBS)

sredis_transaction_SOURCES = sredis-transaction.c
sredis_transaction_LDADD = ./libsredis-1.0.la $(HIREDIS_LIBS) -lpthread
