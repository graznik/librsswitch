# rsswitchd

rsswitchd is a network daemon for the Raspberry Pi, BeagleBone
Black and a stub build for tests. It links
[librsswitch](https://github.com/graznik/librsswitch) and
serves a line-based TCP protocol that switches REV 008342 and
Pollin 2605 sockets on or off.


## Installation (Raspberry Pi, BeagleBone Black)

rsswitchd links the shared library of this repository, so build the
library first and then the daemon:

```bash
cd librsswitch
make
sudo make install
make -C examples/network
sudo make -C examples/network install
```

The library goes in first (`make && sudo make install`, see
"Installing the library" in the main README). sudo make -C
examples/network install installs only the daemon and its unit, and
stops with a hint, installing nothing, when librsswitch.so.1 is
missing in LIBDIR. A library installed with another LIBDIR needs the
same LIBDIR on the daemon's install command.

The same build serves the Raspberry Pi, the Raspberry Pi 3 and the
BeagleBone Black. sudo make install creates the gpio group and the
rsswitchd system user when either is missing, installs the binary and
the unit, then enables the service and restarts it, so that it runs
the binary that was just installed -- one step end to end.

Running sudo make install again after a new build is the upgrade: the
group and the user are kept, the binary and the unit are replaced, and
the daemon is restarted. An rsswitchd that was installed before the
library was a shared one has the library code inside it. Upgrading it
needs the library installed first, as above, or the new daemon cannot
start and systemd gives up after the start limit.

For a staged install (packaging, or checking the result without
root), use `make install DESTDIR=<dir>`: the binary and unit file
are placed under `<dir>`, and both the account creation and the
systemctl steps are skipped.


## Installation (x86_64 host)

From the repository root:

```bash
make BOARD=stub
make -C examples/network BOARD=stub
LD_LIBRARY_PATH=build/stub examples/network/rsswitchd --help
```

The daemon carries no RPATH: from the tree it finds the library through
LD_LIBRARY_PATH, and an installed daemon through the loader cache.


## Running

```
 --port PORT    TCP port to listen on, 0-65535; 0 lets the
                kernel pick a free port (default 5000)
 --bind ADDR    IPv4 address to listen on (default 0.0.0.0,
                meaning all addresses)
 --gpio CONFIG  board-specific GPIO config (default: the
                board's built-in default)
 --timeout SECS closes a connection when no complete command
                arrives for SECS seconds, 1-3600 (default 10)
 --help         show usage and exit
 --version      print the librsswitch version and exit
```

--version prints the version of the library the loader picked, which
need not be the one the daemon was built against.

Exit status is 0 for --help, --version or a clean stop by
SIGTERM/SIGINT, 1 when socket setup fails or --gpio is malformed (both
printed on stderr), or 2 for a usage error.

As soon as the daemon is listening, it prints one line to
stderr: `rsswitchd: listening on ADDR:PORT`. With --port 0 this
is the only way to learn the port the kernel picked; scripts and
tests can wait for this line instead of guessing a delay.

SIGTERM and SIGINT stop the daemon cleanly: any send already in
progress, and its reply, always finish first, then the daemon
closes its connection and its listening socket and exits 0.


## Protocol

Commands are lines ending in `\n` or `\r\n`.

Protocol v2 is `dev group socket data`, with single spaces,
plain decimal digits and no leading zeros:

```
 dev   0 = REV 008342     1 = Pollin 2605
 group 0-3                0-31
 socket 0-2               0-31
 data  1 = on, 0 = off (both devices)
```

The 4-digit command format of earlier versions is no longer
accepted, because it could not address Pollin groups or sockets
above 9. Such a line is answered ERR syntax; a client sends
`dev group socket data` lines instead.

The raw command is `raw <tristate> <pulse_len>`: 1-12 characters
of 0, 1 and F, followed by a pulse length in microseconds,
50-2000. Both arguments are required.

An empty line is ignored and gets no reply. A line may hold at
most 128 bytes before its `\n`, a CR included; a longer line gets
ERR toolong (see Replies) and the rest of that line is discarded.

The daemon serves one connection at a time; other clients wait in
a queue until the current connection closes or times out. Several
commands may be sent on one connection, and they run in the order
they arrive. A connection is closed when no complete command has
arrived for --timeout seconds, or when a reply cannot be written
within that time; a command not yet fully sent when this happens
is dropped, not run.


## Replies

Every command gets exactly one reply line: `OK`, once the
codeword was actually sent, or `ERR <class> <text>`. The class
word is fixed; the text after it may change.

```
 syntax   malformed command
 range    a well-formed value outside its bounds, e.g.
          "group 40 not in 0-31", "dev 9 unknown",
          "tristate longer than 12",
          "pulse_len 49 not in 50-2000"
 toolong  line longer than 128 bytes
 gpio     the GPIO failed with a cause the library could
          tell, e.g. "ERR gpio busy GPIO line in use by
          another program"
 send     transmission failed -- the daemon's own checks
          passed, but the library reported a board error it
          could not classify: "ERR send transmission failed"
```

For `gpio` the word after the class is fixed too, and it names the
cause. The text after the word may change, so a client should match
on the class and the word only:

```
 ERR gpio nochip GPIO chip not found
 ERR gpio busy GPIO line in use by another program
 ERR gpio denied no permission on the GPIO chip
 ERR gpio badline GPIO line not valid for this chip
 ERR gpio io writing to the GPIO line failed
```

nochip: the chip named by --gpio does not exist. busy: another
program holds the line. denied: the daemon may not open the chip.
badline: the chip has no such line. io: the line was found, but
driving it failed. The reply never carries the chip name, a device
path or the system's error text; those are in the syslog.

On the stub build, which has no GPIO, `--gpio` can name a
chip such as `failbusy:0` to make every command fail with that
cause, for tests; the top-level README lists the names.

The connection stays open after every reply, including an ERR.
A command that was never fully read when the connection ends
gets no reply at all.


## Examples

```bash
printf '0 3 0 0\n' | nc -N 127.0.0.1 5000
```

```
OK
```

```bash
printf '0 3 0 0\r\n' | nc -N 127.0.0.1 5000
```

```
OK
```

```bash
printf '0 3 0 0\n0 3 0 1\n' | nc -N 127.0.0.1 5000
```

```
OK
OK
```

```bash
printf '1 17 23 1\n' | nc -N 127.0.0.1 5000
```

```
OK
```

```bash
printf 'raw 0FFF0000F00F 350\n' | nc -N 127.0.0.1 5000
```

```
OK
```

```bash
printf '1 017 23 1\n' | nc -N 127.0.0.1 5000
```

```
ERR syntax malformed command
```

```bash
printf '1 40 0 1\n' | nc -N 127.0.0.1 5000
```

```
ERR range group 40 not in 0-31
```

On the stub build, `1 17 23 1` and the raw command above
both print the codeword 0FFF0000F00F on the daemon's own stdout,
and `0 3 0 0` prints FFF11FF00001.


## Security and service

The daemon has no authentication: whoever can reach the port can
switch any configured socket. The shipped systemd unit therefore
starts it with `--bind 127.0.0.1`, the recommended setup behind
the web app on the same host; edit the unit's ExecStart line to
serve other hosts. The program's own default, used when --bind
is not given, stays 0.0.0.0.

The unit is ordered after network-online.target and wants it. That
matters only when the edited ExecStart line binds a LAN address that
DHCP assigns at boot: a start before the address exists fails, and
after the start limit below the unit stays failed.
The target waits only when a wait-online service is enabled, and then
the start at boot is delayed until the network is online or the
timeout of that service (often 60 to 90 seconds) has passed, even
with the shipped `--bind 127.0.0.1`, which needs no network. If you
keep it, remove network-online.target from the After= and Wants=
lines of the unit to start at once. This shows whether a wait-online
service is enabled:

```bash
systemctl is-enabled NetworkManager-wait-online.service \
	systemd-networkd-wait-online.service
```

The unit runs the daemon as the unprivileged rsswitchd user, in
the gpio group (SupplementaryGroups=gpio). libgpiod opens
`/dev/gpiochip*` on both boards, so that group needs read and write
access to it.

Raspberry Pi OS and the BeagleBoard.org Debian images grant this
with a udev rule. On another image, put the rule into a file under
`/etc/udev/rules.d/`, e.g. 85-gpio.rules:

```
KERNEL=="gpiochip*", SUBSYSTEM=="gpio", MODE="0660", GROUP="gpio"
```

Then reload the rules and trigger the gpio subsystem, or reboot:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=gpio
```

`ls -l /dev/gpiochip0` must then show group gpio and mode
`crw-rw----`.

The unit also sets NoNewPrivileges, ProtectSystem=strict,
ProtectHome, PrivateTmp and Restart=on-failure. It deliberately
leaves out PrivateDevices and DevicePolicy, which would hide
`/dev/gpiochip*` and break GPIO access.

With an edited ExecStart line, the GPIO line is chosen with
`--gpio chip:line`, e.g. `--gpio gpiochip0:17`. Any other form, such
as a bare GPIO number, makes the daemon refuse to start: it prints
`rsswitchd: malformed --gpio '<value>'` on stderr, which
`journalctl -u rsswitchd` shows, logs the reason as
`[-] Malformed GPIO config`, and exits with status 1. The unit then
fails after the start limit below, instead of running with a GPIO line
it cannot use. Only the format is checked: a well-formed `chip:line`
that names a missing chip still starts, and each switching command is
answered with `ERR gpio nochip GPIO chip not found` until the ExecStart
line is fixed.

When the daemon cannot start, for example because another program
holds port 5000, systemd starts it at most 5 times within 60
seconds, 2 seconds apart (`StartLimitIntervalSec`,
`StartLimitBurst`), the first start included. Then it gives up and
the unit is failed, with the reason in `journalctl -u rsswitchd`.
Until 60 seconds after the first of these starts, systemd refuses a
start by hand as well, and `sudo systemctl reset-failed rsswitchd`
clears that. `sudo make install` clears the start limit itself before
it restarts the service, so a fixed build can be installed at once.


## Logging

One syslog line is written per connection, per successfully sent
command, and per ERR reply, plus "Shutting down." on a clean
stop. Read them with:

```bash
journalctl -t rsswitchd
```


## References

- [librsswitch](https://github.com/graznik/librsswitch)
- [graznik.de](https://graznik.de): hardware and build-environment setup
