librsswitch
===========
librsswitch (remote socket switch) is a Raspberry Pi Linux library for controlling remote controlled power sockets.
The library emulates the encoder chip within the power socket remote control, so a separate 433 MHz sender is needed.

librsswitch is written in C and heavily based on [rcswitch-pi](https://github.com/r10r/rcswitch-pi).

Encoders
===========
At the moment librsswitch is able to emulate the following encoder chips:

- PT2260-R4S

Usage
===========
You'll have to install the C library [bcm2835](http://www.open.com.au/mikem/bcm2835/) on your Raspberry Pi.
