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

"""Tests of the serve command: the app under waitress."""

from __future__ import annotations

import http.client
import json
import signal
import socket
import subprocess
import sys

import pytest
import waitress

from rsswitch_web.__main__ import MAX_REQUEST_BODY, main

from conftest import (DEV_WARNING, LISTENING, WEB_DIR, Child, get_json,
                      run_cli, write_config)


def request(port, method, path, body=None):
    """Send a request to 127.0.0.1:port, return (status, response, data)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        headers = {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        return resp.status, resp, resp.read()
    finally:
        conn.close()


def test_serve_answers_the_api_and_the_page(tmp_path):
    child = Child("--config", write_config(tmp_path), "serve")
    try:
        child.wait_ready()
        # The first line is the readiness line: waitress has nothing to
        # warn about, and there is no development-server line.
        assert child.lines[0] == LISTENING + "127.0.0.1:%d" % child.port
        assert DEV_WARNING not in child.lines
        assert child.port > 0
        tree = get_json(child.port, "/api/rooms")
        assert [r["name"] for r in tree["rooms"]] == ["Wohnzimmer", "Küche"]
        status, resp, data = request(child.port, "GET", "/")
        assert status == 200
        assert resp.getheader("Content-Type").startswith("text/html")
        assert resp.getheader("Content-Security-Policy")
        assert b"<html" in data.lower()
        child.proc.send_signal(signal.SIGTERM)
        assert child.proc.wait(5) == 0
        assert DEV_WARNING not in child.lines
    finally:
        child.stop()


def test_serve_stops_on_sigint_with_status_0(tmp_path):
    child = Child("--config", write_config(tmp_path), "serve")
    try:
        child.wait_ready()
        child.proc.send_signal(signal.SIGINT)
        assert child.proc.wait(5) == 0
    finally:
        child.stop()


def test_serve_corrupt_data_file_exits_1(tmp_path):
    data = tmp_path / "data.json"
    data.write_text("{")
    status, err = run_cli("--config", write_config(tmp_path), "serve")
    assert status == 1
    assert err.startswith("rsswitch-web: data file ")
    assert str(data) in err
    assert LISTENING not in err
    assert data.read_text() == "{"


def test_serve_port_in_use_exits_1(tmp_path):
    taken = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        taken.bind(("127.0.0.1", 0))
        taken.listen(1)
        port = taken.getsockname()[1]
        status, err = run_cli(
            "--config", write_config(tmp_path, listen_port=port), "serve")
    finally:
        taken.close()
    assert status == 1
    assert err.startswith("rsswitch-web: cannot listen on 127.0.0.1:%d: "
                          % port)
    assert LISTENING not in err


def test_serve_typo_key_exits_2(tmp_path):
    path = tmp_path / "config.json"
    path.write_text(json.dumps({"thread": 4}))
    status, err = run_cli("--config", str(path), "serve")
    assert status == 2
    assert err == ("rsswitch-web: config: unknown key 'thread' "
                   "(did you mean 'threads'?)\n")


def test_serve_missing_explicit_config_exits_2(tmp_path):
    missing = str(tmp_path / "nope.json")
    status, err = run_cli("--config", missing, "serve")
    assert status == 2
    assert err.startswith("rsswitch-web: config: cannot read %s: " % missing)
    assert err.count("\n") == 1


def test_serve_refuses_threads_out_of_range_with_status_2(tmp_path):
    status, err = run_cli(
        "--config", write_config(tmp_path, threads=17), "serve")
    assert status == 2
    assert err == ("rsswitch-web: config: key 'threads': must be an "
                   "integer from 1 to 16\n")


def test_serve_passes_a_body_up_to_64_kib_to_the_app(tmp_path):
    child = Child("--config", write_config(tmp_path), "serve")
    try:
        child.wait_ready()
        # Over the limit of the app (16 KiB), under the one of waitress:
        # the app answers in JSON.
        body = json.dumps({"name": "x" * (20 * 1024)})
        status, resp, data = request(child.port, "POST", "/api/rooms", body)
        assert status == 413
        assert resp.getheader("Content-Type").startswith("application/json")
        assert json.loads(data.decode("utf-8"))["error"]["code"] \
            == "payload_too_large"
        # Over the limit of waitress: it refuses at the header, so the
        # body is not even sent.
        sock = socket.create_connection(("127.0.0.1", child.port), timeout=5)
        try:
            sock.sendall(("POST /api/rooms HTTP/1.1\r\nHost: x\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %d\r\n\r\n"
                          % (70 * 1024)).encode("ascii"))
            first = sock.recv(4096).split(b"\r\n", 1)[0]
        finally:
            sock.close()
        assert first.startswith(b"HTTP/1.") and b" 413 " in first
        child.proc.send_signal(signal.SIGTERM)
        assert child.proc.wait(5) == 0
    finally:
        child.stop()


class FakeServer:
    """What waitress.create_server returns, recording what happens."""

    def __init__(self, app, **kwargs):
        self.app = app
        self.kwargs = kwargs
        self.calls = []
        self.listening = None

    def run(self):
        self.calls.append("run")
        sock = self.kwargs["sockets"][0]
        self.listening = sock.getsockopt(socket.SOL_SOCKET,
                                         socket.SO_ACCEPTCONN)

    def close(self):
        self.calls.append("close")


@pytest.fixture
def fake_waitress(monkeypatch):
    """Replace waitress.create_server, and restore SIGTERM afterwards."""
    made = []

    def create_server(app, **kwargs):
        made.append(FakeServer(app, **kwargs))
        return made[-1]

    monkeypatch.setattr(waitress, "create_server", create_server)
    old = signal.getsignal(signal.SIGTERM)
    yield made
    signal.signal(signal.SIGTERM, old)
    for server in made:
        for sock in server.kwargs.get("sockets", []):
            sock.close()


def test_serve_hands_the_bound_socket_and_threads_to_waitress(
        tmp_path, fake_waitress, capsys):
    config = write_config(tmp_path, threads=3)
    assert main(["--config", config, "serve"]) == 0
    assert len(fake_waitress) == 1
    server = fake_waitress[0]
    assert server.app.config["RSSWITCH_WEB"]["threads"] == 3
    assert sorted(server.kwargs) == ["max_request_body_size", "sockets",
                                     "threads"]
    assert server.kwargs["threads"] == 3
    assert server.kwargs["max_request_body_size"] == 65536
    assert MAX_REQUEST_BODY == 65536
    sockets = server.kwargs["sockets"]
    assert len(sockets) == 1
    host, port = sockets[0].getsockname()[:2]
    assert host == "127.0.0.1" and port > 0
    # The socket listened when the server ran, and run() ended with a
    # close().
    assert server.listening
    assert server.calls == ["run", "close"]
    assert capsys.readouterr().err == LISTENING + "127.0.0.1:%d\n" % port


def test_serve_uses_4_threads_by_default(tmp_path, fake_waitress):
    assert main(["--config", write_config(tmp_path), "serve"]) == 0
    assert fake_waitress[0].kwargs["threads"] == 4


def test_serve_stops_when_waitress_sees_a_keyboard_interrupt(
        tmp_path, fake_waitress, monkeypatch, capsys):
    def interrupted(self):
        raise KeyboardInterrupt

    monkeypatch.setattr(FakeServer, "run", interrupted)
    assert main(["--config", write_config(tmp_path), "serve"]) == 0
    assert fake_waitress[0].calls == ["close"]


def test_serve_without_waitress_exits_1(tmp_path, monkeypatch, capsys):
    monkeypatch.setitem(sys.modules, "waitress", None)
    assert main(["--config", write_config(tmp_path), "serve"]) == 1
    err = capsys.readouterr().err
    assert err.startswith("rsswitch-web: ")
    assert "requirements.txt" in err
    assert err.count("\n") == 1
    assert LISTENING not in err


def test_importing_the_command_line_does_not_import_waitress():
    # waitress is imported inside serve only, so that run and
    # hash-password work in a venv that does not have it.
    proc = subprocess.run(
        [sys.executable, "-c", "import sys, rsswitch_web.__main__; "
         "print('waitress' in sys.modules)"],
        cwd=str(WEB_DIR), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=30)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == "False\n"


def test_help_lists_serve(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["--help"])
    assert exc.value.code == 0
    out = capsys.readouterr().out
    assert "serve" in out
    assert "serve the app with waitress, for production" in out
    assert "serve the app with the development server" in out
