# librsswitch

librsswitch (remote socket switch) is a Linux library for controlling
remote controlled power sockets. It switches REV 008345 and Pollin
2605 433 MHz sockets by emulating the PT2260/PT2262 encoder chip of
their remote control on a GPIO line, so a separate 433 MHz sender is
needed. It runs on the Raspberry Pi (32-bit), the Raspberry Pi 3
(aarch64) and the BeagleBone Black, and has a stub board that prints
the codewords instead of driving a line, for tests.

librsswitch is written in C and heavily inspired by
[rcswitch-pi](https://github.com/r10r/rcswitch-pi), which is based on
[rc-switch](http://code.google.com/p/rc-switch/).


## Supported boards

- Raspberry Pi (32-bit), through libgpiod
- Raspberry Pi 3 (aarch64), through libgpiod
- BeagleBone Black, through libgpiod
- stub board, for tests on any host that needs no hardware


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


## Building the library

The Makefile in the root of the repository builds the library as a
shared library with the SONAME librsswitch.so.1 and as a static
library, for one board at a time:

```bash
make                # the libgpiod board, in build/gpiod/
make BOARD=stub     # the stub board, in build/stub/
```

Each build leaves librsswitch.so.1.0.0, the links librsswitch.so.1 and
librsswitch.so to it, and librsswitch.a in the directory of its board.
The two boards build side by side, so switching between them needs no
make clean. Any other BOARD stops make with an error. The version in
the file names and in the SONAME comes from the LIBRSSWITCH_VERSION_*
lines of librsswitch.h.

The shared library exports the seven functions of librsswitch.h and
nothing else; the names of the board code, such as transmit() and
board_init(), are not visible to a program that links it. The static
library has no export list, so it keeps those names.

LIBGPIOD_VERSION=1 or 2 forces the libgpiod API (see "Software setup"
below), and DEBUG=1 turns on the verbose syslog output:

```bash
make LIBGPIOD_VERSION=2 DEBUG=1
```

A change of one of these, of CROSS_COMPILE or of a compiler flag
rebuilds the objects of that board by itself.

The build is native unless CROSS_COMPILE names a toolchain prefix.
Nothing guesses the architecture:

```bash
make CROSS_COMPILE=arm-linux-gnueabihf-   # 32-bit ARM (ARMv7)
make CROSS_COMPILE=aarch64-linux-gnu-     # aarch64
```

arm-linux-gnueabihf- produces ARMv7 code. A Raspberry Pi Zero (ARMv6)
builds the library natively on the board instead.

A libgpiod that is not installed system-wide is found through CPPFLAGS
and LDFLAGS. CFLAGS can be given too; the flags that the shared
library needs are kept:

```bash
make CPPFLAGS=-I<prefix>/include LDFLAGS=-L<prefix>/lib
```

BUILD names the directory of the board instead of build/<board>/, for
a build that must not write below the sources, or on a file system that
cannot make symbolic links. The Makefiles of the examples take the
library from the same variable, so give them the same value:

```bash
make BOARD=stub BUILD=/tmp/rsswitch-build/stub
make -C examples/network BOARD=stub BUILD=/tmp/rsswitch-build/stub
LD_LIBRARY_PATH=/tmp/rsswitch-build/stub examples/network/rsswitchd --bind 127.0.0.1
```

The goal test runs the tests of tests/, and clean removes build/ and
the BUILD directory.


## Installing the library

make install puts the library of one board, its header and a
pkg-config file under PREFIX, which is /usr/local by default:

```bash
make
sudo make install
```

Build as yourself first and install as root afterwards. make install
builds whatever is missing, so a bare sudo make install in a tree that
was not built leaves root-owned files in build/. After a make it only
copies, provided it gets the same variables (CPPFLAGS, LDFLAGS, BOARD
and so on) as the make before it.

It installs these six paths and nothing else:

| Path | Content |
|------|---------|
| INCLUDEDIR/librsswitch.h | the header |
| LIBDIR/librsswitch.so.1.0.0 | the shared library |
| LIBDIR/librsswitch.so.1 | link to it, named by the SONAME |
| LIBDIR/librsswitch.so | link for the linker, -lrsswitch |
| LIBDIR/librsswitch.a | the static library |
| PKGCONFIGDIR/librsswitch.pc | the pkg-config file |

The directories follow from PREFIX: LIBDIR is PREFIX/lib, INCLUDEDIR is
PREFIX/include and PKGCONFIGDIR is LIBDIR/pkgconfig, and each can be set
on its own. DESTDIR is put in front of every path, to stage the files
for a package, and the paths in librsswitch.pc do not contain it. The
Debian multiarch directory is not detected; for PREFIX=/usr give it:

```bash
sudo make install PREFIX=/usr LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)
```

Without DESTDIR the install runs ldconfig, so the loader finds the new
library; as a normal user that fails with a warning, which is harmless.
No program carries an RPATH, so a LIBDIR outside the standard library
directories needs a file in /etc/ld.so.conf.d that names it, followed by
ldconfig. make uninstall removes the six paths again.

A program finds the header and the library through pkg-config:

```bash
cc -o myprog myprog.c $(pkg-config --cflags --libs librsswitch)
```

For the libgpiod board the pkg-config file requires libgpiod. The stub
board installs under the same names, so the whole stack (the library,
the daemon and the web app) can be tried on a host without a GPIO line:

```bash
make BOARD=stub && sudo make BOARD=stub install
```

One call with make BOARD=stub install does the same, and also works
with a DESTDIR. The stub library replaces a libgpiod install below the
same PREFIX, and the other way round.


## Software setup (Raspberry Pi / Raspberry Pi 3 / BeagleBone Black)

GPIO access goes through
[libgpiod](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/),
not bcm2835 -- install the dev package on the board:

```bash
sudo apt install libgpiod-dev
```

Raspberry Pi OS and Debian 13 on the BeagleBone Black ship libgpiod
2.x, older images 1.x, and both work.

One build covers the Raspberry Pi, the Raspberry Pi 3 and the
BeagleBone Black: the board source is boards/gpiod/gpiod_board.c, and
the GPIO line is chosen at run time with --gpio. Without it the
library uses line 17 of gpiochip0, which is GPIO17 on the Raspberry Pi
and pin P9_23 of the BeagleBone Black on kernel 6.x such as on Debian
13; boards/gpiod/gpiod_board.h sets it. On other kernels the chip of
P9_23 can differ, so check it with gpioinfo and pass "chip:line" with
--gpio instead.
libgpiod 1.x and 2.x are both supported: the board picks its API from
the installed gpiod.h at compile time (gpiod.h from 1.x defines
GPIOD_LINE_BULK_MAX_LINES, the 2.x header does not), so a plain build
just works with whichever libgpiod-dev is installed. If the probe ever
picks the wrong one, force it with LIBGPIOD_VERSION:

```bash
make LIBGPIOD_VERSION=1   # force the libgpiod 1.x (gpiod_line_*) API
make LIBGPIOD_VERSION=2   # force the libgpiod 2.x (gpiod_line_request_*) API
```

The "chip:line" config format (see "GPIO configuration" below) is the
same for both versions.

Build the library first, then the programs, which link it. The
programs take BOARD, DEBUG, LIBGPIOD_VERSION and CROSS_COMPILE like the
library does. On the board itself everything uses the native gcc
(only the cross-compiling host below needs a cross toolchain):

```bash
make                      # the library, in build/gpiod/
make -C examples/basic    # rsssend
make -C examples/network  # rsswitchd
```

DEBUG=1 turns on verbose syslog output (off by default, so a release
build never logs every command):

```bash
make DEBUG=1
make -C examples/network DEBUG=1
```

The programs carry no RPATH. Run from the tree, they find the library
through LD_LIBRARY_PATH:

```bash
LD_LIBRARY_PATH=build/gpiod examples/basic/rsssend 0 0 0 1
LD_LIBRARY_PATH=build/gpiod examples/network/rsswitchd --bind 127.0.0.1
```

An installed daemon finds the installed library through the loader
cache (see "Installing the library").


## Software setup (cross compiling on x86_64)

Install an ARM/aarch64 cross toolchain (e.g. via your distro's
gcc-arm-linux-gnueabihf / gcc-aarch64-linux-gnu packages), then name
its prefix in CROSS_COMPILE, for the library and for the programs.
Nothing guesses the compiler from the host:

```bash
make CROSS_COMPILE=aarch64-linux-gnu-
make -C examples/basic CROSS_COMPILE=aarch64-linux-gnu-
make -C examples/network CROSS_COMPILE=aarch64-linux-gnu-
```

arm-linux-gnueabihf- targets ARMv7 in the same way. A Raspberry Pi
Zero (ARMv6) builds natively on the board instead. A toolchain that
lives outside PATH (for example a Linaro tarball under /opt) needs its
bin/ directory added to PATH in your shell first.

libgpiod itself (headers + library) needs to be available for the
target architecture in your cross sysroot -- either cross-build it from
source or install your distro's cross-dev variant if one exists.
A libgpiod installed under its own prefix is picked up through the
usual CPPFLAGS and LDFLAGS:

```bash
make CROSS_COMPILE=aarch64-linux-gnu- CPPFLAGS=-I<prefix>/include LDFLAGS=-L<prefix>/lib
```


## Host build (x86_64 host)

The stub board needs no libgpiod and does not touch a GPIO. From the
repository root:

```bash
make BOARD=stub
make -C examples/basic BOARD=stub
LD_LIBRARY_PATH=build/stub examples/basic/rsssend 1 17 23 1
```

```
0FFF0000F00F
```

```bash
make -C examples/network BOARD=stub
LD_LIBRARY_PATH=build/stub examples/network/rsswitchd --bind 127.0.0.1 --port 0
```


## Usage

Seven public functions, declared in librsswitch.h. The header can be
included from C++ as well: its declarations have C linkage there.

```c
rss_gpio_config(const char *config);
```

Sets the GPIO configuration string used by every following
rss_send()/rss_send_codeword() call. NULL (the default, if never
called) means "use this board's compiled-in default". Format is
per-board -- see "GPIO configuration" below.

```c
rss_gpio_config_check(const char *config);
```

Checks a GPIO configuration string without storing it and without
touching the GPIO. Returns 0 for NULL and for a string the board can
parse, and -1 for a malformed one, after the board has logged the
cause via syslog. Only the format is checked, so a well-formed string
may still name a chip that does not exist; the first send reports
that. Call it before rss_gpio_config() to refuse a bad
configuration at start-up, as rsssend and rsswitchd do.

```c
rss_send(uint32_t dev, uint32_t group, uint32_t socket, uint32_t data);
```

- dev: 0 = REV 008345, 1 = Pollin 2605
- group: REV 008345 0-3, Pollin 2605 0-31
- socket: REV 008345 0-2, Pollin 2605 0-31
- data: 1 = On, 0 = Off

Returns 0 on success, -1 for an out-of-range group/socket/data, an
unknown dev, or a board error; rss_last_error() then tells why.
Arguments are fully validated before board_init() or any GPIO access,
so an invalid address never touches the board and nothing is sent.

```c
rss_send_codeword(const char *tristate, uint32_t pulse_len);
```

Sends a raw tri-state codeword instead of one of the two built-in
device tables above.

- tristate: 1 to RSS_CODEWORD_MAX_LEN (12) bytes of '0', '1' and
  uppercase 'F'
- pulse_len: microseconds, between RSS_PULSE_LEN_MIN_US (50) and
  RSS_PULSE_LEN_MAX_US (2000)

Input is fully validated before board_init() or any GPIO access, so an
invalid tristate or pulse_len never touches the board. On success,
tristate is sent 10 times, each repetition followed by a sync pulse --
the same repeat count rss_send() uses. The stub board prints the
codeword to stdout, exactly as it does for rss_send().

Returns 0 on success, -1 on error (invalid input, or a board error);
rss_last_error() then tells why.

```c
rss_last_error(void);
```

Returns the cause of the calling thread's last rss_send() or
rss_send_codeword() call as an `enum rss_err`:

- RSS_ERR_NONE (0): no failure
- RSS_ERR_OTHER (1): a failure the library cannot classify,
  including every board that only reports a plain -1
- RSS_ERR_NOCHIP (2): the GPIO chip does not exist
- RSS_ERR_BUSY (3): the line is in use by another program or
  process
- RSS_ERR_DENIED (4): no permission to open the chip
- RSS_ERR_BADLINE (5): the line offset is not valid for the chip
- RSS_ERR_IO (6): driving the line failed during the send
- RSS_ERR_ARG (7): the call was rejected before the board was
  touched (an invalid dev, group, socket, data, codeword or pulse_len)

Every send call sets the cause, so it is RSS_ERR_NONE after a
success. The cause is kept per thread: a send in another thread never
changes it. The other functions leave it unchanged. The values never
change; a later cause is only added after RSS_ERR_ARG. Which
causes a failure can have besides OTHER depends on the board; the
libgpiod boards are described in "GPIO configuration" below.

```c
rss_addr_limits(uint32_t dev, uint32_t *ngroups, uint32_t *nsockets,
uint32_t *ndata);
```

Reports the valid address range for dev, the same bounds rss_send()
enforces: REV 008345 has 4 groups, 3 sockets and 2 data values; Pollin
2605 has 32 groups, 32 sockets and 2 data values. Lets a caller (such
as rsswitchd) validate an address itself before calling rss_send(),
reading the same tables rss_send() does instead of copying the
sizes elsewhere.

Returns 0 and fills `*ngroups`, `*nsockets` and `*ndata`, or -1 for an
unknown dev or a NULL pointer, leaving the outputs untouched.

```c
const char *rss_version(void);
```

Returns the version of the library that is actually loaded, as
"MAJOR.MINOR.PATCH" in static storage, for example "1.0.0". The header
carries the version of the library it belongs to as
LIBRSSWITCH_VERSION_MAJOR, LIBRSSWITCH_VERSION_MINOR and
LIBRSSWITCH_VERSION_PATCH and as the string LIBRSSWITCH_VERSION built
from them. The two can differ when a program runs against another
build of the library than the one it was compiled with. MAJOR is the
SONAME and changes only for an incompatible ABI change, MINOR grows
with new API, PATCH with fixes. The version is the library's own and
independent of the tags of the project.


## Thread safety

rss_send(), rss_send_codeword(), rss_addr_limits(),
rss_gpio_config_check(), rss_gpio_config() and
rss_last_error() are safe to call concurrently from multiple
threads of the same process. Every send session -- board_init(), all
repetitions, board_exit() -- is serialized by one library-wide lock,
so concurrent sends never interleave their pulses; a calling thread
simply blocks until the previous send session completes.
rss_gpio_config() takes the same lock around its store, so a config
change is ordered against sends that start after it returns (it does
not affect a session already in progress). rss_addr_limits() and
rss_gpio_config_check() take no lock at all: the first only reads
constant tables, the second only parses the string it is given, and
neither touches gpio_config or opens the board, so they never block
and never wait behind a send in progress. rss_last_error() takes no
lock either: it reads state that belongs to the calling thread only.

This guarantee is process-local only: two separate processes driving
the same GPIO line (for example rsssend run once while rsswitchd is
active) are not serialized against each other.

The library is not async-signal-safe -- never call any of these
functions from a signal handler. Cancelling a thread while it is
inside a send is not supported; doing so may leave the lock held.


## GPIO configuration

The config string passed to rss_gpio_config() (or "--gpio" on the
example programs) is board-specific:

- Raspberry Pi / Raspberry Pi 3 / BeagleBone Black: "chip:line", e.g.
  "gpiochip0:17". NULL selects the board's default, gpiochip0:17. A
  string not of the form "chip:line" (for example a bare GPIO number
  such as "49", the BeagleBone Black's former format, a chip without
  a line, a chip name containing "/", or a chip name that does not
  start with a letter or digit, such as "..") is malformed. rsssend and
  rsswitchd check the string with rss_gpio_config_check() and
  refuse to start with exit status 1 and
  `<prog>: malformed --gpio '<value>'` on stderr, while the library
  logs "[-] Malformed GPIO config". On these libgpiod boards, a
  program that stores such a string with rss_gpio_config() without
  checking it still gets -1 from rss_send() and
  rss_send_codeword(), and no GPIO chip is opened. The default is
  GPIO17 (header pin 11) on the Raspberry Pi, and P9_23 on a
  BeagleBone Black with Debian 13 (kernel 6.x). On other kernels the
  chip of P9_23 can differ: find it with `gpioinfo` from the gpiod
  package and pass it as "chip:line".

  After a failed send, rss_last_error() reports on these boards:
  RSS_ERR_NOCHIP when the chip does not exist, RSS_ERR_BUSY when
  the line is in use by another program, RSS_ERR_DENIED when the
  process may not open the chip, RSS_ERR_BADLINE when the line
  offset is not valid for the chip, and RSS_ERR_IO when a write to
  the line failed. Every other failure is RSS_ERR_OTHER: a
  malformed config, an allocation failure and, on libgpiod 1.x, a
  device that exists but is not a GPIO chip. The last one stays
  "other" on purpose, because a wrong cause is worse than none.
- stub: the stub build checks the string with the same "chip:line"
  rules, so the programs refuse the same values, and its board
  drives no GPIO. Apart from the chip names below, the stub ignores the
  string stored with rss_gpio_config(), so on this build
  rss_send() and rss_send_codeword() succeed even with a
  malformed one.

  A chip named failnochip, failbusy, faildenied, failbadline, failio
  or failother (followed by ":" and a line, e.g. "failbusy:0") makes
  the stub fail on purpose, so every failure cause can be tested
  without hardware. rss_last_error() then reads RSS_ERR_NOCHIP,
  RSS_ERR_BUSY, RSS_ERR_DENIED, RSS_ERR_BADLINE,
  RSS_ERR_IO or RSS_ERR_OTHER. failio fails at the first pulse,
  so the codeword is still printed; the others fail before anything is
  sent. Every send fails while such a name is set. On a libgpiod board
  these names are ordinary chip names that do not exist, so they report
  a missing chip.


## Running the tests (x86_64 host)

From the repository root:

```bash
make -C tests venv test
```

The venv target makes the web app's virtual environment in
examples/web/venv from examples/web/requirements-dev.txt. The first run
downloads the packages, and later runs only check that the venv is
there. A venv directory that has no pip, for example one left by an
interrupted run, is removed and made again. `make -C tests venv test`
makes the venv first, with -j as well. `make -C tests test` then runs
every C test, then the tests of the web app, including the end-to-end
test against the stub-board rsswitchd that it builds. It exits non-zero
when any of them fails. It fails, and never skips, when the
venv, pytest or the daemon is missing; it never makes the venv itself.
A venv at another place is named with WEB_PYTHON, as the absolute path
of the python in its bin directory:

```bash
make -C tests test WEB_PYTHON=$PWD/examples/web/venv/bin/python
```

WEB_VENV=/path makes and uses a venv at that place instead, with
`make -C tests venv test WEB_VENV=/path`.

On a host where libgpiod cannot be linked, the libgpiod test
(test_gpiod) is skipped. Make warns about it at the start, and
`make -C tests test` prints a line
`SKIPPED: test_gpiod (libgpiod not linkable)` after the results. The exit status is not changed by the skip.
GPIOD_EXPECT_VERSION (see below) turns the skip into an error.

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
ignore the resulting code. The host tests cannot detect this, since
the stub board has no real timing constraint to violate.
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
