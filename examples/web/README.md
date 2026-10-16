# rsswitch-web

rsswitch-web is a Flask app that keeps rooms and sockets in a JSON
file and switches them through
[rsswitchd](../network/README.md) over TCP. It never touches GPIO
itself.


## Installation as a service

This is the recommended way on a Raspberry Pi, and on any other host
with systemd. The app runs under waitress, as the unprivileged user
rsswitch-web, and starts at boot after rsswitchd. From examples/web:

```bash
sudo apt install python3-venv
make -C ../network rpi     # or: rpi3, bbb, x86_64
sudo make -C ../network install
sudo make install
```

The second and third line build and install
[rsswitchd](../network/README.md), which the app needs to switch
anything. sudo make install in this directory then

- creates the system user rsswitch-web when it is missing. It has no
  login shell and no home directory, and is in no group but its own:
  the app needs no GPIO access, rsswitchd owns the GPIO lines;
- installs the app into /opt/rsswitch-web, and creates the virtual
  environment /opt/rsswitch-web/venv there when it is missing, with
  the packages of requirements.txt. This needs network access, and
  takes a while on a slow board;
- installs config.example.json as /etc/rsswitch-web/config.json,
  only when that file does not exist yet. It is readable by the user
  rsswitch-web and by nobody else but root;
- creates /var/lib/rsswitch-web, which holds the data file and
  belongs to the user rsswitch-web;
- installs the unit /etc/systemd/system/rsswitch-web.service, enables
  it and restarts it.

The configuration that is installed listens on 127.0.0.1, port 8080,
and asks for no password, so only the host itself can reach the app
at first. To use it from a phone, set a password, see the next
section, and then set listen_address to the address of the Pi, or to
0.0.0.0 for all of its IPv4 addresses.

The unit is hardened: besides the unprivileged user, it makes the
file system read-only except the data directory, hides the home
directories and the devices, and allows no address family but IP and
local sockets. It also drops all capabilities, forbids namespaces and
real-time scheduling, and filters the system calls to those of a
normal service without the privileged ones. See the comments in
rsswitch-web.service.


## Setting a password for the service

Make a hash of the password, as described under Password. The
command asks for the password twice and prints the hash; it reads no
configuration, so any user can run it:

```bash
cd /opt/rsswitch-web && venv/bin/python -m rsswitch_web hash-password
```

Put the username and the printed hash into the configuration, set
listen_address there when the phones are to reach the app, and
restart the service, because the file is read once at start-up:

```bash
sudoedit /etc/rsswitch-web/config.json
sudo systemctl restart rsswitch-web
```


## Running alongside rsswitchd

rsswitchd.service starts rsswitchd with `--bind 127.0.0.1`, so it
listens on 127.0.0.1:5000 only. These are the defaults of
daemon_host and daemon_port, so the app needs no setting to find it.
Keep rsswitchd on 127.0.0.1: the app is then the only way to reach
it, and the app can have a password.

rsswitch-web.service is ordered after rsswitchd.service and wants it,
so both start at boot, the daemon first. The app does not need the
daemon to start: while it is down, a switch fails with "rsswitchd
unreachable" and the page says so.

```bash
systemctl status rsswitchd rsswitch-web
journalctl -u rsswitch-web
```

The journal of rsswitch-web has the listening line and the errors of
the app, such as a data file that cannot be used. waitress logs no
requests. The journal of rsswitchd shows each codeword that was sent.

When the app stops at start-up with an error, for example a
configuration that it refuses, systemd starts it again after 2
seconds, at most 5 times within 60 seconds. Then it gives up and
leaves the unit failed, which `systemctl status rsswitch-web` shows,
with the reason in `journalctl -u rsswitch-web`. Fix the cause and
start the service again. Until 60 seconds after the first of those
starts, systemd refuses a start by hand as well, and the journal says
"Start request repeated too quickly". `systemctl reset-failed` clears
that.

```bash
sudo systemctl reset-failed rsswitch-web
sudo systemctl restart rsswitch-web
```


## Upgrading

Update the source tree, build and install rsswitchd again when it
has changed, and run sudo make install once more in this directory:

```bash
sudo make install
```

