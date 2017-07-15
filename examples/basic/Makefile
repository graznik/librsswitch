CFLAGS =  -DDEBUG
CFLAGS += -Wall -Werror
CFLAGS += -Wmissing-prototypes -Wmissing-declarations
CFLAGS += -Wstrict-prototypes -Wpointer-arith -Wwrite-strings
CFLAGS += -Wcast-qual -Wcast-align -Wbad-function-cast
CFLAGS += -Wformat-security  -Wformat-nonliteral -Wmissing-format-attribute
CFLAGS += -Winline -W -pedantic -funsigned-char

ARCH := $(shell uname -m)

ifeq ($(ARCH),x86_64)
ARCH=arm
CROSS_COMPILE=arm-linux-gnueabihf-
CC=$(CROSS_COMPILE)gcc
INCLUDES=-I/usr/local/include/
LIBS=-L/usr/local/lib
endif

sswitch:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c ../../librsswitch.c -l rt -l bcm2835

clean:
	-@rm -rf *.o *~ rsssend 2>/dev/null || true
