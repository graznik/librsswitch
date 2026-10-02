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

"""Tests of GET /api/meta, the address limits of the socket types."""

from __future__ import annotations

import base64

import pytest
from conftest import sample_doc
from werkzeug.security import generate_password_hash

from rsswitch_web import model
from rsswitch_web.config import DEFAULTS, MAX_DAEMON_TIMEOUT
from rsswitch_web.model import SOCKET_TYPES, SocketType, address_problem

# A quick hash, because the test that logs in pays for it.
HASH = generate_password_hash("secret", "pbkdf2:sha256:1000")


def table():
    return {
        name: {"groups": t.groups, "sockets": t.sockets}
        for name, t in SOCKET_TYPES.items()}


@pytest.fixture
def client(make_app):
    return make_app(sample_doc()).test_client()


def test_meta_equals_socket_types(client):
    resp = client.get("/api/meta")
    assert resp.status_code == 200
    assert resp.mimetype == "application/json"
    body = resp.get_json()
    assert body["socket_types"] == table()
    assert list(body["socket_types"]) == list(SOCKET_TYPES)
    assert body["socket_types"] == {
        "rev_008345": {"groups": 4, "sockets": 3},
        "pollin_2605": {"groups": 32, "sockets": 32}}


def test_meta_has_the_timing_of_the_default_configuration(client):
    body = client.get("/api/meta").get_json()
    assert set(body) == {"socket_types", "switch_timeout", "all_off_gap_ms"}
    assert body["switch_timeout"] == (DEFAULTS["daemon_connect_timeout"]
                                      + DEFAULTS["daemon_read_timeout"])
    assert body["all_off_gap_ms"] == DEFAULTS["all_off_gap_ms"]


def test_meta_timing_follows_the_configuration(make_app):
    app = make_app(sample_doc(), daemon_connect_timeout=1.5,
                   daemon_read_timeout=40, all_off_gap_ms=750)
    body = app.test_client().get("/api/meta").get_json()
    assert body["switch_timeout"] == 41.5
    assert body["all_off_gap_ms"] == 750


def test_meta_timeout_covers_the_longest_a_configuration_allows(make_app):
    # The page waits this long for a switch, so it must not be less than
    # what the app can take: a connect and a read that both time out.
    app = make_app(sample_doc(), daemon_connect_timeout=MAX_DAEMON_TIMEOUT,
                   daemon_read_timeout=MAX_DAEMON_TIMEOUT)
    body = app.test_client().get("/api/meta").get_json()
    assert body["switch_timeout"] == 2 * MAX_DAEMON_TIMEOUT


def test_meta_follows_the_table(client, monkeypatch):
    monkeypatch.setitem(model.SOCKET_TYPES, "test_type",
                        SocketType(9, 2, 5))
    body = client.get("/api/meta").get_json()
    assert body["socket_types"]["test_type"] == {"groups": 2, "sockets": 5}
    assert list(body["socket_types"])[-1] == "test_type"


@pytest.mark.parametrize("type_name", sorted(SOCKET_TYPES))
def test_meta_limits_agree_with_validation(client, type_name):
    limits = client.get("/api/meta").get_json()["socket_types"][type_name]
    groups, sockets = limits["groups"], limits["sockets"]
    assert address_problem(type_name, groups - 1, sockets - 1) is None
    assert address_problem(type_name, groups, 0)[0] == "group"
    assert address_problem(type_name, 0, sockets)[0] == "number"


@pytest.mark.parametrize("method", ["POST", "PUT", "PATCH", "DELETE"])
def test_meta_is_read_only(client, method):
    resp = client.open("/api/meta", method=method,
                       headers={"Content-Type": "application/json"},
                       data="{}")
    assert resp.status_code == 405
    assert resp.get_json()["error"]["code"] == "method_not_allowed"
    assert "GET" in resp.headers["Allow"]


def test_meta_needs_login_when_a_password_is_set(make_app):
    client = make_app(sample_doc(), password_hash=HASH).test_client()
    assert client.get("/api/meta").status_code == 401
    raw = base64.b64encode(b"admin:secret").decode("ascii")
    resp = client.get("/api/meta",
                      headers={"Authorization": "Basic " + raw})
    assert resp.status_code == 200
    assert resp.get_json()["socket_types"] == table()
