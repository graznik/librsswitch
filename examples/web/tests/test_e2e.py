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

"""End-to-end test: the app under waitress drives the real rsswitchd.

Two programs run as processes of their own on ephemeral ports of
127.0.0.1: the x86_64 build of rsswitchd, which prints every codeword
it sends, and `python -m rsswitch_web serve`. The tests talk to the app
over real HTTP, so they prove that a request reaches the daemon and
makes it send the right codeword. They prove the codewords of the
stub board, not the radio signal.

Like every test that uses real_daemon, they are skipped when no
rsswitchd is available, unless RSSWITCH_REQUIRE_DAEMON is 1.
"""

from __future__ import annotations

import base64
import json
import urllib.error
import urllib.request

import pytest
from werkzeug.security import generate_password_hash

from conftest import Child, refused_port

POLLIN_ON = "0FFF0000F00F"
POLLIN_OFF = "0FFF0000F0F0"
REV_3_0_OFF = "FFF11FF00001"


class Http:
    """A client of the app on 127.0.0.1:port that ignores any proxy."""

    def __init__(self, port, auth=None):
        self.port = port
        self.auth = auth
        self._opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}))

    def call(self, method, path, body=None):
        """Send a request, return (status, headers, data).

        data is the parsed JSON of an answer that is JSON, and the text
        of any other answer. A status of 400 or more is returned, not
        raised.
        """
        headers = {}
        payload = None
        if method in ("POST", "PUT", "PATCH"):
            headers["Content-Type"] = "application/json"
            payload = json.dumps({} if body is None else body).encode()
        if self.auth is not None:
            token = base64.b64encode(self.auth.encode("utf-8"))
            headers["Authorization"] = "Basic " + token.decode("ascii")
        req = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (self.port, path), data=payload,
            headers=headers, method=method)
        try:
            with self._opener.open(req, timeout=10) as resp:
                status, got, raw = resp.status, resp.headers, resp.read()
        except urllib.error.HTTPError as e:
            status, got, raw = e.code, e.headers, e.read()
            e.close()
        text = raw.decode("utf-8")
        if got.get_content_type() == "application/json":
            return status, got, json.loads(text)
        return status, got, text


@pytest.fixture
def stack(tmp_path, real_daemon):
    """Return a factory: stack(**overrides) -> Http for a running app.

    The app is started with a config file below tmp_path, an empty
    household, the port of real_daemon and a short All-off gap. Every
    app it started is stopped at teardown, also when the test failed;
    real_daemon stops the daemon in its own finalizer.
    """
    children = []

    def start(auth=None, **overrides):
        config = {
            "data_file": str(tmp_path / "data.json"),
            "listen_address": "127.0.0.1",
            "listen_port": 0,
            "daemon_host": "127.0.0.1",
            "daemon_port": real_daemon.port,
            "all_off_gap_ms": 10,
            "threads": 2,
        }
        config.update(overrides)
        path = tmp_path / ("config%d.json" % len(children))
        path.write_text(json.dumps(config))
        child = Child("--config", str(path), "serve")
        children.append(child)
        child.wait_ready(10)
        return Http(child.port, auth), child

    yield start
    for child in children:
        child.stop()


def make_room(http, name):
    status, _, data = http.call("POST", "/api/rooms", {"name": name})
    assert status == 201, data
    return data["room"]["id"]


def make_socket(http, rid, name, kind, group, number):
    status, _, data = http.call(
        "POST", "/api/rooms/%s/sockets" % rid,
        {"name": name, "type": kind, "group": group, "number": number})
    assert status == 201, data
    assert data["warnings"] == []
    return data["socket"]["id"]


def codewords(daemon, before, count):
    """Return the lines the daemon printed after the first `before`.

    Wait until `count` of them have arrived, so that the answer of the
    app, which comes first, is not compared with a half-read pipe.
    """
    assert daemon.wait_for_lines(before + count, 5), daemon.stdout
    return daemon.stdout[before:]


