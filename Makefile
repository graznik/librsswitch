CFLAGS=-Wall -DDEBUG
ARCH := $(shell uname -m)

ifeq ($(ARCH),x86_64)
ARCH=arm
CROSS_COMPILE=arm-linux-gnueabihf-
CC=$(CROSS_COMPILE)gcc
INCLUDES=-I/usr/local/include/
LIBS=-L/usr/local/lib
endif

sswitch:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c librsswitch.c -l rt -l bcm2835

clean:
	-@rm -rf *.o *~ rsssend 2>/dev/null || true