The code is replaced, so a module that the new version no longer has
is gone, and requirements.txt is installed into the venv again. The
configuration and the data file are not touched, and the service is
restarted to run the new code. Installing rsswitchd again with
`sudo make -C ../network install` restarts rsswitchd as well, so the
new daemon runs at once.


## Staged install

make install DESTDIR=DIR only copies the files below DIR. It creates
no user, no venv, and runs no systemctl, which makes it fit for
packaging, and for looking at what an install puts where:

```bash
make install DESTDIR=/tmp/rsswitch-web-stage
ls -l /tmp/rsswitch-web-stage/etc/systemd/system/rsswitch-web.service /tmp/rsswitch-web-stage/etc/rsswitch-web/config.json
```

A venv cannot be moved, because it records its own path; the
packager makes it at the final place, from requirements.txt.


## Manual installation (Raspberry Pi)

For development, or to run the app without systemd, make a virtual
environment by hand. For production use the service installation
above.

```bash
sudo apt install python3-venv
cd librsswitch/examples/web/
python3 -m venv venv
venv/bin/pip install -r requirements.txt
```

The app gets its own virtual environment so that pip never touches
the system Python (PEP 668). Never use sudo pip or
--break-system-packages.


## Installation (x86_64 host)

From examples/web:

```bash
make -C ../network x86_64
python3 -m venv venv
venv/bin/pip install -r requirements.txt
venv/bin/pip install -r requirements-dev.txt
venv/bin/python -m pytest
```

requirements-dev.txt adds pytest. Python 3.9 or newer is required.
The suite includes a smoke test against that x86_64 rsswitchd and
skips it when the binary is missing. See Tests below for the one
command that runs everything.


## Tests

One command runs every test of the repository, the C tests first and
the tests of this app second. From examples/web, with the venv made
as above (the first two commands make it, once):

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-dev.txt
make -C ../../tests test
```

That is `make -C tests test` from the repository root. It needs the
venv in examples/web/venv with requirements-dev.txt installed. A venv
at another place is named with WEB_PYTHON, which must be the python of
that venv, as an absolute path:

```bash
make -C ../../tests test WEB_PYTHON=$PWD/venv/bin/python
```

The command builds the x86_64 rsswitchd, starts it for the tests that
need the real daemon, and runs the end-to-end test: it starts the
daemon and the app under waitress on ephemeral ports and drives the
app over HTTP, down to the codeword that the daemon prints. The app
tests run also when a C test failed, so one run shows both results.
The command exits non-zero when anything failed, and also when the
venv, pytest or the daemon is missing. It never skips the app's tests
or the end-to-end test.

`venv/bin/python -m pytest` in examples/web runs only the app's tests.
The real-daemon and end-to-end tests are skipped then when
examples/network/rsswitchd is missing. Two environment variables
change that: RSSWITCHD_BIN names the rsswitchd binary to start, which
must be a build for the x86_64 stub board, and RSSWITCH_REQUIRE_DAEMON=1
makes a missing or unstartable binary fail the tests instead of
skipping them. make -C tests test sets both.


## Trying the stack on an x86_64 host

This needs no root and no systemd. It runs the daemon of the x86_64
build, which only prints the codeword it would send, and the app
against it. After the steps of the installation above, from
examples/web:

```bash
make -C ../network x86_64
../network/rsswitchd --bind 127.0.0.1 --port 5000 &
cat > /tmp/rsswitch-web-try.json <<'CONFIG'
{
  "listen_address": "127.0.0.1",
  "data_file": "/tmp/rsswitch-web-try-data.json"
}
CONFIG
venv/bin/python -m rsswitch_web --config /tmp/rsswitch-web-try.json serve
```

Open `http://127.0.0.1:8080/`, add a room and a socket, and switch
it: the daemon prints the codeword. Stop the app with Ctrl-C and the
daemon with `kill %1`.


## Configuration

The configuration is one JSON object in a file. Its path is given
with --config PATH, before the command; the default is
/etc/rsswitch-web/config.json. When the default file does not
exist, the built-in defaults are used and a warning is printed on
stderr. A file that is named with --config must exist, or the app
exits with status 2. config.example.json lists every key with its
default.

Every key is optional:

