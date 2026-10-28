# Builds the library for one board into build/<board>/: the shared
# library librsswitch.so.MAJOR.MINOR.PATCH with the links
# librsswitch.so.MAJOR and librsswitch.so, and the static library
# librsswitch.a, both made from the same objects.
#
#   make                      the libgpiod board, in build/gpiod/
#   make BOARD=stub           the stub board, in build/stub/
#
# The two boards build into separate directories, so building one leaves
# the other in place and switching needs no make clean.

# Which board is built. A board is chosen by the source file that is
# compiled, not by a define.
BOARD ?= gpiod

# The prefix of the cross toolchain, for example aarch64-linux-gnu-.
# Empty means a native build; nothing here guesses the architecture.
CROSS_COMPILE ?=

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar

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

BUILD := build/$(BOARD)
OBJS := $(SRCS:%.c=$(BUILD)/%.o)
DEPS := $(OBJS:.o=.d)

SONAME := librsswitch.so.$(MAJOR)
SOFILE := $(BUILD)/librsswitch.so.$(VERSION)
SOLINKS := $(BUILD)/$(SONAME) $(BUILD)/librsswitch.so
ARFILE := $(BUILD)/librsswitch.a
LIBFILES := $(SOFILE) $(SOLINKS) $(ARFILE)

.PHONY: all test clean FORCE

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

# The tests build their own copy of the sources, whatever BOARD is.
test:
	$(MAKE) -C tests test

clean:
	rm -rf build

-include $(DEPS)
