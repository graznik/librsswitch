#CFLAGS=-Wall
CFLAGS=-Wall -DDEBUG

sswitch:
	gcc $(CFLAGS) -o rsssend rsssend.c librsswitch.c -l rt -l bcm2835

clean:
	rm *.o *~ rsssend