| Key | Meaning |
|-----|---------|
| `daemon_host` | where rsswitchd listens (default `"127.0.0.1"`); an IP address, not a host name: write `"127.0.0.1"` where you used `"localhost"`, and the app refuses to start with a message that says so |
| `daemon_port` | TCP port of rsswitchd (default 5000); an integer, 1-65535 |
| `daemon_connect_timeout` | seconds to wait for the connection to rsswitchd (default 2); a number above 0 and at most 60 |
| `daemon_read_timeout` | seconds to wait for its reply (default 5); a number above 0 and at most 60 |
| `listen_address` | address the app listens on (default `"127.0.0.1"`, this host only; `"0.0.0.0"` is all IPv4 addresses and `"::"` all IPv6 ones, set a password first); an IP address, not a host name |
| `listen_port` | port the app listens on (default 8080); an integer, 0-65535; 0 lets the kernel pick a free port |
| `threads` | worker threads of `serve` (default 4); an integer, 1-16; `run` ignores it. It bounds how many requests, password checks and rsswitchd calls run at once, see below |
| `data_file` | the file that holds the rooms and the sockets (default `"/var/lib/rsswitch-web/data.json"`); an absolute path |
| `all_off_gap_ms` | pause between two commands of an All-off in milliseconds (default 200); an integer, 0-10000 |
| `username` | the login name (default `null`, which means "admin" when `password_hash` is set); no ":" in it |
| `password_hash` | hash of the password (default `null`, no login is asked for); a Werkzeug scrypt or pbkdf2 hash, see Password |

One All off holds a thread for its whole run, which lasts as long as
its sockets need with all_off_gap_ms between them. With the default
of 4 threads, four All offs at the same time keep every other request
waiting until one of them has finished.

An unknown key, a value of the wrong type or a value outside its
range stops the app at start-up with the key named in the message,
and with a hint when the key looks like a misspelt one. A username
without a password_hash is an error as well. The file is read once,
at start-up, so a change needs a restart.

The directory of data_file must exist and be writable by the user
that runs the app. The file is created on the first change, with
mode 0600, and a lock file with the same name and the suffix .lock
is kept next to it. The data file must not be edited while the app
runs. A data file that cannot be read, or that has another format,
is never overwritten; the app refuses to start instead.


## Password

The configuration holds a hash of the password, never the password
itself. rsswitch_web creates the hash:

```bash
venv/bin/python -m rsswitch_web hash-password
```

It asks for the password twice without echoing it, so that the
password is neither in the shell history nor in the process list,
and prints one line, the hash. Paste that line into the config
file:

```json
{
  "username": "admin",
  "password_hash": "<hash from hash-password>"
}
```

The username defaults to admin and may be left out. A username
without a password_hash is an error.

The default hash is scrypt, which needs about 32 MiB of memory for
every check. The app checks a login only once per credential, but
on a slow board such as an ARMv6 Raspberry Pi that check still takes
a while. A cheaper hash is made with

```bash
venv/bin/python -m rsswitch_web hash-password --method pbkdf2:sha256:600000
```

The app refuses a scrypt hash that OpenSSL cannot compute here, or
that needs more than 256 MiB of memory or has p above 16, and a
pbkdf2 hash with more than 10000000 iterations, when it starts.

It also refuses a hash of the empty password when it starts, because
that would let anybody in with an empty password, and a login that
looks protected is worse than none. This costs one hash at start-up.

hash-password reads no config file. It exits with 1 when the two
entries differ, when the password is empty or is not valid text
(it cannot be encoded as UTF-8), or when the input ends, and
with 2 when --method is not a method that the app accepts. It
prints nothing on stdout in these cases.


### Failed logins

After 5 failed logins within 60 seconds from one address, every
request from that address is answered with 429, the code
`too_many_attempts` and a `Retry-After` header, for 60 seconds. The
app checks no password for it in that time, not even the right one.
Then the address can log in again.

Only requests that carry a username and a password and fail count.
A request without credentials, which is what a browser sends before
it shows its login prompt, does not count. A successful login clears
the count of its address. A request whose credentials the app has
accepted before, and remembers, is no login: it does not clear the
count.

