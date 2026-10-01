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
import os
import platform
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

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

    With serial=True it serves one connection at a time, as rsswitchd
    does: a connection that is accepted later waits until the one
    before it is closed.
    """

    def __init__(self, behaviour=None, serial=False):
        self.behaviour = behaviour or (lambda line: b"OK\n")
        self._serial = threading.Lock() if serial else None
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
        held = False
        try:
            while self._serial and not held:
                if self._stop.is_set():
                    return
                held = self._serial.acquire(timeout=0.05)
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
            if held:
                self._serial.release()

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
    """Return a factory: fake_daemon(behaviour=None, serial=False).

    Every daemon it made is closed at teardown, so no thread of a test
    outlives it.
    """
    made = []

    def factory(behaviour=None, serial=False):
        daemon = FakeDaemon(behaviour, serial)
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


@pytest.fixture
def switch_app(make_app):
    """Return a factory: switch_app(daemon, doc=None, **overrides) -> app.

    daemon is a FakeDaemon or a port number. The app talks to it with
    short timeouts, and its data file holds sample_doc() unless doc is
    given.
    """
    def factory(daemon, doc=None, **overrides):
        port = daemon if isinstance(daemon, int) else daemon.port
        config = {
            "daemon_port": port,
            "daemon_connect_timeout": 0.5,
            "daemon_read_timeout": 0.5,
        }
        config.update(overrides)
        return make_app(sample_doc() if doc is None else doc, **config)

    return factory


def post_json(app, path, body=None):
    """POST body as JSON to path, and return the response."""
    return app.test_client().post(
        path, data=json.dumps({} if body is None else body),
        headers={"Content-Type": "application/json"})


def json_request(client, method, url, body=None):
    """Send body as JSON with the given method, and return the response.

    The Content-Type is always application/json, because the app
    refuses a state change without it. Without a body, none is sent.
    """
    kwargs = {"headers": {"Content-Type": "application/json"}}
    if body is not None:
        kwargs["data"] = json.dumps(body)
    return client.open(url, method=method, **kwargs)


def data_bytes(app):
    """Return the bytes of the data file of app."""
    with open(app.config["RSSWITCH_WEB"]["data_file"], "rb") as f:
        return f.read()


def data_doc(app):
    """Return the data file of app as a document."""
    return json.loads(data_bytes(app).decode("utf-8"))


_LISTENING = "rsswitchd: listening on "


class RealDaemon:
    """A running rsswitchd. Its output is collected by reader threads.

    port is the port the kernel picked. stdout holds the lines the
    daemon printed there, which on the x86_64 build are the codewords.
    """

    def __init__(self, proc):
        self.proc = proc
        self.port = None
        self.stdout = []
        self.stderr = []
        self._ready = threading.Event()
        self._readers = [
            threading.Thread(target=self._read_stdout, daemon=True),
            threading.Thread(target=self._read_stderr, daemon=True),
        ]
        for t in self._readers:
            t.start()

    def _read_stdout(self):
        for line in self.proc.stdout:
            self.stdout.append(line.rstrip("\n"))

    def _read_stderr(self):
        # Read to the end, so the pipe never fills up.
        for line in self.proc.stderr:
            line = line.rstrip("\n")
            self.stderr.append(line)
            if self.port is None and line.startswith(_LISTENING):
                self.port = int(line.rsplit(":", 1)[1])
                self._ready.set()

    def wait_ready(self, timeout):
        """Wait for the listening line. Fail when the daemon exits first."""
        deadline = time.monotonic() + timeout
        while not self._ready.wait(0.02):
            if self.proc.poll() is not None:
                for t in self._readers:
                    t.join(1)
                pytest.fail("rsswitchd exited with %s: %s"
                            % (self.proc.returncode, self.stderr))
            if time.monotonic() > deadline:
                pytest.fail("rsswitchd did not report its port: %s"
                            % self.stderr)

    def wait_for_stdout(self, text, timeout):
        """Return True when a stdout line contains text within timeout."""
        deadline = time.monotonic() + timeout
        while True:
            if any(text in line for line in list(self.stdout)):
                return True
            if time.monotonic() > deadline:
                return False
            time.sleep(0.01)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for t in self._readers:
            t.join(2)
        self.proc.stdout.close()
        self.proc.stderr.close()


@pytest.fixture
def real_daemon():
    """The x86_64 build of rsswitchd, started on a port of its own.

    The binary is the one named in RSSWITCHD_BIN. The caller vouches
    that it is a build for the x86_64 stub board, as tests/Makefile
    makes one on any host. Without RSSWITCHD_BIN it is
    examples/network/rsswitchd, which only an x86_64 host runs: a build
    for a board must never be started by a test.

    The test is skipped when the binary is missing or cannot run. With
    RSSWITCH_REQUIRE_DAEMON=1 it fails instead, so that a test run that
    is meant to cover the daemon cannot quietly leave it out.
    """
    give_up = (pytest.fail if os.environ.get("RSSWITCH_REQUIRE_DAEMON") == "1"
               else pytest.skip)
    hint = ("rsswitchd not available: name an x86_64 build in "
            "RSSWITCHD_BIN, or run make -C ../network x86_64")
    named = os.environ.get("RSSWITCHD_BIN")
    if named:
        binary = Path(named)
    else:
        binary = Path(__file__).resolve().parents[2] / "network" / "rsswitchd"
        if platform.machine() != "x86_64":
            give_up(hint)
    if not (binary.is_file() and os.access(str(binary), os.X_OK)):
        give_up("%s: %s" % (binary, hint))
    try:
        proc = subprocess.Popen(
            [str(binary), "--bind", "127.0.0.1", "--port", "0"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
    except OSError as e:
        give_up("%s cannot be started (%s): %s" % (binary, e, hint))
    daemon = RealDaemon(proc)
    try:
        daemon.wait_ready(5)
        yield daemon
    finally:
        daemon.stop()


WEB_DIR = Path(__file__).resolve().parents[1]
LISTENING = "rsswitch-web: listening on "
DEV_WARNING = "rsswitch-web: development server only, not for production use"


class Child:
    """A python -m rsswitch_web process whose stderr is read to the end.

    Werkzeug logs one line per request on stderr, and a pipe that nobody
    empties would stop the server once it is full.
    """

    def __init__(self, *args):
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "rsswitch_web"] + list(args),
            cwd=str(WEB_DIR), stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.lines = []
        self.port = None
        self._ready = threading.Event()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        for line in self.proc.stderr:
            line = line.rstrip("\n")
            self.lines.append(line)
            if self.port is None and line.startswith(LISTENING):
                self.port = int(line.rsplit(":", 1)[1])
                self._ready.set()

    def wait_ready(self, timeout=10):
        """Wait for the listening line. Fail when the child exits first."""
        deadline = time.monotonic() + timeout
        while not self._ready.wait(0.02):
            if self.proc.poll() is not None:
                self._reader.join(2)
                pytest.fail("exited with %s: %s"
                            % (self.proc.returncode, self.lines))
            if time.monotonic() > deadline:
                pytest.fail("no listening line: %s" % self.lines)

    def stop(self):
        """Terminate the child if it still runs, and return its status."""
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            status = self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            status = self.proc.wait()
        self._reader.join(2)
        self.proc.stderr.close()
        return status


def run_cli(*args):
    """Run the command line to its end and return (status, stderr)."""
    proc = subprocess.run(
        [sys.executable, "-m", "rsswitch_web"] + list(args),
        cwd=str(WEB_DIR), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=30)
    return proc.returncode, proc.stderr


def get_json(port, path):
    """GET path from 127.0.0.1:port, without any proxy."""
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open("http://127.0.0.1:%d%s" % (port, path), timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def write_config(tmp_path, **overrides):
    """Write a config file for a data file below tmp_path, and return it."""
    data = tmp_path / "data.json"
    if not data.exists():
        write_doc(data, sample_doc())
    cfg = {
        "data_file": str(data),
        "listen_address": "127.0.0.1",
        "listen_port": 0,
        "daemon_port": refused_port(),
    }
    cfg.update(overrides)
    path = tmp_path / "config.json"
    path.write_text(json.dumps(cfg))
    return str(path)
