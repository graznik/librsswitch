CFLAGS =  -DDEBUG
CFLAGS += -Wall -Werror
CFLAGS += -Wmissing-prototypes -Wmissing-declarations
CFLAGS += -Wstrict-prototypes -Wpointer-arith -Wwrite-strings
CFLAGS += -Wcast-qual -Wcast-align -Wbad-function-cast
CFLAGS += -Wformat-security  -Wformat-nonliteral -Wmissing-format-attribute
CFLAGS += -Winline -W -pedantic -funsigned-char

ARCH := $(shell uname -m)
PREFIX=/usr/local

ifeq ($(ARCH),x86_64)
ARCH=arm
CROSS_COMPILE=arm-linux-gnueabihf-
CC=$(CROSS_COMPILE)gcc
INCLUDES=-I/usr/local/include/
LIBS=-L/usr/local/lib
endif

rsswitchd:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsswitchd rsswitchd.c ../../librsswitch.c -l rt -l bcm2835

install: rsswitchd
	install -m 0755 rsswitchd $(PREFIX)/bin
	cp rsswitchd.service /etc/systemd/system/
	systemctl enable rsswitchd
	systemctl start rsswitchd

clean:
	-@rm -rf *.o *~ rsswitchd 2>/dev/null || true
