#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

"""The client of rsswitchd's line protocol (protocol v2).

Every command uses a connection of its own, which is closed as soon as
the reply has been read. rsswitchd serves one connection at a time, so
a connection that is held open would make every other client wait in
the accept queue, and a phone that taps a single socket would run into
its read timeout while a batch is in progress.

Nothing here waits without a limit. The connect has a timeout of its
own and the whole wait for the reply has one deadline, which is not
started again by each byte that arrives. There is no retry: a daemon
that is not there is reported at once.

The module does not import Flask.
"""

from __future__ import annotations

import re
import socket
import time

# Longest reply line that is accepted. rsswitchd's own lines are at
# most 4 + 1 + 5 + 1 + 64 bytes long.
MAX_REPLY = 256

# Longest daemon text that is passed on to a client.
MAX_MESSAGE = 128

_ERR = re.compile(r"ERR ([a-z]+)(?: (.*))?", re.DOTALL)


class DaemonError(Exception):
    """rsswitchd did not confirm a command.

    code and status are what the API answers: a fixed word for clients
    to branch on and an HTTP status.
    """

    code = "daemon_error"
    status = 502


class DaemonUnreachable(DaemonError):
    """No connection could be made: refused, no route or no such host."""

    code = "daemon_unreachable"


class DaemonTimeout(DaemonError):
    """The connect or the reply took longer than its limit."""

    code = "daemon_timeout"
    status = 504


class DaemonProtocolError(DaemonError):
    """The reply was not one of the lines rsswitchd sends.

    That includes a connection that ended or was reset before the
    reply, and a reply that is far too long.
    """

    code = "daemon_protocol"


class DaemonReplyError(DaemonError):
    """rsswitchd answered ERR <class> <text>.

    daemon_class is the fixed word after ERR. daemon_message is the
    text after it, reduced to printable ASCII, because the daemon's
    words are shown to a user.
    """

    code = "daemon_error"

    def __init__(self, daemon_class, daemon_message):
        super().__init__("%s %s" % (daemon_class, daemon_message))
        self.daemon_class = daemon_class
        self.daemon_message = daemon_message


def sanitize(text) -> str:
    """Keep the printable ASCII characters of text, at most MAX_MESSAGE."""
    kept = "".join(c for c in text if " " <= c <= "~")
    return kept[:MAX_MESSAGE]


def _read_line(sock, deadline) -> str:
    """Read one line from sock, giving up when the deadline has passed.

    The time that is left is worked out again before every recv(), so
    a peer that sends one byte at a time cannot stretch the wait.
    """
    buf = b""
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout("timed out")
        sock.settimeout(remaining)
        chunk = sock.recv(MAX_REPLY)
        if not chunk:
            raise DaemonProtocolError("connection closed before a reply")
        buf += chunk
        end = buf.find(b"\n")
        if end >= 0:
            line = buf[:end]
            break
        if len(buf) > MAX_REPLY:
            raise DaemonProtocolError("reply line too long")
    if len(line) > MAX_REPLY:
        raise DaemonProtocolError("reply line too long")
    if line.endswith(b"\r"):
        line = line[:-1]
    return line.decode("ascii", errors="replace")


def _check_argument(value, name, limit=None):
    if type(value) is not int or value < 0:
        raise ValueError("%s must be a non-negative integer" % name)
    if limit is not None and value > limit:
        raise ValueError("%s must be at most %d" % (name, limit))


class RsswitchdClient:
    """Sends commands to one rsswitchd. Holds no connection."""

    def __init__(self, host, port, connect_timeout, read_timeout):
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.read_timeout = read_timeout

    def send(self, dev, group, number, data) -> None:
        """Send one command and wait for the daemon's reply.

        Return when the daemon answered OK, which means the codeword
        was sent. Raise ValueError for an argument that is not a
        non-negative integer, or a data other than 0 and 1, before any
        connection is made. Raise a DaemonError subclass for every
        other outcome. The line is made from integers only, so no
        text of a request and no leading zero can reach the wire.
        """
        _check_argument(dev, "dev")
        _check_argument(group, "group")
        _check_argument(number, "number")
        _check_argument(data, "data", 1)
        line = ("%d %d %d %d\n" % (dev, group, number, data)).encode("ascii")
        try:
            sock = socket.create_connection((self.host, self.port),
                                            timeout=self.connect_timeout)
        except socket.timeout:
            # Before OSError: on Python 3.9 it is a subclass of it.
            raise DaemonTimeout("connect timed out")
        except OSError as e:
            raise DaemonUnreachable("cannot connect: %s" % e)
        with sock:
            deadline = time.monotonic() + self.read_timeout
            try:
                sock.settimeout(self.read_timeout)
                sock.sendall(line)
                reply = _read_line(sock, deadline)
            except socket.timeout:
                raise DaemonTimeout("no reply within %s s"
                                    % self.read_timeout)
            except OSError as e:
                raise DaemonProtocolError("connection failed: %s" % e)
        if reply == "OK":
            return
        m = _ERR.fullmatch(reply)
        if m:
            raise DaemonReplyError(m.group(1), sanitize(m.group(2) or ""))
        raise DaemonProtocolError("unexpected reply")
