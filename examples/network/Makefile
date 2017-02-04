CFLAGS=-Wall -DDEBUG
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
