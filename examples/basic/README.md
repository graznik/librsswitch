# rsssend

## Usage (Raspberry Pi, BeagleBone Black)

rsssend links the shared library of this repository, so build the
library first. It carries no RPATH: from the tree it finds the library
through LD_LIBRARY_PATH.

```bash
cd librsswitch
make
make -C examples/basic
LD_LIBRARY_PATH=build/gpiod examples/basic/rsssend 0 0 0 1
```

The same build serves the Raspberry Pi, the Raspberry Pi 3 and the
BeagleBone Black. A program that was built against an installed library
(see "Installing the library" in the main README) needs no
LD_LIBRARY_PATH.

## Usage (cross compiling on x86_64)

```bash
cd librsswitch
make CROSS_COMPILE=aarch64-linux-gnu-
make -C examples/basic CROSS_COMPILE=aarch64-linux-gnu-
file examples/basic/rsssend
```

```
  rsssend: ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV), dynamically linked
```

arm-linux-gnueabihf- builds for 32-bit ARM (ARMv7) in the same way. The
program needs librsswitch.so.1 on the board, so install the library
there first (see the main README) and then copy the program:

```bash
scp examples/basic/rsssend user@rpi:~/
ssh user@rpi
./rsssend 0 0 0 1
```

## Usage (x86_64 host)

From the repository root:

```bash
make BOARD=stub
make -C examples/basic BOARD=stub
LD_LIBRARY_PATH=build/stub examples/basic/rsssend 1 17 23 1
```

```
0FFF0000F00F
```

Arguments: DEV (0 = REV 008345, 1 = Pollin 2605), GROUP, SOCKET, DATA
(0 = off, 1 = on), and optionally --gpio CONFIG. Exit status: 0 sent,
1 send failed, the address was rejected by the library or --gpio is
malformed (nothing sent), 2 usage error (nothing sent). A malformed
--gpio is refused before anything is sent, with
`rsssend: malformed --gpio '<value>'` on stderr.
