# rsswitchd

rsswitchd is a network daemon for the Raspberry Pi, BeagleBone
Black and an x86_64 test build. It links
[librsswitch](https://github.com/graznik/librsswitch) and
serves a line-based TCP protocol that switches REV 008345 and
Pollin 2605 sockets on or off.


## Installation (Raspberry Pi, BeagleBone Black)

```bash
cd librsswitch/examples/network/
make rpi     # or: make rpi3 on a 64-bit Raspberry Pi OS
             # or: make bbb on a BeagleBone Black
sudo make install
```

sudo make install creates the gpio group and the rsswitchd system
user when either is missing, installs the binary and the unit, then
enables the service and restarts it, so that it runs the binary that
was just installed -- one step end to end.

Running sudo make install again after a new build is the upgrade: the
group and the user are kept, the binary and the unit are replaced, and
the daemon is restarted.

For a staged install (packaging, or checking the result without
root), use `make install DESTDIR=<dir>`: the binary and unit file
are placed under `<dir>`, and both the account creation and the
systemctl steps are skipped.


## Installation (x86_64 host)

From examples/network:

```bash
make x86_64
./rsswitchd --help
make install DESTDIR=/tmp/rsswitchd-stage
ls /tmp/rsswitchd-stage/usr/local/bin/rsswitchd /tmp/rsswitchd-stage/etc/systemd/system/rsswitchd.service
```


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
```

Exit status is 0 for --help or a clean stop by SIGTERM/SIGINT, 1
when socket setup fails or --gpio is malformed (both printed on
stderr), or 2 for a usage error.

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
 dev   0 = REV 008345     1 = Pollin 2605
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
 send     transmission failed -- the daemon's own checks
          passed, but the library reported a board error,
          e.g. "ERR send transmission failed"
```

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

On the x86_64 test build, `1 17 23 1` and the raw command above
both print the codeword 0FFF0000F00F on the daemon's own stdout,
and `0 3 0 0` prints FFF11FF00001.


## Security and service

The daemon has no authentication: whoever can reach the port can
switch any configured socket. The shipped systemd unit therefore
starts it with `--bind 127.0.0.1`, the recommended setup behind
the web app on the same host; edit the unit's ExecStart line to
serve other hosts. The program's own default, used when --bind
is not given, stays 0.0.0.0.

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
answered with `ERR send transmission failed` until the ExecStart line
is fixed.

When the daemon cannot start, for example because another program
holds port 5000, systemd tries again after 2 seconds, at most 5
times within 60 seconds (`StartLimitIntervalSec`,
`StartLimitBurst`). Then it gives up and the unit is failed, with
the reason in `journalctl -u rsswitchd`. Until 60 seconds after the
first of these starts, systemd refuses a start by hand as well, and
`sudo systemctl reset-failed rsswitchd` clears that.


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
