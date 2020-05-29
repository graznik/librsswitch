CFLAGS =  -DDEBUG
CFLAGS +=  -Werror -Wall -W -pedantic
CFLAGS += -D_FORTIFY_SOURCE=2 -O2
CFLAGS += -funsigned-char -Wpointer-arith
CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes
CFLAGS += -Wpointer-arith -Wwrite-strings -Wcast-qual -Wcast-align
CFLAGS += -Wbad-function-cast -Wformat-security -Wformat-nonliteral
CFLAGS += -Wmissing-format-attribute -Winline
CFLAGS += -Wformat -Werror=format-security -Werror=array-bounds

.PHONY: rpi, bbb, x86_64, clean

ifeq ($(MAKECMDGOALS), )
$(error No target given. Supported targets: rpi, bbb)
endif

ARCH := $(shell uname -m)
PREFIX=/usr/local

ifeq ($(ARCH),x86_64)
ifneq ($(MAKECMDGOALS), x86_64)
ARCH:=arm
CROSS_COMPILE=arm-linux-gnueabihf-
else
CFLAGS += -DX86_64
endif
endif

ifeq ($(MAKECMDGOALS), rpi)
CFLAGS += -DRASPBERRY_PI
LFLAGS = -l rt -l bcm2835
else ifeq ($(MAKECMDGOALS), bbb)
CFLAGS += -DBEAGLEBONE_BLACK
endif

CC=$(CROSS_COMPILE)gcc
INCLUDES = -I/usr/local/include/
LIBS = -L/usr/local/lib


rpi:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsswitchd rsswitchd.c ../../librsswitch.c ../../boards/rpi/rpi.c $(LFLAGS)

bbb:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsswitchd rsswitchd.c ../../librsswitch.c ../../boards/bbb/bbb.c $(LFLAGS)

x86_64:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsswitchd rsswitchd.c ../../librsswitch.c $(LFLAGS)


install: rsswitchd
	install -m 0755 rsswitchd $(PREFIX)/bin
	cp rsswitchd.service /etc/systemd/system/
	systemctl enable rsswitchd
	systemctl start rsswitchd

clean:
	-@rm -rf *.o *~ rsswitchd 2>/dev/null || true
