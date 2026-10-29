# Builds the library for one board into build/<board>/: the shared
# library librsswitch.so.MAJOR.MINOR.PATCH with the links
# librsswitch.so.MAJOR and librsswitch.so, and the static library
# librsswitch.a, in which only the public functions are global names.
# The unit tests link a third file, librsswitch-internal.a, which keeps
# the objects as they are and is never installed.
#
#   make                      the libgpiod board, in build/gpiod/
#   make BOARD=stub           the stub board, in build/stub/
#   make install              the library of BOARD, its header and its
#                             pkg-config file, under PREFIX
#   make abicheck             compare the shared library with the
#                             checked-in ABI baseline (optional, needs
#                             abidiff)
#   make abi-baseline         write a new baseline, only for an
#                             intended change of the interface
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
# objcopy, nm and readelf make and check the installed static library.
# A cross build takes them from the same prefix as the compiler, never
# from the host.
OBJCOPY := $(CROSS_COMPILE)objcopy
NM := $(CROSS_COMPILE)nm
READELF := $(CROSS_COMPILE)readelf

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

# Every library source of every board. make clean removes the objects
# of all of them from BUILD whatever BOARD is, because one BUILD may
# have held either board. A new board adds its source here as well as
# in the chain above.
ALL_SRCS := librsswitch.c boards/gpiod/gpiod_config.c boards/gpiod/gpiod_board.c boards/stub/stub.c

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
STATIC_OBJ := $(BUILD)/librsswitch-static.o
INTERNAL_ARFILE := $(BUILD)/librsswitch-internal.a
LIBFILES := $(SOFILE) $(SOLINKS) $(ARFILE)

.PHONY: all test clean install uninstall abicheck abi-baseline FORCE

all: $(LIBFILES) $(INTERNAL_ARFILE)

# make does not notice a changed DEBUG, LIBGPIOD_VERSION, CROSS_COMPILE
# or flag, because the objects exist already. The flags are written to
# a stamp file, only when they differ from the file's content, and
# every object and library file depends on it, so such a change
# rebuilds the board's objects without make clean. The tools and the
# options that make the static library are part of the stamp as well,
# so that a tree built with other ones makes its archive again.
ARCHIVE_LOCALIZE := --localize-hidden --wildcard --keep-global-symbol='rss_*'
STAMP := $(CC) $(AR) $(OBJCOPY) $(ARCHIVE_LOCALIZE) $(RSS_CFLAGS) $(CFLAGS) $(RSS_CPPFLAGS) $(CPPFLAGS) $(LDFLAGS) $(SO_LIBS)
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

# The members of an archive are linked as they are, so
# -fvisibility=hidden does not keep the internal names out of a static
# link. board_init(), transmit() and the other functions of the board,
# the config parser and the test hooks of the stub would clash with a
# function of the same name in the program. A program that defined all
# of the board functions would even replace the board of the library.
#
# So the library is first linked into one relocatable object, in which
# the references between its own files point at definitions in the same
# object. objcopy then makes every name local except the public
# functions:
#   --localize-hidden takes the names that -fvisibility=hidden keeps out
#   of the shared library.
#   --keep-global-symbol with the rss_* pattern of librsswitch.map keeps
#   only the interface global, even if a CFLAGS given on the command
#   line changed the visibility.
# The relocatable link goes through the compiler, with the flags of the
# objects, so that it uses the ABI they were built for. With -flto in
# CFLAGS the objects hold intermediate code whose symbol table objcopy
# does not change; -flinker-output=nolto-rel compiles it into machine
# code first. The recipe refuses an object that still holds such code or
# a global name outside the interface. The archive therefore defines
# what the shared library exports and nothing else, and it is what
# install copies. Should a step fail, the archive is gone and the next
# make runs every step again.
$(ARFILE): $(OBJS) $(BUILD)/.flags
	rm -f $@
	$(CC) $(RSS_CFLAGS) $(CFLAGS) -r -nostdlib -flinker-output=nolto-rel \
		-o $(STATIC_OBJ) $(OBJS)
	$(OBJCOPY) $(ARCHIVE_LOCALIZE) $(STATIC_OBJ)
	@if $(READELF) -S $(STATIC_OBJ) | grep -q '\.gnu\.lto_'; then \
		echo "$(STATIC_OBJ) still holds LTO code" >&2; \
		exit 1; \
	fi
	@$(NM) -g --defined-only $(STATIC_OBJ) | \
		awk 'NF == 3 && $$3 !~ /^rss_/ { print "global name in $(STATIC_OBJ): " $$3; bad = 1 } END { exit bad }' >&2
	$(AR) rcs $@ $(STATIC_OBJ)

