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

"""Shared helpers and fixtures. Tests write only below tmp_path."""

from __future__ import annotations

import json
import socket
import struct
import threading

import pytest
from flask import abort, jsonify

from rsswitch_web import create_app
from rsswitch_web.errors import ApiError, json_body, reject_unknown_fields


def write_doc(path, doc):
    """Write doc to path the way the store writes it."""
    text = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
    with open(str(path), "wb") as f:
        f.write(text.encode("utf-8"))


def sample_doc():
    """Return a household with two rooms and two sockets."""
    return {
        "version": 1,
        "rooms": [
            {
                "id": "r_000000000001",
                "name": "Wohnzimmer",
                "sockets": [
                    {
                        "id": "s_000000000001",
                        "name": "Stehlampe",
                        "type": "rev_008345",
                        "group": 0,
                        "number": 0,
                        "last_sent": None,
                    },
                    {
                        "id": "s_000000000002",
                        "name": "Heizlüfter",
                        "type": "pollin_2605",
                        "group": 17,
                        "number": 23,
                        "last_sent": {"state": "on",
                                      "at": "2026-09-29T08:00:00.000Z"},
                    },
                ],
            },
            {
                "id": "r_000000000002",
                "name": "Küche",
                "sockets": [],
            },
        ],
    }


@pytest.fixture
def make_app(tmp_path):
    """Return a factory: make_app(doc=None, **overrides) -> Flask app.

    The data file is tmp_path/data.json. It is written when doc is
    given and left missing otherwise.
    """
    def factory(doc=None, **overrides):
        path = tmp_path / "data.json"
        if doc is not None:
            write_doc(path, doc)
        return create_app({"data_file": str(path), **overrides})

    return factory


@pytest.fixture
def probe_app(make_app):
    """A make_app() app with routes that only the tests use.

    They are registered before the first request.
    """
    app = make_app()

    @app.route("/api/_probe",
               methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
    def probe():
        return jsonify(json_body(required=False))

    @app.route("/api/_probe/strict", methods=["POST"])
    def probe_strict():
        body = json_body()
        reject_unknown_fields(body, {"name"})
        return jsonify(body)

    @app.route("/api/_probe/boom")
    def probe_boom():
        raise RuntimeError("secret failure in /srv/private/rsswitch.db")

    @app.route("/api/_probe/api-error")
    def probe_api_error():
        raise ApiError(409, "name_conflict", "taken", field="name",
                       headers={"X-Probe": "1"})

    @app.route("/api/_probe/abort/<int:code>")
    def probe_abort(code):
        abort(code)

    return app


# What a FakeDaemon behaviour may return besides bytes to send back.
HANG = object()   # never answer, until the daemon is closed
CLOSE = object()  # close the connection, which the peer sees as EOF
RESET = object()  # abort the connection, which the peer sees as a reset


class Drip:
    """Send data one byte per interval, and no newline unless data has one."""

    def __init__(self, data, interval):
        self.data = data
        self.interval = interval


class FakeDaemon:
    """A stand-in for rsswitchd on 127.0.0.1 and a port of its own.

    Every line that arrives is appended to lines, without its newline.
    behaviour(line) decides what happens next: it returns bytes to send
    back, None to send nothing, or one of HANG, CLOSE, RESET and Drip.
    It may run any code first, for example change the data file.
    connections counts the connections that were accepted.
    """

    def __init__(self, behaviour=None):
        self.behaviour = behaviour or (lambda line: b"OK\n")
        self.lines = []
        self.connections = 0
        self._stop = threading.Event()
        self._threads = []
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(16)
        self._sock.settimeout(0.05)
        self.port = self._sock.getsockname()[1]
        self._acceptor = threading.Thread(target=self._accept_loop,
                                          daemon=True)
        self._acceptor.start()

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections += 1
            t = threading.Thread(target=self._serve, args=(conn,),
                                 daemon=True)
            self._threads.append(t)
            t.start()

    def _serve(self, conn):
        conn.settimeout(0.05)
        buf = b""
        try:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(256)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("ascii", errors="replace")
                    self.lines.append(line)
                    if not self._act(conn, self.behaviour(line)):
                        return
        except OSError:
            pass
        finally:
            conn.close()

    def _act(self, conn, action):
        """Do what a behaviour asked for. False ends the connection."""
        if action is HANG:
            self._stop.wait()
            return False
        if action is CLOSE:
            return False
        if action is RESET:
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
            return False
        if isinstance(action, Drip):
            for i in range(len(action.data)):
                conn.sendall(action.data[i:i + 1])
                if self._stop.wait(action.interval):
                    break
            self._stop.wait()
            return False
        if action:
            conn.sendall(action)
        return True

    def close(self):
        """Release every HANG and Drip, close the socket, join the threads."""
        self._stop.set()
        self._sock.close()
        self._acceptor.join(5)
        for t in list(self._threads):
            t.join(5)


@pytest.fixture
def fake_daemon():
    """Return a factory: fake_daemon(behaviour=None) -> FakeDaemon.

    Every daemon it made is closed at teardown, so no thread of a test
    outlives it.
    """
    made = []

    def factory(behaviour=None):
        daemon = FakeDaemon(behaviour)
        made.append(daemon)
        return daemon

    yield factory
    for daemon in made:
        daemon.close()


def refused_port():
    """Return a port on 127.0.0.1 that nothing listens on."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port
