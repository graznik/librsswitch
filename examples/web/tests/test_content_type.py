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

"""Tests of the JSON content type that state changes need."""

from __future__ import annotations

import pytest

SWITCH = "/api/sockets/s_000000000001/switch"
BODY = '{"state": "on"}'


def is_415(resp):
    assert resp.status_code == 415
    assert resp.mimetype == "application/json"
    error = resp.get_json()["error"]
    assert error["code"] == "unsupported_media_type"
    assert error["message"]


def test_switch_without_content_type_is_415_and_daemon_untouched(
        fake_daemon, switch_app):
    fake = fake_daemon()
    client = switch_app(fake).test_client()
    is_415(client.post(SWITCH, data=BODY))
    is_415(client.post(SWITCH))
    assert fake.connections == 0
    assert fake.lines == []


def test_switch_text_plain_is_415(fake_daemon, switch_app):
    fake = fake_daemon()
    client = switch_app(fake).test_client()
    is_415(client.post(SWITCH, data=BODY, content_type="text/plain"))
    is_415(client.post(SWITCH, data="state=on",
                       content_type="application/x-www-form-urlencoded"))
    assert fake.connections == 0
    assert fake.lines == []


@pytest.mark.parametrize("content_type", [
    "application/json",
    "application/json; charset=utf-8",
    "Application/JSON;charset=UTF-8",
])
def test_switch_json_with_charset_is_accepted(fake_daemon, switch_app,
                                              content_type):
    fake = fake_daemon()
    client = switch_app(fake).test_client()
    resp = client.post(SWITCH, data=BODY, content_type=content_type)
    assert resp.status_code == 200
    assert fake.lines == ["0 0 0 1"]


def test_json_content_type_with_empty_body_on_switch_is_400(fake_daemon,
                                                            switch_app):
    fake = fake_daemon()
    client = switch_app(fake).test_client()
    resp = client.post(SWITCH, content_type="application/json")
    assert resp.status_code == 400
    assert resp.get_json()["error"]["code"] == "bad_json"
    assert fake.connections == 0


@pytest.mark.parametrize("method", ["put", "patch"])
def test_put_and_patch_need_json(probe_app, method):
    call = getattr(probe_app.test_client(), method)
    is_415(call("/api/_probe", data="{}"))
    is_415(call("/api/_probe"))
    assert call("/api/_probe", data="{}",
                content_type="application/json").status_code == 200


def test_delete_without_body_passes_without_content_type(probe_app):
    resp = probe_app.test_client().delete("/api/_probe")
    assert resp.status_code == 200


def test_delete_with_body_needs_json(probe_app):
    client = probe_app.test_client()
    is_415(client.delete("/api/_probe", data='{"a": 1}'))
    resp = client.delete("/api/_probe", data='{"a": 1}',
                         content_type="application/json")
    assert resp.status_code == 200
    assert resp.get_json() == {"a": 1}


def test_get_is_never_gated(probe_app):
    client = probe_app.test_client()
    assert client.get("/api/rooms").status_code == 200
    assert client.head("/api/rooms").status_code == 200
    assert client.options("/api/rooms").status_code == 200
    assert client.get("/api/_probe").status_code == 200


def test_unknown_path_post_without_content_type_is_415(make_app):
    client = make_app().test_client()
    is_415(client.post("/api/nothing-here"))
    resp = client.post("/api/nothing-here", content_type="application/json")
    assert resp.status_code == 404
