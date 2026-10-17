# rsssend

## Usage (Raspberry Pi, BeagleBone Black)

```bash
cd librsswitch/examples/basic
make rpi     # or: make rpi3 on a 64-bit Raspberry Pi OS
             # or: make bbb on a BeagleBone Black
./rsssend 0 0 0 1
```

## Usage (cross compiling on x86_64)

```bash
cd librsswitch/examples/basic
make rpi
file rsssend
```

```
  rsssend: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV)
```

```bash
scp rsssend user@rpi:~/
ssh user@rpi
./rsssend 0 0 0 1
```

## Usage (x86_64 host)

From examples/basic:

```bash
make x86_64
./rsssend 1 17 23 1
```

```
0FFF0000F00F
```

Arguments: DEV (0 = REV 008345, 1 = Pollin 2605), GROUP, SOCKET, DATA
(0 = off, 1 = on), and optionally --gpio CONFIG. Exit status: 0 sent,
1 send failed or the address was rejected by the library (nothing
sent), 2 usage error (nothing sent).
