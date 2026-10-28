# Builds rsssend against the library of this repository.
#
#   make                      against the libgpiod board
#   make BOARD=stub           against the stub board
#   make CROSS_COMPILE=aarch64-linux-gnu-
#
# The library is built first, with the Makefile in the top directory,
# and the program links its shared library, so BOARD, CROSS_COMPILE,
# DEBUG and LIBGPIOD_VERSION mean here what they mean there. The
# program carries no RPATH: from the tree it runs with the build
# directory of its board on LD_LIBRARY_PATH.

# The root of the repository, whatever directory make runs in.
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)

BOARD ?= gpiod

# The prefix of the cross toolchain. Empty means a native build;
# nothing here guesses the architecture.
CROSS_COMPILE ?=

CC := $(CROSS_COMPILE)gcc

# Where the library of BOARD is built; the top Makefile uses the same
# variable. A relative path is relative to this directory.
BUILD ?= $(TOP)/build/$(BOARD)
LIBBUILD := $(abspath $(BUILD))

# The flags the program cannot be built without live in their own
# variable, so that a CFLAGS given on the command line adds to them.
RSS_CFLAGS := -Werror -Wall -W -pedantic
RSS_CFLAGS += -pthread
RSS_CFLAGS += -D_FORTIFY_SOURCE=2 -O2
RSS_CFLAGS += -funsigned-char -Wpointer-arith
RSS_CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes
RSS_CFLAGS += -Wwrite-strings -Wcast-qual -Wcast-align
RSS_CFLAGS += -Wbad-function-cast -Wformat-security -Wformat-nonliteral
RSS_CFLAGS += -Wmissing-format-attribute -Winline
RSS_CFLAGS += -Wformat -Werror=format-security -Werror=array-bounds

ifeq ($(DEBUG),1)
RSS_CFLAGS += -DDEBUG
endif

.PHONY: all clean FORCE

all: rsssend

# The library is made by the top Makefile on every run, which only
# rebuilds what changed. Variables given on the command line, such as
# CROSS_COMPILE, DEBUG and LIBGPIOD_VERSION, reach it by themselves.
$(LIBBUILD)/librsswitch.so: FORCE
	$(MAKE) -C $(TOP) BOARD=$(BOARD) BUILD=$(LIBBUILD)

FORCE:

rsssend: rsssend.c $(TOP)/librsswitch.h $(LIBBUILD)/librsswitch.so
	$(CC) $(RSS_CFLAGS) $(CFLAGS) $(CPPFLAGS) -I$(TOP) $(LDFLAGS) \
		-o $@ rsssend.c -L$(LIBBUILD) -lrsswitch

clean:
	-@rm -rf *.o *~ rsssend 2>/dev/null || true
