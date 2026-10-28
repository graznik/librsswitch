# Builds the library for one board into build/<board>/: the shared
# library librsswitch.so.MAJOR.MINOR.PATCH with the links
# librsswitch.so.MAJOR and librsswitch.so, and the static library
# librsswitch.a, both made from the same objects.
#
#   make                      the libgpiod board, in build/gpiod/
#   make BOARD=stub           the stub board, in build/stub/
#   make install              the library of BOARD, its header and its
#                             pkg-config file, under PREFIX
#
# The two boards build into separate directories, so building one leaves
# the other in place and switching needs no make clean. BUILD names the
# directory of one board, for a build that must not write below the
# sources.

# Which board is built. A board is chosen by the source file that is
# compiled, not by a define.
BOARD ?= gpiod

# The prefix of the cross toolchain, for example aarch64-linux-gnu-.
# Empty means a native build; nothing here guesses the architecture.
CROSS_COMPILE ?=

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar

# Where install puts the files. DESTDIR is prepended to every path and
# is meant for staging a package: it is not part of the paths that
# librsswitch.pc records. On Debian systems with PREFIX=/usr the library
# belongs in the multiarch directory, so LIBDIR is set by the caller;
# nothing here detects it.
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# The version of the library is kept once, in librsswitch.h, as three
# numbers on lines of their own. The file names and the SONAME come
# from there. The awk program has no hash sign: a hash inside a
# function call is taken for a comment by some versions of make.
MAJOR := $(shell awk '$$2 == "LIBRSSWITCH_VERSION_MAJOR" { print $$3 }' librsswitch.h)
MINOR := $(shell awk '$$2 == "LIBRSSWITCH_VERSION_MINOR" { print $$3 }' librsswitch.h)
PATCH := $(shell awk '$$2 == "LIBRSSWITCH_VERSION_PATCH" { print $$3 }' librsswitch.h)
ifeq ($(MAJOR),)
$(error LIBRSSWITCH_VERSION_MAJOR not found in librsswitch.h)
endif
VERSION := $(MAJOR).$(MINOR).$(PATCH)

# Forces the libgpiod API choice of boards/gpiod/gpiod_board.c when its
# header probe picks the wrong one, for example when cross-compiling
# against a sysroot whose gpiod.h cannot be told apart from the probe
# alone. 1 means libgpiod 1.x (gpiod_line_*), 2 means libgpiod 2.x
# (gpiod_line_request_*). Unset leaves the probe in charge.
LIBGPIOD_VERSION ?=

# The flags the library cannot be built without live in their own
# variables. A CFLAGS given on the command line replaces every
# assignment to CFLAGS in a makefile, which would drop -fPIC and
# -fvisibility=hidden and break the shared library. User flags come
# after these, and CPPFLAGS and LDFLAGS are the place for the include
# and library paths of a libgpiod that is not installed system-wide.
# -g is on so that the ABI of the library can be read from the debug
# information.
RSS_CFLAGS := -Werror -Wall -W -pedantic
RSS_CFLAGS += -pthread
RSS_CFLAGS += -D_FORTIFY_SOURCE=2 -O2
RSS_CFLAGS += -funsigned-char -Wpointer-arith
RSS_CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes
RSS_CFLAGS += -Wwrite-strings -Wcast-qual -Wcast-align
RSS_CFLAGS += -Wbad-function-cast -Wformat-security -Wformat-nonliteral
RSS_CFLAGS += -Wmissing-format-attribute -Winline
RSS_CFLAGS += -Wformat -Werror=format-security -Werror=array-bounds
RSS_CFLAGS += -fPIC -fvisibility=hidden -g

RSS_CPPFLAGS := -I.

ifeq ($(DEBUG),1)
RSS_CPPFLAGS += -DDEBUG
endif

ifneq ($(LIBGPIOD_VERSION),)
ifeq ($(filter $(LIBGPIOD_VERSION),1 2),)
$(error LIBGPIOD_VERSION must be 1 or 2)
endif
RSS_CPPFLAGS += -DLIBRSSWITCH_GPIOD_VERSION=$(LIBGPIOD_VERSION)
endif

# The sources of the library: the core and the board code, which is the
# only part that differs. The stub board parses its config like the
# libgpiod board does, so it shares gpiod_config.c.
SRCS := librsswitch.c boards/gpiod/gpiod_config.c
ifeq ($(BOARD),gpiod)
SRCS += boards/gpiod/gpiod_board.c
SO_LIBS := -lgpiod
else ifeq ($(BOARD),stub)
SRCS += boards/stub/stub.c
SO_LIBS :=
else
$(error BOARD must be gpiod or stub)
endif

# Where the files of BOARD are built. The default is a directory of its
# own below the sources. A checkout on a file system that cannot make
# symbolic links, or a build that must leave the source tree alone,
# names another directory, for example BUILD=/tmp/rsswitch/stub. A
# relative path is taken relative to the directory make runs in. The
# programs and the tests take the library from the same variable.
BUILD ?= build/$(BOARD)
ifeq ($(BUILD),)
$(error BUILD must not be empty)
endif

OBJS := $(SRCS:%.c=$(BUILD)/%.o)
DEPS := $(OBJS:.o=.d)

SONAME := librsswitch.so.$(MAJOR)
SOFILE := $(BUILD)/librsswitch.so.$(VERSION)
SOLINKS := $(BUILD)/$(SONAME) $(BUILD)/librsswitch.so
ARFILE := $(BUILD)/librsswitch.a
LIBFILES := $(SOFILE) $(SOLINKS) $(ARFILE)

