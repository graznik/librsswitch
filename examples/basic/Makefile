CFLAGS += -Werror -Wall -W -pedantic
CFLAGS += -D_FORTIFY_SOURCE=2 -O2
CFLAGS += -funsigned-char -Wpointer-arith
CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes
CFLAGS += -Wpointer-arith -Wwrite-strings -Wcast-qual -Wcast-align
CFLAGS += -Wbad-function-cast -Wformat-security -Wformat-nonliteral
CFLAGS += -Wmissing-format-attribute -Winline
CFLAGS += -Wformat -Werror=format-security -Werror=array-bounds

.PHONY: rpi, rpi3, bbb, x86_64, clean

ifeq ($(MAKECMDGOALS), )
$(error No target given. Supported targets: rpi, rpi3, bbb, x86_64)
endif

ARCH := $(shell uname -m)

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
CFLAGS += -DRASPBERRY_PI3
LFLAGS = -l rt -l gpiod
else ifeq ($(MAKECMDGOALS), bbb)
CFLAGS += -DBEAGLEBONE_BLACK
endif

CC=$(CROSS_COMPILE)gcc
INCLUDES = -I/usr/include/
INCLUDES += -I/usr/local/include/
LIBS = -L/usr/local/lib

rpi:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c ../../librsswitch.c ../../boards/rpi/rpi.c $(LFLAGS)

rpi3:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c ../../librsswitch.c ../../boards/rpi/rpi3.c $(LFLAGS)

bbb:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c ../../librsswitch.c ../../boards/bbb/bbb.c $(LFLAGS)

x86_64:
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBS) -o rsssend rsssend.c ../../librsswitch.c ../../boards/x86_64/x86_64.c $(LFLAGS)

clean:
	-@rm -rf *.o *~ rsssend 2>/dev/null || true
