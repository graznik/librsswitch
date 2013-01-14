CFLAGS=-Wall
#CFLAGS=-Wall -DDEBUG

sswitch:
	gcc $(CFLAGS) -o send send.c librsswitch.c -l rt -l bcm2835

clean:
	rm *.o *~ send
