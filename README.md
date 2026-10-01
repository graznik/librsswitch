# librsswitch

librsswitch (remote socket switch) is a Linux library for controlling
remote controlled power sockets. It switches REV 008345 and Pollin
2605 433 MHz sockets by emulating the PT2260/PT2262 encoder chip of
their remote control on a GPIO line, so a separate 433 MHz sender is
needed. It runs on the Raspberry Pi (32-bit), the Raspberry Pi 3
(aarch64) and the BeagleBone Black, and has an x86_64 stub build that
prints the codewords instead of driving a line, for tests.

librsswitch is written in C and heavily inspired by
[rcswitch-pi](https://github.com/r10r/rcswitch-pi), which is based on
[rc-switch](http://code.google.com/p/rc-switch/).


## Supported boards

- Raspberry Pi (32-bit), through libgpiod
- Raspberry Pi 3 (aarch64), through libgpiod
- BeagleBone Black, through sysfs GPIO
- x86_64 host, a stub build for tests that needs no hardware


## Supported devices

At the moment librsswitch supports the following devices:

- REV 008345 (PT2260-R4S)
- Pollin 2605 (PT2262)


## Hardware setup

See the [librsswitch Wiki](https://github.com/graznik/librsswitch/wiki)
for a detailed description of how to set up the entire hardware.


## Software setup (Raspberry Pi / Raspberry Pi 3)

GPIO access goes through
[libgpiod](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/),
not bcm2835 -- install the dev package on the Pi:

```bash
sudo apt install libgpiod-dev
```

rpi and rpi3 build the exact same board source (boards/rpi/rpi.c) and
differ only in the cross toolchain (32-bit arm-linux-gnueabihf- vs.
aarch64-linux-gnu- when cross compiling; both use the native gcc on a
Pi). libgpiod 1.x and 2.x are both supported: the board picks its API
from the installed gpiod.h at compile time (gpiod.h from 1.x defines
GPIOD_LINE_BULK_MAX_LINES, the 2.x header does not), so a plain build
just works with whichever libgpiod-dev is installed. If the probe ever
picks the wrong one, force it with LIBGPIOD_VERSION:

```bash
make LIBGPIOD_VERSION=1 rpi   # force the libgpiod 1.x (gpiod_line_*) API
make LIBGPIOD_VERSION=2 rpi   # force the libgpiod 2.x (gpiod_line_request_*) API
```

The "chip:line" config format (see "GPIO configuration" below) is the
same for both versions.

Build with the matching Makefile target from examples/basic/ or
examples/network/; on the Pi itself, both rpi and rpi3 use the
native gcc (only the cross-compiling host below needs a cross
toolchain):

```bash
make rpi     # Raspberry Pi (32-bit ARM)
make rpi3    # Raspberry Pi 3 (aarch64)
make bbb     # BeagleBone Black (sysfs GPIO, no libgpiod needed)
make x86_64  # host build, GPIO calls are stubbed out
```

Any of these targets also accepts DEBUG=1 to turn on verbose syslog
output (off by default, so a release build never logs every command):

```bash
make DEBUG=1 rpi
```


## Software setup (cross compiling on x86_64)

Install an ARM/aarch64 cross toolchain (e.g. via your distro's
gcc-arm-linux-gnueabihf / gcc-aarch64-linux-gnu packages), then build
with the matching target from examples/basic/ or examples/network/ --
the Makefile picks the cross compiler for you based on the target and
picks up libgpiod headers from /usr/include and /usr/local/include:

```bash
cd examples/basic   # or examples/network
make rpi     # arm-linux-gnueabihf-gcc
make rpi3    # aarch64-linux-gnu-gcc
```

setenv.sh and setenv64.sh are only needed when your cross toolchain
lives outside PATH (for example a Linaro tarball under /opt) -- they
add its bin/ directory to PATH. A toolchain installed from your
distro's packages needs neither script, since the Makefile already
picks the right compiler for the target.

libgpiod itself (headers + library) needs to be available for the
target architecture in your cross sysroot -- either cross-build it
from source or install your distro's cross-dev variant if one exists.
A libgpiod installed under its own prefix is picked up through the
usual CPPFLAGS and LDFLAGS, which take precedence over the default
/usr/include, /usr/local/include and /usr/local/lib paths:

```bash
make CPPFLAGS=-I<prefix>/include LDFLAGS=-L<prefix>/lib rpi
```


## Host build (x86_64 host)

From the repository root:

```bash
make -C examples/basic x86_64
./examples/basic/rsssend 1 17 23 1
```

```
0FFF0000F00F
```

```bash
make -C examples/network x86_64
```


## Usage

Four public functions, declared in librsswitch.h.

```c
socket_gpio_config(const char *config);
```

Sets the GPIO configuration string used by every following
socket_send()/socket_send_codeword() call. NULL (the default, if never
called) means "use this board's compiled-in default". Format is
per-board -- see "GPIO configuration" below.

```c
socket_send(uint32_t dev, uint32_t group, uint32_t socket, uint32_t data);
```

- dev: 0 = REV 008345, 1 = Pollin 2605
- group: REV 008345 0-3, Pollin 2605 0-31
- socket: REV 008345 0-2, Pollin 2605 0-31
- data: 1 = On, 0 = Off

Returns 0 on success, -1 for an out-of-range group/socket/data, an
unknown dev, or a board error. Arguments are fully validated before
board_init() or any GPIO access, so an invalid address never touches
the board and nothing is sent.

```c
socket_send_codeword(const char *tristate, uint32_t pulse_len);
```

Sends a raw tri-state codeword instead of one of the two built-in
device tables above.

- tristate: 1 to SOCKET_CODEWORD_MAX_LEN (12) bytes of '0', '1' and
  uppercase 'F'
- pulse_len: microseconds, between SOCKET_PULSE_LEN_MIN_US (50) and
  SOCKET_PULSE_LEN_MAX_US (2000)

Input is fully validated before board_init() or any GPIO access, so an
invalid tristate or pulse_len never touches the board. On success,
tristate is sent 10 times, each repetition followed by a sync pulse --
the same repeat count socket_send() uses. On the x86_64 build, the
codeword is printed to stdout, exactly like socket_send() does.

Returns 0 on success, -1 on error (invalid input, or a board error).

```c
socket_addr_limits(uint32_t dev, uint32_t *ngroups, uint32_t *nsockets,
uint32_t *ndata);
```

Reports the valid address range for dev, the same bounds socket_send()
enforces: REV 008345 has 4 groups, 3 sockets and 2 data values; Pollin
2605 has 32 groups, 32 sockets and 2 data values. Lets a caller (such
as rsswitchd) validate an address itself before calling socket_send(),
reading the same tables socket_send() does instead of copying the
sizes elsewhere.

Returns 0 and fills `*ngroups`, `*nsockets` and `*ndata`, or -1 for an
unknown dev or a NULL pointer, leaving the outputs untouched.


## Thread safety

socket_send(), socket_send_codeword(), socket_addr_limits() and
socket_gpio_config() are safe to call concurrently from multiple
threads of the same process. Every send session -- board_init(), all
repetitions, board_exit() -- is serialized by one library-wide lock,
so concurrent sends never interleave their pulses; a calling thread
simply blocks until the previous send session completes.
socket_gpio_config() takes the same lock around its store, so a config
change is ordered against sends that start after it returns (it does
not affect a session already in progress). socket_addr_limits() takes
no lock at all: it only reads constant tables and never touches
gpio_config or the board, so it never blocks and never waits behind a
send in progress.

This guarantee is process-local only: two separate processes driving
the same GPIO line (for example rsssend run once while rsswitchd is
active) are not serialized against each other.

The library is not async-signal-safe -- never call any of these
functions from a signal handler. Cancelling a thread while it is
inside a send is not supported; doing so may leave the lock held.


## GPIO configuration

The config string passed to socket_gpio_config() (or "--gpio" on the
example programs) is board-specific:

- Raspberry Pi / Raspberry Pi 3: "chip:line", e.g. "gpiochip0:17". NULL
  or a malformed string is logged and the board falls back to the
  default, gpiochip0:17.
- BeagleBone Black: a numeric GPIO number, e.g. "49". NULL falls back
  to the default, 49.
- x86_64: ignored -- the stub board never touches real GPIO.


## Running the tests (x86_64 host)

From the repository root:

```bash
make -C tests test
```

### ThreadSanitizer run

Optional ThreadSanitizer run of the concurrency test (needs libtsan;
not part of the default test target above, and run under setarch
automatically when available):

```bash
make -C tests tsan
```


## Known limitations

Pulses are timed with sleeps on a non-real-time kernel, so system load
or signal delivery can stretch or shorten a pulse and a receiver may
ignore the resulting code. The x86_64 host tests cannot detect this,
since the stub board has no real timing constraint to violate.
Real-hardware verification of pulse timing is outside this milestone.

board_exit() drives the GPIO line low whenever a send fails, and
rsswitchd finishes a send already in progress before it exits on
SIGTERM or SIGINT. No library code runs when a sender is killed
with SIGKILL or crashes, though, and such a hard kill while the
line is high leaves the 433 MHz transmitter keyed, blocking the
band for other devices, until the next send starts, since
board_init() always drives the line low first. A Raspberry Pi
kernel keeps a released output at its last level, and a
BeagleBone Black pin exported beforehand stays an output, so
neither release nor close bounds this window.

The 433 MHz link is one-way: the library has no way to confirm that a
socket actually switched, only that a codeword was sent.


## References

- [rcswitch-pi](https://github.com/r10r/rcswitch-pi)
- [rc-switch](http://code.google.com/p/rc-switch/)
- [librsswitch Wiki](https://github.com/graznik/librsswitch/wiki)
- [libgpiod](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/)
