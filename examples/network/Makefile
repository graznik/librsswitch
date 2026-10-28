# Builds rsswitchd against the library of this repository.
#
#   make                      against the libgpiod board
#   make BOARD=stub           against the stub board
#   make CROSS_COMPILE=aarch64-linux-gnu-
#   make install              the daemon and its unit, after the library
#
# The library is built first, with the Makefile in the top directory,
# and the daemon links its shared library, so BOARD, CROSS_COMPILE,
# DEBUG and LIBGPIOD_VERSION mean here what they mean there. The
# daemon carries no RPATH: from the tree it runs with the build
# directory of its board on LD_LIBRARY_PATH, and an installed daemon
# finds the installed library through the loader cache.

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

# The flags the daemon cannot be built without live in their own
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

PREFIX=/usr/local
SYSTEMD_UNIT_DIR=/etc/systemd/system

.PHONY: all install clean FORCE

all: rsswitchd

# The library is made by the top Makefile on every run, which only
# rebuilds what changed. Variables given on the command line, such as
# CROSS_COMPILE, DEBUG and LIBGPIOD_VERSION, reach it by themselves.
$(LIBBUILD)/librsswitch.so: FORCE
	$(MAKE) -C $(TOP) BOARD=$(BOARD) BUILD=$(LIBBUILD)

FORCE:

# -lrt stays for every board: the daemon calls clock_gettime(), which
# glibc before 2.17 keeps in librt.
rsswitchd: rsswitchd.c rsswitchd_proto.c rsswitchd_proto.h \
		$(TOP)/librsswitch.h $(LIBBUILD)/librsswitch.so
	$(CC) $(RSS_CFLAGS) $(CFLAGS) $(CPPFLAGS) -I$(TOP) $(LDFLAGS) \
		-o $@ rsswitchd.c rsswitchd_proto.c \
		-L$(LIBBUILD) -lrsswitch -lrt

# The daemon runs as the unprivileged system user rsswitchd, in the
# gpio group for GPIO access. Both the group and the user are
# created only when missing, so a second install keeps them.
# Staged installs (DESTDIR set) skip this together with the
# systemctl steps below, same as before. A static account is used
# instead of systemd's DynamicUser= because the account must persist
# across restarts and stay a member of the gpio group.
# Without DESTDIR the service is restarted at the end, not only
# started: an upgrade replaces the binary, and a daemon that is
# already running keeps running the old one until it is restarted.
# A restart also starts a stopped service. daemon-reload comes first,
# as in examples/web/Makefile, so that the restart uses the unit file
# that make install has just written. Within the start-limit interval
# after failed starts, systemd refuses a restart, so reset-failed
# clears that first. It may fail, because a unit that never failed
# makes it complain; every other command of the block stops the
# install when it fails.
install:
	@if [ ! -x rsswitchd ]; then \
		echo "rsswitchd has not been built, run make in examples/network first" >&2; \
		exit 1; \
	fi
	install -D -m 0755 rsswitchd $(DESTDIR)$(PREFIX)/bin/rsswitchd
	install -D -m 0644 rsswitchd.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)/rsswitchd.service
	@if [ -z "$(DESTDIR)" ]; then \
		set -e; \
		getent group gpio > /dev/null || groupadd --system gpio; \
		getent passwd rsswitchd > /dev/null || useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin --groups gpio rsswitchd; \
		systemctl daemon-reload; \
		systemctl enable rsswitchd; \
		systemctl reset-failed rsswitchd 2>/dev/null || true; \
		systemctl restart rsswitchd; \
	fi

clean:
	-@rm -rf *.o *~ rsswitchd 2>/dev/null || true