# This archive is for the unit tests only and is never installed. The
# tests call the board functions and the test hooks of the stub
# directly and put a board of their own in front of the archive. That
# needs the objects as separate members with their global names.
$(INTERNAL_ARFILE): $(OBJS) $(BUILD)/.flags
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

# The tests link the stub library, whatever BOARD is. They build it in
# BUILD when BOARD is the stub, and in build/stub otherwise, so that a
# BUILD meant for the libgpiod board never receives the stub.
ifeq ($(BOARD),stub)
TESTBUILD := $(abspath $(BUILD))
else
TESTBUILD := $(abspath build/stub)
endif

test:
	$(MAKE) -C tests test BUILD=$(TESTBUILD)

# The binary interface of the shared library is compared with a
# baseline written by abidw from an earlier build and checked in. The
# check is optional: it needs abidiff from the abigail-tools package,
# and without the tool, or without a baseline for the machine, it says
# so in one line and succeeds. A baseline describes one architecture,
# because sizes and alignments differ between them, so there is one file
# per machine, named after the first field of the compiler's target.
# The baseline is written only by make abi-baseline, when a change of
# the interface is intended.
ABIDW ?= abidw
ABIDIFF ?= abidiff
ABI_MACHINE := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
ABI_BASELINE := abi/$(ABI_MACHINE).abi

# abidiff compares only the symbols of a library without debug
# information and reports no difference in a type or a parameter, so
# such a library is refused. Bits 1 and 2 of the exit status of abidiff
# mean that the tool itself failed or was called wrongly; every other
# non-zero status is a difference of the interface.
abicheck: $(SOFILE)
	@if ! command -v $(ABIDIFF) > /dev/null 2>&1; then \
		echo "SKIP abicheck: abidiff not found" >&2; \
		exit 0; \
	fi; \
	if [ ! -f $(ABI_BASELINE) ]; then \
		echo "SKIP abicheck: no ABI baseline for $(ABI_MACHINE) ($(ABI_BASELINE))" >&2; \
		exit 0; \
	fi; \
	if ! $(CROSS_COMPILE)readelf -S $(SOFILE) | grep -q '\.debug_info'; then \
		echo "abicheck: $(SOFILE) has no debug info; build it unstripped" >&2; \
		exit 1; \
	fi; \
	$(ABIDIFF) --header-file2 librsswitch.h $(ABI_BASELINE) $(SOFILE); \
	status=$$?; \
	if [ $$status -eq 0 ]; then \
		echo "abicheck: ABI unchanged against $(ABI_BASELINE)"; \
	elif [ $$((status & 3)) -ne 0 ]; then \
		echo "abicheck: abidiff failed with exit $$status (tool error, not an ABI change)" >&2; \
		exit 1; \
	else \
		echo "abicheck: the ABI differs from $(ABI_BASELINE); if the change is intended, run make abi-baseline and commit the new baseline" >&2; \
		exit 1; \
	fi

abi-baseline: $(SOFILE)
	mkdir -p abi
	$(ABIDW) --no-show-locs --no-comp-dir-path --no-corpus-path \
		--header-file librsswitch.h --out-file $(ABI_BASELINE) $(SOFILE)

# build/ belongs to the Makefile and is removed as a whole. A BUILD
# elsewhere may come from the environment and may name a directory that
# holds other files, or the sources. From it clean removes only the
# files that a build or an install writes there, for either board, and
# then the directories of the build that are left empty, BUILD last. A
# dedicated BUILD therefore disappears, and a file of the owner's stays
# together with the directories that hold it. The names are relative to
# BUILD and are quoted for the shell. There is no cd, which CDPATH could
# redirect, and no rmdir -p, which would climb into parents that the
# build did not make.
CLEAN_FILES := $(ALL_SRCS:.c=.o) $(ALL_SRCS:.c=.d) \
	librsswitch.so.$(VERSION) $(SONAME) librsswitch.so \
	librsswitch.a librsswitch-static.o librsswitch-internal.a \
	librsswitch.pc .flags
CLEAN_DIRS := $(patsubst %/,%,$(sort $(dir $(filter boards/%,$(ALL_SRCS))))) boards
QBUILD := $(subst ','\'',$(BUILD))

clean:
	rm -rf build
ifneq ($(BUILD),build/$(BOARD))
	rm -f $(foreach f,$(CLEAN_FILES),'$(QBUILD)/$(f)')
	@rmdir $(foreach d,$(CLEAN_DIRS),'$(QBUILD)/$(d)') '$(QBUILD)' 2> /dev/null || :
endif

-include $(DEPS)
