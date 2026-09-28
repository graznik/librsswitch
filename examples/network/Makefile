CFLAGS += -Werror -Wall -W -pedantic
CFLAGS += -pthread
CFLAGS += -D_FORTIFY_SOURCE=2 -O2
CFLAGS += -funsigned-char -Wpointer-arith
CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes
CFLAGS += -Wpointer-arith -Wwrite-strings -Wcast-qual -Wcast-align
CFLAGS += -Wbad-function-cast -Wformat-security -Wformat-nonliteral
CFLAGS += -Wmissing-format-attribute -Winline
CFLAGS += -Wformat -Werror=format-security -Werror=array-bounds

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG
endif

.PHONY: rpi rpi3 bbb x86_64 install clean

ifeq ($(MAKECMDGOALS), )
$(error No target given. Supported targets: rpi, rpi3, bbb, x86_64)
endif

ARCH := $(shell uname -m)
PREFIX=/usr/local
SYSTEMD_UNIT_DIR=/etc/systemd/system

ifeq ($(ARCH),x86_64)
ifeq ($(MAKECMDGOALS), x86_64)
CFLAGS += -DX86_64
else ifeq ($(MAKECMDGOALS), rpi3)
ARCH:=aarch64
CROSS_COMPILE=aarch64-linux-gnu-
else
ARCH:=arm
CROSS_COMPILE=arm-linux-gnueabihf-
endif
endif

ifeq ($(MAKECMDGOALS), rpi)
CFLAGS += -DRASPBERRY_PI
LFLAGS = -l rt -l gpiod
else ifeq ($(MAKECMDGOALS), rpi3)
CFLAGS += -DRASPBERRY_PI
LFLAGS = -l rt -l gpiod
else ifeq ($(MAKECMDGOALS), bbb)
CFLAGS += -DBEAGLEBONE_BLACK
endif

# Forces boards/rpi/rpi.c's libgpiod API choice when its header probe
# picks the wrong one (e.g. cross-compiling against a sysroot whose
# gpiod.h can't be told apart from the probe alone). 1 = libgpiod 1.x
# (gpiod_line_*), 2 = libgpiod 2.x (gpiod_line_request_*). Unset (the
# default) leaves the probe in charge.
ifneq ($(LIBGPIOD_VERSION),)
ifeq ($(filter $(LIBGPIOD_VERSION),1 2),)
$(error LIBGPIOD_VERSION must be 1 or 2)
endif
CFLAGS += -DLIBRSSWITCH_GPIOD_VERSION=$(LIBGPIOD_VERSION)
endif

CC=$(CROSS_COMPILE)gcc
INCLUDES = -I/usr/include/
INCLUDES += -I/usr/local/include/
LIBS = -L/usr/local/lib


rpi:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $(LDFLAGS) $(LIBS) -o rsswitchd rsswitchd.c rsswitchd_proto.c ../../librsswitch.c ../../boards/rpi/rpi.c $(LFLAGS)

rpi3:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $(LDFLAGS) $(LIBS) -o rsswitchd rsswitchd.c rsswitchd_proto.c ../../librsswitch.c ../../boards/rpi/rpi.c $(LFLAGS)

bbb:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $(LDFLAGS) $(LIBS) -o rsswitchd rsswitchd.c rsswitchd_proto.c ../../librsswitch.c ../../boards/bbb/bbb.c $(LFLAGS)

x86_64:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $(LDFLAGS) $(LIBS) -o rsswitchd rsswitchd.c rsswitchd_proto.c ../../librsswitch.c ../../boards/x86_64/x86_64.c $(LFLAGS)


install:
	@if [ ! -x rsswitchd ]; then \
		echo "rsswitchd has not been built, run make <rpi|rpi3|bbb|x86_64> first" >&2; \
		exit 1; \
	fi
	install -D -m 0755 rsswitchd $(DESTDIR)$(PREFIX)/bin/rsswitchd
	install -D -m 0644 rsswitchd.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)/rsswitchd.service
	@if [ -z "$(DESTDIR)" ]; then \
		systemctl enable rsswitchd; \
		systemctl start rsswitchd; \
	fi

clean:
	-@rm -rf *.o *~ rsswitchd 2>/dev/null || true