.PHONY: all test clean install uninstall FORCE

all: $(LIBFILES)

# make does not notice a changed DEBUG, LIBGPIOD_VERSION, CROSS_COMPILE
# or flag, because the objects exist already. The flags are written to
# a stamp file, only when they differ from the file's content, and
# every object and library file depends on it, so such a change
# rebuilds the board's objects without make clean.
STAMP := $(CC) $(RSS_CFLAGS) $(CFLAGS) $(RSS_CPPFLAGS) $(CPPFLAGS) $(LDFLAGS) $(SO_LIBS)
QSTAMP := $(subst ','\'',$(STAMP))

$(BUILD)/.flags: FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(QSTAMP)' | cmp -s - $@ || printf '%s\n' '$(QSTAMP)' > $@

$(BUILD)/%.o: %.c $(BUILD)/.flags
	@mkdir -p $(dir $@)
	$(CC) $(RSS_CFLAGS) $(CFLAGS) $(RSS_CPPFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# -z defs turns a missing -lgpiod into a link error here instead of a
# failure when a program starts. No -rpath: where the loader looks for
# the library is decided by the system and by the user of the library,
# not by a path of the build tree.
$(SOFILE): $(OBJS) librsswitch.map $(BUILD)/.flags
	$(CC) -shared -pthread -Wl,-soname,$(SONAME) \
		-Wl,--version-script=librsswitch.map -Wl,-z,defs $(LDFLAGS) \
		-o $@ $(OBJS) $(SO_LIBS)

$(BUILD)/$(SONAME): $(SOFILE)
	ln -sf librsswitch.so.$(VERSION) $@

$(BUILD)/librsswitch.so: $(BUILD)/$(SONAME)
	ln -sf $(SONAME) $@

# The same position independent objects go into the archive. All
# functions of the board contract stay global in it by design; only the
# shared library is limited to the public interface.
$(ARFILE): $(OBJS) $(BUILD)/.flags
	rm -f $@
	$(AR) rcs $@ $(OBJS)

# install depends on the library files, so that one call builds what is
# missing and installs it, and a call after make only copies. Run make
# as the user first and then sudo make install: a bare sudo make install
# in a tree that was not built leaves root-owned files in build/.
#
# The stub library installs under the same names as the libgpiod one, so
# one replaces the other below the same PREFIX. Only the libgpiod
# library has a dependency, which librsswitch.pc names in its
# Requires.private line.
#
# The pkg-config file is made again on every install and the old one is
# removed first, so it never keeps the settings of an earlier install.
# The loader cache is refreshed only for a real install, not for a
# staged one. That needs root, so a failure only warns: with a LIBDIR
# outside the loader's search path the system needs a file in
# /etc/ld.so.conf.d, because no program carries an RPATH.
PC_FILTER :=
ifeq ($(BOARD),stub)
PC_FILTER := -e '/^Requires\.private:/d'
endif

LDCONFIG_WARNING := warning: ldconfig failed, run ldconfig as root so that $(LIBDIR) is searched for libraries

install: $(LIBFILES)
	install -D -m 0644 librsswitch.h "$(DESTDIR)$(INCLUDEDIR)/librsswitch.h"
	install -D -m 0755 $(SOFILE) "$(DESTDIR)$(LIBDIR)/librsswitch.so.$(VERSION)"
	ln -sf librsswitch.so.$(VERSION) "$(DESTDIR)$(LIBDIR)/$(SONAME)"
	ln -sf $(SONAME) "$(DESTDIR)$(LIBDIR)/librsswitch.so"
	install -D -m 0644 $(ARFILE) "$(DESTDIR)$(LIBDIR)/librsswitch.a"
	rm -f $(BUILD)/librsswitch.pc
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@LIBDIR@|$(LIBDIR)|g' \
		-e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
		-e 's|@VERSION@|$(VERSION)|g' $(PC_FILTER) \
		librsswitch.pc.in > $(BUILD)/librsswitch.pc
	install -D -m 0644 $(BUILD)/librsswitch.pc "$(DESTDIR)$(PKGCONFIGDIR)/librsswitch.pc"
	@if [ -z "$(DESTDIR)" ]; then \
		ldconfig || echo "$(LDCONFIG_WARNING)" >&2; \
	fi

# Removes the six paths that install writes and nothing else.
uninstall:
	rm -f "$(DESTDIR)$(INCLUDEDIR)/librsswitch.h" \
		"$(DESTDIR)$(LIBDIR)/librsswitch.so.$(VERSION)" \
		"$(DESTDIR)$(LIBDIR)/$(SONAME)" \
		"$(DESTDIR)$(LIBDIR)/librsswitch.so" \
		"$(DESTDIR)$(LIBDIR)/librsswitch.a" \
		"$(DESTDIR)$(PKGCONFIGDIR)/librsswitch.pc"
	@if [ -z "$(DESTDIR)" ]; then \
		ldconfig || echo "$(LDCONFIG_WARNING)" >&2; \
	fi

# The tests build their own copy of the sources, whatever BOARD is.
test:
	$(MAKE) -C tests test

# A BUILD outside the default is removed as well.
clean:
	rm -rf build
ifneq ($(BUILD),build/$(BOARD))
	rm -rf "$(BUILD)"
endif

-include $(DEPS)