The counts are kept in memory only, at most 256 addresses, and a
restart clears them. The address is the one of the TCP connection;
headers such as `X-Forwarded-For` are ignored, because a client
could set them to anything. So behind a proxy that hides the
addresses of the clients, such as one that provides TLS (see
[Security](#security)), all clients share the count of the proxy:
whoever can reach the proxy can lock everybody out, the owner
included, with 5 wrong logins, again every 60 seconds. Devices
behind one NAT address share one count as well. Serve the app
directly on a LAN that you trust, or let only trusted clients reach
the proxy. A browser that holds a wrong saved password locks itself
out the same way; wait a minute and enter the right one.


## Running

```bash
venv/bin/python -m rsswitch_web --config /etc/rsswitch-web/config.json serve
```

serve is the production server. It runs the app under waitress, a
pure Python WSGI server that also runs on a Raspberry Pi with an
ARMv6 CPU, with the number of worker threads from the `threads` key.
It checks the configuration and the data file and binds the address
before it prints anything, so the exit status and the diagnostics are
the same as for run. waitress logs no requests. serve needs waitress
in the venv, which `pip install -r requirements.txt` provides; without
it the command exits with 1 and says so.

```bash
venv/bin/python -m rsswitch_web --config /etc/rsswitch-web/config.json run
```

run starts the development server of Werkzeug. It is meant for
development and testing, and says so on stderr when it starts:
`rsswitch-web: development server only, not for production use`. It
ignores `threads` and needs no waitress.

As soon as the app is listening, both commands print the line
`rsswitch-web: listening on ADDR:PORT` to stderr. With listen_port 0
this is the only way to learn the port the kernel picked; scripts
and tests can wait for this line instead of guessing a delay.

SIGTERM and SIGINT stop the app with status 0. The exit status is
1 when the data file cannot be used or the address cannot be
listened on, and 2 for a usage error or a configuration that is
refused. Every diagnostic is one line on stderr that starts with
`rsswitch-web: `.


## The page

Open `http://ADDRESS:PORT/` on a phone in the same network, with the
listen_address and listen_port of the configuration. The page shows
one card per room and one row per socket, in the order of the API.
Each row has an On and an Off button, at least 48 px tall, and works
in light and in dark mode, following the setting of the phone.

Under the name of each socket is a label with the last command that
was sent to it: "Never sent", or for example "Last sent: On · 07:12",
with the date in front of the time when the command was not sent
today. It is not the real state of the socket. The radio is one-way,
so the app never learns whether a socket obeyed, and someone may
also have pressed the button on the socket. The page says so in a
line at its top and in a tooltip on every label.

A tap sends the command at once. While it runs, the tapped button
shows "Sending…" and only the two buttons of that row are disabled.
On success the label changes and flashes. On failure a red message
appears under the row and stays until the next tap on that row, for
example "rsswitchd unreachable" or "rsswitchd timed out". The
message of the daemon is shown as "rsswitchd: ", followed by its
text.

Each room card has an All off button, which switches off the sockets
of that room at once, like a tap on a single socket. The All off
button at the top asks "Switch off all N sockets?" first, with N the
number of sockets on the page, and does nothing when the answer is
no. While a request runs, its button shows "Sending…", and the
request can take a while, because the app pauses between the
sockets. The answer appears as a banner such as "All off: 5 ok, 1
failed, 2 skipped". Each socket that failed gets the error under its
row, and each socket that was skipped gets "skipped: " and the
reason. The sockets that worked show their new label.

The page does not wait for ever for an answer. For a switch it waits
as long as the app can take for one command, which is the connect and
the read timeout together, and for an All off as long as for one
command and one pause for each socket, with 10 seconds more. It
learns both from `GET /api/meta`, so a longer `daemon_read_timeout`
needs no change in the page. When the time is over, the row says "no
answer in time, it may have been sent": the command may still have
reached rsswitchd, so look at the socket, not at the label.

The page loads the rooms again whenever it is shown again, for
example after switching back to the browser, so that changes made
from another phone appear without a manual reload. A message under a
row and a row that is still sending stay as they are. The page does
not poll.

The Edit button at the top switches the page into edit mode, and
Done switches it back. In edit mode the On and Off buttons and the
All off buttons of the rooms are hidden, and every room card has a
"+ Socket" button. It opens a form with the name, the type, the
group, the number and the room of the new socket. The limits of group
and number follow the chosen type, from `GET /api/meta`, and the form
refuses a value outside them before it sends anything; the server
checks it again. An error of the server appears next to the field it
names, or at the bottom of the form, and the form stays open. A
socket with the same address as another one is saved, and the page
says "Same address as: " followed by the other sockets and their
rooms. A browser without the dialog element shows a note instead of
edit mode, and switching keeps working.

In edit mode a "+ Room" button follows the room cards. It asks for a
name, and a name that another room has already is refused inside the
form. Each room head has a pencil button that renames the room and a
bin button that deletes it. Delete asks first, naming the room, and
does nothing when the answer is no. A room must be empty before it
can be deleted: for a room that still holds sockets, the server's
reason appears on its card until the next action on that card, and
the room stays.

In edit mode each socket row has a pencil button and a bin button.
The pencil opens the form of the socket with its current values. Only
the fields that changed are sent, and a form with no change just
closes. The room field moves the socket to the end of another room.
A change of type, group or number clears the "Last sent" label of the
socket, because what was sent before was meant for another address;
the form says so. The bin deletes the socket after asking, naming it.

Room heads and socket rows in edit mode also have an up and a down
button, which swap the item with its neighbour. The first item
cannot go up and the last one cannot go down, so a room with a
single socket has both buttons of that socket disabled. While one
change is on its way, every button of the edit mode is disabled, so
the page never sends two at once. When another phone changed the
order or removed the item in the meantime, the server refuses the
change. The page then loads the rooms again and says "The order was
changed on another device; the page was reloaded."

The page is plain HTML, CSS and JavaScript from the package, with no
build step and nothing loaded from another host. It needs a browser
from 2022 or later, such as Safari on iOS 15.4 and later.


## JSON API

### Conventions

Request and response bodies are JSON objects, in UTF-8. The
server makes every id, a short string that a client treats as
opaque. The order of an array is the order in which the rooms, and
the sockets of a room, are shown.

Every POST, PUT and PATCH must send the header
`Content-Type: application/json`, including the ones without a
body, such as All-off. A DELETE needs it only when it has a body.
A request without it is answered with 415. The header keeps the
forms of other web sites out: a browser sends the saved Basic
credentials along with such a form, but cannot declare it as JSON
without a CORS preflight, which the app does not answer.

A body is at most 16 KiB, else the answer is 413. A field that is
not listed for a route is an error, 422 with the field named. A
body that is not a JSON object is a 400.

Every route checks a request in the same order: 400 for a body that
is not a JSON object, then 404 for an id in the URL, then 422 for
the fields, then 409 for a conflict. So a bad field for an id that
does not exist is answered 404, and nothing is sent to rsswitchd.

### Objects

A room:

```
{"id": "r_4f3a9c1d22b0", "name": "Kitchen", "sockets": [socket]}
```

A socket:

```json
{"id": "s_0a1b2c3d4e5f", "room_id": "r_4f3a9c1d22b0",
 "name": "Lamp", "type": "pollin_2605", "group": 17,
 "number": 23,
 "last_sent": {"state": "on", "at": "2026-09-29T08:23:04.273Z"}}
```

```
 type    rev_008345 (group 0-3, number 0-2) or pollin_2605
         (group 0-31, number 0-31)
 group, number
         JSON integers within the limits of the type, never
         strings, floats or booleans
 name    1-64 characters after trimming, with no control
         characters and at least one character that is not a
         space or an invisible format character such as a
         zero-width space. Room names are unique in the
         household and socket names in their room, ignoring case
 last_sent
         null, or the last command confirmed by rsswitchd; see
         below
```

### Routes

- `GET /api/rooms`:
  All rooms, with their sockets: `{"rooms": [room, ...]}`.
- `GET /api/rooms/<id>`:
  One room: `{"room": room}`.
- `POST /api/rooms`:
  Body `{"name": ...}`. Answers 201 `{"room": room}`; the new room
  is last.
- `PATCH /api/rooms/<id>`:
  Body `{"name": ...}`, or `{}` to change nothing. Answers
  `{"room": room}`.
- `DELETE /api/rooms/<id>`:
  Answers 204 without a body. A room that still holds sockets
  is refused with room_not_empty.
- `PUT /api/rooms/order`:
  Body `{"ids": [room id, ...]}` with the id of every room
  once. Answers `{"ids": [...]}`.
- `POST /api/rooms/<id>/sockets`:
  Body `{"name", "type", "group", "number"}`. Answers 201
  `{"socket": socket, "warnings": [warning, ...]}`; the new
  socket is last in its room.
- `PUT /api/rooms/<id>/sockets/order`:
  Body `{"ids": [socket id, ...]}` with the id of every socket of
  the room once. Answers `{"ids": [...]}`.
- `GET /api/sockets/<id>`:
  One socket: `{"socket": socket}`.
- `PATCH /api/sockets/<id>`:
  Body with any of name, type, group, number and room_id; only
  the fields that are sent change. A room_id moves the socket to
  the end of that room. Answers `{"socket": socket, "warnings":
  [warning, ...]}`. The new address must be valid for the type
  that the socket has afterwards. A change of type, group or
  number sets last_sent to null.
- `DELETE /api/sockets/<id>`:
  Answers 204 without a body.
- `POST /api/sockets/<id>/switch`:
  Body `{"state": "on"}` or `{"state": "off"}`; see Switching.
- `POST /api/rooms/<id>/all-off`:
  Switches off every socket of a room; see All-off.
- `POST /api/all-off`:
  Switches off every socket of every room; see All-off.
- `GET /api/meta`:
  The limits of each socket type and the timing of a switch:
  `{"socket_types": {"rev_008345": {"groups": 4, "sockets": 3}, ...},
  "switch_timeout": 7, "all_off_gap_ms": 200}`. For each type it gives
  the number of groups and of sockets per group, so group runs from 0
  to groups - 1 and number from 0 to sockets - 1. switch_timeout is
  the longest that one command can take before the app answers, in
  seconds (daemon_connect_timeout plus daemon_read_timeout; exact
  because daemon_host is an IP address, so no name lookup adds to it), and
  all_off_gap_ms is the pause between two sends of an All-off. It is
  read-only, and the page checks a form against the limits before
  sending it and waits for a switch as long as the timing says.

A reorder list that is not exactly the current set of ids, because
another phone changed it in the meantime, is refused with
order_mismatch; reload and try again.

### Switching

`POST /api/sockets/<id>/switch` sends one command to rsswitchd and
answers after rsswitchd has replied:

```
{"ok": true, "recorded": true, "socket": socket}
```

recorded is false when the command was sent, but the socket was
deleted or given another address while it was on its way, or a
newer command was already confirmed, or the data file could not be
written. The command went out in every one of these cases, so the
answer is still a success. socket is the socket as it is now, or
null when it is gone or the data file cannot be read. When
rsswitchd does not confirm the command, the answer is one of the
502 or 504 errors below and last_sent does not change.

### All-off

All-off sends "off" to every socket of a room, or of all rooms. It
sends one command for each distinct address (type, group and
number), in display order, so sockets that share an address get one
command and the same outcome. It waits all_off_gap_ms between two
commands. When rsswitchd refuses a command or sends a reply that is
not understood, the next address is tried. When rsswitchd cannot be
reached or does not answer in time, the rest is skipped, so a dead
daemon costs one attempt and not one per socket.

The answer is 200 whatever the outcome; look at ok. A room that
does not exist is a 404.

```json
{"ok": false,
 "results": [
   {"socket_id": "s_0a1b2c3d4e5f", "room_id": "r_4f3a9c1d22b0",
    "status": "ok", "recorded": true,
    "last_sent": {"state": "off", "at": "2026-09-29T08:23:04.273Z"}},
   {"socket_id": "s_1a2b3c4d5e6f", "room_id": "r_4f3a9c1d22b0",
    "status": "error",
    "error": {"code": "daemon_error", "message": "...",
              "daemon_class": "range", "daemon_message": "..."}},
   {"socket_id": "s_2a3b4c5d6e7f", "room_id": "r_8e7d6c5b4a39",
    "status": "skipped",
    "error": {"code": "daemon_unreachable", "message": "..."}}],
 "summary": {"ok": 1, "error": 1, "skipped": 1}}
```

There is one entry per socket, in display order. status is ok,
error or skipped. An ok entry has recorded, which is false when the
socket was changed or deleted in the meantime or the data file
could not be written, and has last_sent only when recorded is
true. A data file that cannot be written does not stop the batch.
An error or skipped entry has error, in the form of the error
object below. ok is true when every entry is ok, which includes a
household without sockets.

### Warnings

A warning does not refuse anything. Creating or editing a socket
may answer with warnings, a list that is empty most of the time.
Two sockets with the same type, group and number switch the same
receivers, which is sometimes wanted:

```json
{"code": "duplicate_address",
 "message": "another socket uses the same type, group and number",
 "socket_ids": ["s_2a3b4c5d6e7f"]}
```

socket_ids names the other sockets.

### Errors

Every error has the same body:

```json
{"error": {"code": "validation", "message": "must not be empty",
           "field": "name"}}
```

field is there for errors about a field of the request. An error
that comes from rsswitchd has daemon_class and daemon_message as
well: the class word and the text of the ERR reply (see
[rsswitchd](../network/README.md)), reduced to printable ASCII and
at most 128 characters.

The code is fixed and a client may branch on it. The message may
change, like the text of an rsswitchd reply.

| Code | Status | Meaning |
|------|--------|---------|
| `bad_json` | 400 | body is empty, is not JSON, or is not a JSON object; only the body checks give it |
| `unauthorized` | 401 | login needed or wrong; the answer has a `WWW-Authenticate` header |
| `not_found` | 404 | no such URL, room or socket |
| `method_not_allowed` | 405 | the route does not take this method; `Allow` names the ones it does |
| `room_not_empty` | 409 | the room still holds sockets |
| `name_conflict` | 409 | the name is taken; field is name |
| `order_mismatch` | 409 | the list of ids is not the current set of ids |
| `too_many_attempts` | 429 | too many failed logins from this address; see `Retry-After` |
| `payload_too_large` | 413 | body above 16 KiB |
| `unsupported_media_type` | 415 | `Content-Type` is not `application/json` |
| `validation` | 422 | a field is missing, unknown or has a bad value; field names it |
| `daemon_unreachable` | 502 | no connection to rsswitchd |
| `daemon_error` | 502 | rsswitchd replied ERR; see daemon_class, daemon_message |
| `daemon_protocol` | 502 | rsswitchd sent a reply that was not understood, or hung up |
| `daemon_timeout` | 504 | rsswitchd did not answer in time |
| `store_error` | 500 | the data file cannot be used |
| `internal_error` | 500 | a bug in the app |
| `http_error` | other | any other HTTP error, also a 400 that is not about the body; the status is the one of the response |


### The last command

last_sent is the last command that rsswitchd confirmed with OK,
and the time of that confirmation in UTC. It is not the real state
of the socket. 433 MHz is one-way: nothing tells the app whether a
socket received the command, and a socket may have been switched
by its remote control or by hand since. null means that no command
was ever confirmed for the socket, or that its address changed. A
client should show it as "last sent on, 08:23", not as "is on".

A client must show names and the messages of rsswitchd as text,
never as HTML. Set textContent, not innerHTML.


## Security

Without password_hash, anyone who can reach listen_address can
switch every socket. The default of listen_address is 127.0.0.1, so
that only this host can. Set a password before you let the app listen
on an address that other hosts can reach: with no password_hash and
such an address, the app prints a warning on stderr at start-up, and
it is in the journal.

Basic authentication sends the username and the password in
cleartext with every request over plain HTTP. Use it only on a LAN
that you trust, or put the app behind something that provides
TLS. Such a proxy must be reachable by trusted clients only: the app
sees only its address, so the limit on failed logins (see
[Failed logins](#failed-logins)) counts all of its clients together.

Keep rsswitchd bound to 127.0.0.1 behind the web app (see
[rsswitchd](../network/README.md)), so that the app is the only way to
reach it.

Clients must display names and daemon messages as text, never as
HTML.

The page and the API send a Content-Security-Policy that allows only
the scripts, styles and requests of the app itself and forbids
framing the page in another site, together with nosniff, no-referrer
and X-Frame-Options DENY. Every API answer has Cache-Control
no-store, so a cache never keeps the rooms or the result of a
switch.

The app never needs root or GPIO access. Run it as an unprivileged
user; it only needs to read its code, to write its data file and
to connect to rsswitchd, which owns the GPIO lines.


## References

- [rsswitchd](../network/README.md)
