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

"""Tests of the JSON error contract."""

from __future__ import annotations

import json
import logging
import shutil

import pytest
from conftest import sample_doc, write_doc

from rsswitch_web import create_app
from rsswitch_web.errors import HTTP_CODES, MAX_BODY_BYTES

JSON = {"Content-Type": "application/json"}


def error_of(resp, status, code):
    """Check status and code, and return the error object."""
    assert resp.status_code == status
    assert resp.mimetype == "application/json"
    body = resp.get_json()
    assert list(body) == ["error"]
    assert body["error"]["code"] == code
    assert isinstance(body["error"]["message"], str)
    assert body["error"]["message"]
    return body["error"]


def test_unknown_route_is_json_404(probe_app):
    resp = probe_app.test_client().get("/api/nothing-here")
    error_of(resp, 404, "not_found")
    resp = probe_app.test_client().get("/")
    error_of(resp, 404, "not_found")


def test_wrong_method_is_json_405_with_allow(probe_app):
    resp = probe_app.test_client().delete("/api/rooms")
    error_of(resp, 405, "method_not_allowed")
    assert "GET" in resp.headers["Allow"]
    allowed = resp.headers["Allow"].split(", ")
    assert allowed == sorted(allowed)


def test_body_over_16_kib_is_413(probe_app):
    client = probe_app.test_client()
    assert MAX_BODY_BYTES == 16 * 1024
    head = b'{"a": "'
    tail = b'"}'
    exact = head + b"x" * (MAX_BODY_BYTES - len(head) - len(tail)) + tail
    assert len(exact) == MAX_BODY_BYTES
    resp = client.post("/api/_probe", data=exact, headers=JSON)
    assert resp.status_code == 200
    resp = client.post("/api/_probe", data=exact + b" ", headers=JSON)
    error_of(resp, 413, "payload_too_large")
    resp = client.post("/api/_probe", data=b"x" * 100000, headers=JSON)
    error_of(resp, 413, "payload_too_large")


@pytest.mark.parametrize("data", [
    b"{",
    b'{"name": "a",}',
    b"\xff\xfe",
    b'{"name": NaN}',
    b'{"name": Infinity}',
    b"[]",
    b'[{"name": "a"}]',
    b'"text"',
    b"null",
    b"1",
    b"[" * 5000,
    b"",
    b"  \n",
], ids=["truncated", "trailing-comma", "not-utf8", "nan", "infinity",
        "list", "list-of-object", "string", "null", "number",
        "nesting-too-deep", "empty", "blank"])
def test_bad_json_cases(probe_app, data):
    resp = probe_app.test_client().post("/api/_probe/strict", data=data,
                                        headers=JSON)
    error_of(resp, 400, "bad_json")


def test_valid_object_passes_and_empty_body_is_optional(probe_app):
    client = probe_app.test_client()
    resp = client.post("/api/_probe/strict", data=b'{"name": "K\xc3\xbcche"}',
                       headers=JSON)
    assert resp.status_code == 200
    assert resp.get_json() == {"name": "Küche"}
    for data in (b"", b"  "):
        resp = client.post("/api/_probe", data=data, headers=JSON)
        assert resp.status_code == 200
        assert resp.get_json() == {}


def test_unknown_field_is_422_with_sorted_first_field(probe_app):
    body = {"zeta": 1, "beta": 2, "name": "x", "alpha": 3}
    resp = probe_app.test_client().post("/api/_probe/strict",
                                        data=json.dumps(body), headers=JSON)
    error = error_of(resp, 422, "validation")
    assert error["field"] == "alpha"


def test_api_error_passes_status_code_field_and_headers(probe_app):
    resp = probe_app.test_client().get("/api/_probe/api-error")
    error = error_of(resp, 409, "name_conflict")
    assert error == {"code": "name_conflict", "message": "taken",
                     "field": "name"}
    assert resp.headers["X-Probe"] == "1"


def test_abort_400_is_bad_json(probe_app):
    resp = probe_app.test_client().get("/api/_probe/abort/400")
    error_of(resp, 400, "bad_json")


@pytest.mark.parametrize("status, code", sorted(HTTP_CODES.items()))
def test_every_known_status_has_its_code(probe_app, status, code):
    resp = probe_app.test_client().get("/api/_probe/abort/%d" % status)
    error_of(resp, status, code)


def test_other_http_error_keeps_status_as_http_error(probe_app):
    resp = probe_app.test_client().get("/api/_probe/abort/416")
    error_of(resp, 416, "http_error")
    resp = probe_app.test_client().get("/api/_probe/abort/409")
    error_of(resp, 409, "http_error")


def test_unexpected_exception_is_500_without_details(probe_app, caplog):
    with caplog.at_level(logging.ERROR):
        resp = probe_app.test_client().get("/api/_probe/boom")
    error_of(resp, 500, "internal_error")
    assert b"secret failure" not in resp.data
    assert b"/srv/private" not in resp.data
    assert b"Traceback" not in resp.data
    assert "secret failure in /srv/private/rsswitch.db" in caplog.text


def test_abort_500_is_internal_error(probe_app):
    resp = probe_app.test_client().get("/api/_probe/abort/500")
    error_of(resp, 500, "internal_error")


def test_store_error_at_runtime_is_500_store_error_and_file_untouched(
        make_app, tmp_path, caplog):
    app = make_app(sample_doc())
    client = app.test_client()
    assert client.get("/api/rooms").status_code == 200

    path = tmp_path / "data.json"
    path.write_bytes(b"{")
    with caplog.at_level(logging.ERROR):
        resp = client.get("/api/rooms")
    error_of(resp, 500, "store_error")
    assert str(tmp_path).encode("utf-8") not in resp.data
    assert b"data.json" not in resp.data
    assert path.read_bytes() == b"{"
    assert str(path) in caplog.text


def test_vanished_data_dir_is_500_store_error_not_an_empty_list(tmp_path,
                                                                caplog):
    data_dir = tmp_path / "volume"
    data_dir.mkdir()
    write_doc(data_dir / "data.json", sample_doc())
    app = create_app({"data_file": str(data_dir / "data.json")})
    client = app.test_client()
    assert len(client.get("/api/rooms").get_json()["rooms"]) == 2

    shutil.rmtree(str(data_dir))
    with caplog.at_level(logging.ERROR):
        resp = client.get("/api/rooms")
    error_of(resp, 500, "store_error")
    assert str(tmp_path).encode("utf-8") not in resp.data
    assert str(data_dir) in caplog.text
    assert not data_dir.exists()