def test_e2e_switches_pollin_17_23_over_http(stack, real_daemon):
    http, child = stack()
    status, headers, _ = http.call("GET", "/")
    assert status == 200
    assert headers.get_content_type() == "text/html"
    assert headers["Content-Security-Policy"]

    rid = make_room(http, "Living room")
    sid = make_socket(http, rid, "Heater", "pollin_2605", 17, 23)

    before = len(real_daemon.stdout)
    status, _, data = http.call("POST", "/api/sockets/%s/switch" % sid,
                                {"state": "on"})
    assert status == 200, data
    assert data["ok"] is True and data["recorded"] is True
    assert data["socket"]["last_sent"]["state"] == "on"
    status, _, data = http.call("POST", "/api/sockets/%s/switch" % sid,
                                {"state": "off"})
    assert status == 200, data
    assert data["ok"] is True and data["recorded"] is True
    assert data["socket"]["last_sent"]["state"] == "off"
    assert codewords(real_daemon, before, 2) == [POLLIN_ON, POLLIN_OFF]

    status, _, tree = http.call("GET", "/api/rooms")
    assert status == 200
    last_sent = tree["rooms"][0]["sockets"][0]["last_sent"]
    assert last_sent["state"] == "off"
    assert last_sent["at"].endswith("Z")
    assert all("development server" not in line for line in child.lines)


def test_e2e_all_off_sends_every_address(stack, real_daemon):
    http, _ = stack()
    rid = make_room(http, "Living room")
    make_socket(http, rid, "Heater", "pollin_2605", 17, 23)
    make_socket(http, rid, "Lamp", "rev_008345", 3, 0)

    before = len(real_daemon.stdout)
    status, _, report = http.call("POST", "/api/rooms/%s/all-off" % rid, {})
    assert status == 200, report
    assert report["ok"] is True
    assert report["summary"] == {"ok": 2, "error": 0, "skipped": 0}
    assert codewords(real_daemon, before, 2) == [POLLIN_OFF, REV_3_0_OFF]

    before = len(real_daemon.stdout)
    status, _, report = http.call("POST", "/api/all-off", {})
    assert status == 200, report
    assert report["summary"] == {"ok": 2, "error": 0, "skipped": 0}
    assert codewords(real_daemon, before, 2) == [POLLIN_OFF, REV_3_0_OFF]


def test_e2e_basic_auth_and_lockout(stack, real_daemon):
    hashed = generate_password_hash("secret", "pbkdf2:sha256:1000")
    anonymous, _ = stack(password_hash=hashed)
    for path in ("/", "/api/rooms"):
        status, headers, _ = anonymous.call("GET", path)
        assert status == 401
        assert headers["WWW-Authenticate"].startswith(
            'Basic realm="rsswitch"')

    http = Http(anonymous.port, "admin:secret")
    assert http.call("GET", "/")[0] == 200
    assert http.call("GET", "/api/rooms")[0] == 200
    rid = make_room(http, "Living room")
    sid = make_socket(http, rid, "Heater", "pollin_2605", 17, 23)
    before = len(real_daemon.stdout)
    status, _, data = http.call("POST", "/api/sockets/%s/switch" % sid,
                                {"state": "on"})
    assert status == 200, data
    assert codewords(real_daemon, before, 1) == [POLLIN_ON]

    wrong = Http(anonymous.port, "admin:wrong")
    for _ in range(5):
        assert wrong.call("GET", "/api/rooms")[0] == 401
    status, headers, data = http.call("GET", "/api/rooms")
    assert status == 429
    assert data["error"]["code"] == "too_many_attempts"
    # The header is the whole seconds left of the lockout, computed when
    # the request arrives, so a run that crosses a second boundary sees 59.
    assert 55 <= int(headers["Retry-After"]) <= 60


def test_e2e_stopped_daemon_is_reported(stack):
    http, _ = stack(daemon_port=refused_port())
    rid = make_room(http, "Living room")
    sid = make_socket(http, rid, "Heater", "pollin_2605", 17, 23)
    status, _, data = http.call("POST", "/api/sockets/%s/switch" % sid,
                                {"state": "on"})
    assert status == 502
    assert data["error"]["code"] == "daemon_unreachable"
    status, _, tree = http.call("GET", "/api/rooms")
    assert status == 200
    assert tree["rooms"][0]["sockets"][0]["last_sent"] is None
