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
- BeagleBone Black, through libgpiod
- x86_64 host, a stub build for tests that needs no hardware


## Supported devices

At the moment librsswitch supports the following devices:

- REV 008345 (PT2260-R4S)
- Pollin 2605 (PT2262)


## Hardware setup

Detailed hardware and build-environment setup is available at
[graznik.de](https://graznik.de).

The data input of the 433 MHz sender is driven by the default line,
GPIO17 (header pin 11) on a Raspberry Pi and P9_23 on a BeagleBone
Black. Another line is chosen with a "chip:line" config, see "GPIO
configuration" below.


## Software setup (Raspberry Pi / Raspberry Pi 3 / BeagleBone Black)

GPIO access goes through
[libgpiod](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/),
not bcm2835 -- install the dev package on the board:

```bash
sudo apt install libgpiod-dev
```

Raspberry Pi OS and Debian 13 on the BeagleBone Black ship libgpiod
2.x, older images 1.x, and both work.

rpi, rpi3 and bbb build the exact same board source,
boards/gpiod/gpiod_board.c. rpi and rpi3 differ only in the cross
toolchain (32-bit arm-linux-gnueabihf- vs. aarch64-linux-gnu- when
cross compiling; both use the native gcc on a Pi), and bbb differs
from rpi only in its default line, which boards/bbb/bbb.h sets.
libgpiod 1.x and 2.x are both supported: the board picks its API from
the installed gpiod.h at compile time (gpiod.h from 1.x defines
GPIOD_LINE_BULK_MAX_LINES, the 2.x header does not), so a plain build
just works with whichever libgpiod-dev is installed. If the probe ever
picks the wrong one, force it with LIBGPIOD_VERSION, for bbb as for
rpi:

```bash
make LIBGPIOD_VERSION=1 rpi   # force the libgpiod 1.x (gpiod_line_*) API
make LIBGPIOD_VERSION=2 rpi   # force the libgpiod 2.x (gpiod_line_request_*) API
```

The "chip:line" config format (see "GPIO configuration" below) is the
same for both versions.

Build with the matching Makefile target from examples/basic/ or
examples/network/; on the board itself, every target uses the
native gcc (only the cross-compiling host below needs a cross
toolchain):

```bash
make rpi     # Raspberry Pi (32-bit ARM)
make rpi3    # Raspberry Pi 3 (aarch64)
make bbb     # BeagleBone Black (32-bit ARM)
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
make bbb     # arm-linux-gnueabihf-gcc
```

setenv.sh and setenv64.sh are only needed when your cross toolchain
lives outside PATH (for example a Linaro tarball under /opt) -- they
add its bin/ directory to PATH. A toolchain installed from your
distro's packages needs neither script, since the Makefile already
picks the right compiler for the target.

libgpiod itself (headers + library) needs to be available for the
target architecture of rpi, rpi3 and bbb in your cross sysroot --
either cross-build it from source or install your distro's cross-dev
variant if one exists.
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

- Raspberry Pi / Raspberry Pi 3 / BeagleBone Black: "chip:line", e.g.
  "gpiochip0:17". NULL, or a malformed string, is logged and the board
  falls back to its default, gpiochip0:17. A bare GPIO number such as
  "49", the BeagleBone Black's former format, is malformed too. The
  default is GPIO17 (header pin 11) on the Raspberry Pi, and P9_23 on a
  BeagleBone Black with Debian 13 (kernel 6.x). On other kernels the
  chip of P9_23 can differ: find it with `gpioinfo` from the gpiod
  package and pass it as "chip:line".
- x86_64: ignored -- the stub board never touches real GPIO.


## Running the tests (x86_64 host)

From the repository root:

```bash
make -C tests venv test
```

The venv target makes the web app's virtual environment in
examples/web/venv from examples/web/requirements-dev.txt. The first run
downloads the packages, and later runs only check that the venv is
there. `make -C tests test` then runs every C test, then the tests of
the web app, including the end-to-end test against the x86_64
rsswitchd that it builds. It exits non-zero when any of them fails.
It fails, and never skips, when the venv, pytest or the daemon is
missing; it never makes the venv itself. A venv at another place is
named with WEB_PYTHON, as the absolute path of the python in its bin
directory:

```bash
make -C tests test WEB_PYTHON=$PWD/examples/web/venv/bin/python
```

WEB_VENV=/path makes and uses a venv at that place instead, with
`make -C tests venv test WEB_VENV=/path`.

### Against libgpiod 2.x

The default run builds the libgpiod tests against the installed
libgpiod, so on a host with libgpiod 1.x the 2.x code of
boards/gpiod/gpiod_board.c is not compiled. To test it, build libgpiod
2.3.1 into a directory of your own (git, meson, ninja and a C compiler;
no root needed; libgpiod 2.3 and later build with meson) and name it on
the command line, from the repository root:

```bash
G2=$HOME/libgpiod-2.3.1
git clone -c advice.detachedHead=false --depth 1 --branch v2.3.1 \
    https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git "$G2-src"
meson setup "$G2-src/build" "$G2-src" --prefix="$G2" --libdir=lib \
    --auto-features=disabled
ninja -C "$G2-src/build" install
make -C tests clean test \
    GPIOD_CFLAGS="-I$G2/include" \
    GPIOD_LIBS="-L$G2/lib -lgpiod -Wl,-rpath,$G2/lib" \
    GPIOD_EXPECT_VERSION=2
```

The first four commands are needed once. clean makes sure the libgpiod
tests are rebuilt against the named library, because make does not
notice changed flags. GPIOD_EXPECT_VERSION=2 stops the build with an
error if the tests would be compiled against another libgpiod major
version, or if libgpiod is not found there at all, instead of quietly
testing the installed one or skipping the tests. The run needs the venv
of the web app like the default run.

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
kernel keeps a released output at its last level, and a BeagleBone
Black may do the same, so releasing the line does not bound this
window either.

The 433 MHz link is one-way: the library has no way to confirm that a
socket actually switched, only that a codeword was sent.


## References

- [rcswitch-pi](https://github.com/r10r/rcswitch-pi)
- [rc-switch](http://code.google.com/p/rc-switch/)
- [graznik.de](https://graznik.de): hardware and build-environment setup
- [libgpiod](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/)
