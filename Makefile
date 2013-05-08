
PREFIX ?= /usr/local

export PREFIX

CFLAGS = -Wall -Wextra -fPIC -Ihiredis
LDFLAGS = -Lhiredis -lhiredis

INSTALL=$(shell which install)

INCLUDE_PATH?=include/hiredis
LIBRARY_PATH?=lib
INSTALL_INCLUDE_PATH= $(PREFIX)/$(INCLUDE_PATH)
INSTALL_LIBRARY_PATH= $(PREFIX)/$(LIBRARY_PATH)

OBJS = sredis.o xerror.o

VERSION_MINOR=1
VERSION_MAJOR=0

LIBNAME=libsredis
DYLIBSUFFIX=so

STLIBSUFFIX=a
DYLIB_MINOR_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(VERSION_MAJOR).$(VERSION_MINOR)
DYLIB_MAJOR_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(VERSION_MAJOR)
DYLIBNAME=$(LIBNAME).$(DYLIBSUFFIX)
DYLIB_MAKE_CMD=$(CC) -shared -Wl,-soname,$(DYLIB_MINOR_NAME) -o $(DYLIBNAME) $(LDFLAGS)
STLIBNAME=$(LIBNAME).$(STLIBSUFFIX)
STLIB_MAKE_CMD=ar rcs $(STLIBNAME)

.PHONY: hiredis hiredis-clean hiredis-install all clean install

all: hiredis $(DYLIBNAME) $(STLIBNAME) sredis-example sredis-benchmark

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(DYLIBNAME): $(OBJS)
	$(DYLIB_MAKE_CMD) $(OBJS)

$(STLIBNAME): $(OBJS)
	$(STLIB_MAKE_CMD) $(OBJS)

dynamic: $(DYLIBNAME)
static: $(STLIBNAME)

sredis-example: sredis-example.o $(DYLIBNAME)
	$(CC) -o sredis-example sredis-example.o $(OBJS) \
		-Wl,-rpath=$(INSTALL_LIBRARY_PATH) \
		-L. -lsredis $(LDFLAGS)

sredis-benchmark: sredis-benchmark.o $(DYLIBNAME)
	$(CC) -o sredis-benchmark sredis-benchmark.o $(OBJS) \
		-Wl,-rpath=$(INSTALL_LIBRARY_PATH) \
		-L. -lsredis $(LDFLAGS) -lrt

sredis-example.o sredis-benchmark.o: %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

hiredis:
	$(MAKE) -C hiredis

hiredis-clean:
	$(MAKE) -C hiredis clean

hiredis-install:
	$(MAKE) -C hiredis install

clean: hiredis-clean
	rm -f $(OBJS)
	rm -f sredis-example.o sredis-example

install: all hiredis-install
	mkdir -p $(PREFIX)/libexec/sredis
	mkdir -p $(PREFIX)/include
	mkdir -p $(PREFIX)/lib

	$(INSTALL) sredis.h $(PREFIX)/include
	$(INSTALL) sredis-example $(PREFIX)/libexec/sredis
	$(INSTALL) $(DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(DYLIB_MINOR_NAME)
	cd $(INSTALL_LIBRARY_PATH) && ln -sf $(DYLIB_MINOR_NAME) $(DYLIB_MAJOR_NAME)
	cd $(INSTALL_LIBRARY_PATH) && ln -sf $(DYLIB_MAJOR_NAME) $(DYLIBNAME)
	$(INSTALL) $(STLIBNAME) $(INSTALL_LIBRARY_PATH)
