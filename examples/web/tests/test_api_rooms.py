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

"""Tests of the room routes, from the data file to JSON over HTTP."""

from __future__ import annotations

import os
import re
import threading

import pytest
from conftest import (data_bytes, data_doc, json_request, sample_doc,
                      write_doc)

from rsswitch_web import create_app, model

KITCHEN = "r_000000000002"
LIVING = "r_000000000001"
PRECOMPOSED = "K\u00fcche"
COMBINING = "Ku\u0308che"

SOCKET_KEYS = ["id", "room_id", "name", "type", "group", "number",
               "last_sent"]


def test_get_rooms_serves_the_data_file_in_display_order(make_app):
    app = make_app(sample_doc())
    resp = app.test_client().get("/api/rooms")
    assert resp.status_code == 200
    assert resp.mimetype == "application/json"
    assert resp.get_json() == {
        "rooms": [
            {
                "id": "r_000000000001",
                "name": "Wohnzimmer",
                "sockets": [
                    {
                        "id": "s_000000000001",
                        "room_id": "r_000000000001",
                        "name": "Stehlampe",
                        "type": "rev_008345",
                        "group": 0,
                        "number": 0,
                        "last_sent": None,
                    },
                    {
                        "id": "s_000000000002",
                        "room_id": "r_000000000001",
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
    first = resp.get_json()["rooms"][0]
    assert list(first) == ["id", "name", "sockets"]
    assert list(first["sockets"][0]) == SOCKET_KEYS


def test_get_rooms_on_missing_file_is_empty_and_creates_nothing(
        make_app, tmp_path):
    app = make_app()
    resp = app.test_client().get("/api/rooms")
    assert resp.status_code == 200
    assert resp.get_json() == {"rooms": []}
    assert os.listdir(str(tmp_path)) == []


def test_get_rooms_keeps_non_ascii_names_unescaped(make_app):
    app = make_app(sample_doc())
    resp = app.test_client().get("/api/rooms")
    assert "Küche".encode("utf-8") in resp.data
    assert "Heizlüfter".encode("utf-8") in resp.data
    assert b"\\u" not in resp.data


def test_get_rooms_reflects_a_changed_file(make_app, tmp_path):
    app = make_app(sample_doc())
    client = app.test_client()
    assert len(client.get("/api/rooms").get_json()["rooms"]) == 2

    doc = sample_doc()
    doc["rooms"].reverse()
    doc["rooms"][1]["name"] = "Salon"
    write_doc(tmp_path / "data.json", doc)
    rooms = client.get("/api/rooms").get_json()["rooms"]
    assert [r["name"] for r in rooms] == ["Küche", "Salon"]
    assert rooms[1]["sockets"][0]["room_id"] == "r_000000000001"


def restart(app):
    """Return a new app on the data file of app, as after a restart."""
    return create_app({"data_file": app.config["RSSWITCH_WEB"]["data_file"]})


def names(client):
    rooms = client.get("/api/rooms").get_json()["rooms"]
    return [r["name"] for r in rooms]


def ids(client):
    rooms = client.get("/api/rooms").get_json()["rooms"]
    return [r["id"] for r in rooms]


def create(client, name):
    return json_request(client, "POST", "/api/rooms", {"name": name})


def test_room_lifecycle_persists_across_restart(make_app):
    app = make_app()
    client = app.test_client()
    flat = {}
    for name in ("Wohnzimmer", "K\u00fcche", "Flur"):
        resp = create(client, name)
        assert resp.status_code == 201
        flat[name] = resp.get_json()["room"]["id"]
    resp = json_request(client, "PATCH", "/api/rooms/" + flat["K\u00fcche"],
                        {"name": "Kueche"})
    assert resp.status_code == 200
    assert resp.get_json() == {"room": {"id": flat["K\u00fcche"],
                                        "name": "Kueche", "sockets": []}}
    order = [flat["Flur"], flat["Wohnzimmer"], flat["K\u00fcche"]]
    resp = json_request(client, "PUT", "/api/rooms/order", {"ids": order})
    assert resp.status_code == 200
    resp = client.delete("/api/rooms/" + flat["Wohnzimmer"])
    assert resp.status_code == 204

    again = restart(app).test_client()
    assert again.get("/api/rooms").get_json() == {"rooms": [
        {"id": flat["Flur"], "name": "Flur", "sockets": []},
        {"id": flat["K\u00fcche"], "name": "Kueche", "sockets": []},
    ]}


def test_create_room_returns_201_with_generated_id(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = create(client, "Bad")
    assert resp.status_code == 201
    room = resp.get_json()["room"]
    assert list(resp.get_json()) == ["room"]
    assert re.fullmatch(r"r_[0-9a-f]{12}", room["id"])
    assert room == {"id": room["id"], "name": "Bad", "sockets": []}
    assert ids(client) == [LIVING, KITCHEN, room["id"]]


def test_create_first_room_creates_the_data_file(make_app, tmp_path):
    app = make_app()
    assert not os.path.exists(str(tmp_path / "data.json"))
    resp = create(app.test_client(), "Wohnzimmer")
    assert resp.status_code == 201
    doc = data_doc(app)
    assert doc["version"] == 1
    assert [r["name"] for r in doc["rooms"]] == ["Wohnzimmer"]


@pytest.mark.parametrize("body", [
    {},
    {"name": None},
    {"name": ""},
    {"name": "   "},
    {"name": "a" * 65},
    {"name": "a\nb"},
    {"name": "\u200b"},
    {"name": 5},
    {"name": ["x"]},
])
def test_create_room_name_validation(make_app, body):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "POST", "/api/rooms", body)
    assert resp.status_code == 422
    error = resp.get_json()["error"]
    assert error["code"] == "validation"
    assert error["field"] == "name"
    assert data_bytes(app) == before


@pytest.mark.parametrize("body, field", [
    ({"name": "Bad", "id": "r_aaaaaaaaaaaa"}, "id"),
    ({"name": "Bad", "sockets": []}, "sockets"),
    ({"name": "Bad", "color": "red"}, "color"),
])
def test_create_room_rejects_client_id_and_unknown_fields(make_app, body,
                                                          field):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "POST", "/api/rooms", body)
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == field
    assert data_bytes(app) == before


@pytest.mark.parametrize("name", [
    PRECOMPOSED, COMBINING, "k\u00fcche", "K\u00dcCHE", "  K\u00fcche  ",
])
def test_duplicate_room_name_is_409_case_and_nfc_insensitive(make_app,
                                                             name):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = create(app.test_client(), name)
    assert resp.status_code == 409
    error = resp.get_json()["error"]
    assert error["code"] == "name_conflict"
    assert error["field"] == "name"
    assert data_bytes(app) == before


def test_same_room_name_twice_gives_201_then_409(make_app):
    client = make_app().test_client()
    assert create(client, "Flur").status_code == 201
    assert create(client, "Flur").status_code == 409
    assert names(client) == ["Flur"]


def test_concurrent_same_name_creates_one_room(make_app):
    app = make_app()
    barrier = threading.Barrier(2)
    statuses = []

    def post():
        client = app.test_client()
        barrier.wait()
        statuses.append(create(client, "Flur").status_code)

    threads = [threading.Thread(target=post) for _ in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    assert sorted(statuses) == [201, 409]
    assert names(app.test_client()) == ["Flur"]


def test_rename_room(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = json_request(client, "PATCH", "/api/rooms/" + KITCHEN,
                        {"name": "  Salon "})
    assert resp.status_code == 200
    assert resp.get_json() == {"room": {"id": KITCHEN, "name": "Salon",
                                        "sockets": []}}
    assert names(client) == ["Wohnzimmer", "Salon"]


def test_rename_keeps_the_sockets_of_the_room(make_app):
    client = make_app(sample_doc()).test_client()
    resp = json_request(client, "PATCH", "/api/rooms/" + LIVING,
                        {"name": "Salon"})
    room = resp.get_json()["room"]
    assert [s["id"] for s in room["sockets"]] == [
        "s_000000000001", "s_000000000002"]


def test_rename_room_conflict_is_409(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "PATCH", "/api/rooms/" + KITCHEN,
                        {"name": "wohnzimmer"})
    assert resp.status_code == 409
    assert resp.get_json()["error"]["code"] == "name_conflict"
    assert data_bytes(app) == before


def test_rename_to_own_name_other_case_is_ok(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = json_request(client, "PATCH", "/api/rooms/" + KITCHEN,
                        {"name": "K\u00dcCHE"})
    assert resp.status_code == 200
    assert names(client) == ["Wohnzimmer", "K\u00dcCHE"]


def test_rename_with_invalid_name_is_422(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "PATCH", "/api/rooms/" + KITCHEN,
                        {"name": None})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "name"
    assert data_bytes(app) == before


def test_patch_room_empty_body_is_noop_and_file_untouched(make_app):
    app = make_app(sample_doc())
    path = app.config["RSSWITCH_WEB"]["data_file"]
    before = data_bytes(app)
    mtime = os.stat(path).st_mtime_ns
    resp = json_request(app.test_client(), "PATCH", "/api/rooms/" + KITCHEN,
                        {})
    assert resp.status_code == 200
    assert resp.get_json() == {"room": {"id": KITCHEN, "name": "K\u00fcche",
                                        "sockets": []}}
    assert data_bytes(app) == before
    assert os.stat(path).st_mtime_ns == mtime


def test_patch_room_unknown_field_is_422(make_app):
    client = make_app(sample_doc()).test_client()
    resp = json_request(client, "PATCH", "/api/rooms/" + KITCHEN,
                        {"sockets": []})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "sockets"


def test_patch_and_delete_of_an_unknown_room_are_404(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    client = app.test_client()
    resp = json_request(client, "PATCH", "/api/rooms/r_ffffffffffff",
                        {"name": "Bad", "color": "red"})
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"
    assert client.delete("/api/rooms/r_ffffffffffff").status_code == 404
    assert data_bytes(app) == before


def test_get_room_by_id_and_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = client.get("/api/rooms/" + LIVING)
    assert resp.status_code == 200
    room = resp.get_json()["room"]
    assert room["name"] == "Wohnzimmer"
    assert [s["room_id"] for s in room["sockets"]] == [LIVING, LIVING]
    resp = client.get("/api/rooms/r_ffffffffffff")
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"


def test_delete_empty_room_204_then_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = client.delete("/api/rooms/" + KITCHEN)
    assert resp.status_code == 204
    assert resp.data == b""
    assert ids(client) == [LIVING]
    resp = client.delete("/api/rooms/" + KITCHEN)
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"


def test_delete_last_room_leaves_empty_household(make_app):
    doc = sample_doc()
    del doc["rooms"][0]
    app = make_app(doc)
    client = app.test_client()
    assert client.delete("/api/rooms/" + KITCHEN).status_code == 204
    assert client.get("/api/rooms").get_json() == {"rooms": []}
    assert data_doc(app) == {"version": 1, "rooms": []}


def test_delete_non_empty_room_is_409_room_not_empty_and_file_untouched(
        make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = app.test_client().delete("/api/rooms/" + LIVING)
    assert resp.status_code == 409
    error = resp.get_json()["error"]
    assert error["code"] == "room_not_empty"
    assert "sockets" in error["message"]
    assert data_bytes(app) == before


@pytest.mark.parametrize("rid", [
    "R_000000000002", "%20" + KITCHEN, KITCHEN + "%20", KITCHEN.upper(),
])
def test_delete_room_id_case_or_whitespace_is_404(make_app, rid):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = app.test_client().delete("/api/rooms/" + rid)
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"
    assert data_bytes(app) == before


def test_reorder_rooms_persists(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = json_request(client, "PUT", "/api/rooms/order",
                        {"ids": [KITCHEN, LIVING]})
    assert resp.status_code == 200
    assert resp.get_json() == {"ids": [KITCHEN, LIVING]}
    assert ids(client) == [KITCHEN, LIVING]
    assert ids(restart(app).test_client()) == [KITCHEN, LIVING]


def test_reorder_keeps_the_sockets(make_app):
    client = make_app(sample_doc()).test_client()
    json_request(client, "PUT", "/api/rooms/order",
                 {"ids": [KITCHEN, LIVING]})
    rooms = client.get("/api/rooms").get_json()["rooms"]
    assert [s["id"] for s in rooms[1]["sockets"]] == [
        "s_000000000001", "s_000000000002"]


@pytest.mark.parametrize("wanted", [
    [LIVING],
    [LIVING, KITCHEN, "r_ffffffffffff"],
    [LIVING, LIVING],
    [LIVING, KITCHEN, LIVING],
    [LIVING, "r_ffffffffffff"],
    [],
], ids=["missing", "extra", "repeated", "repeated-and-full", "stale",
        "empty"])
def test_reorder_mismatch_is_409(make_app, wanted):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "PUT", "/api/rooms/order",
                        {"ids": wanted})
    assert resp.status_code == 409
    assert resp.get_json()["error"]["code"] == "order_mismatch"
    assert data_bytes(app) == before


@pytest.mark.parametrize("body", [
    {},
    {"ids": None},
    {"ids": LIVING},
    {"ids": {"a": 1}},
    {"ids": [LIVING, 1]},
    {"ids": [None]},
    {"ids": [[LIVING]]},
])
def test_reorder_ids_type_is_422(make_app, body):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "PUT", "/api/rooms/order", body)
    assert resp.status_code == 422
    error = resp.get_json()["error"]
    assert error["code"] == "validation"
    assert error["field"] == "ids"
    assert data_bytes(app) == before


def test_reorder_unknown_field_is_422(make_app):
    client = make_app(sample_doc()).test_client()
    resp = json_request(client, "PUT", "/api/rooms/order",
                        {"ids": [LIVING, KITCHEN], "extra": 1})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "extra"


def test_reorder_twice_is_idempotent(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    body = {"ids": [KITCHEN, LIVING]}
    assert json_request(client, "PUT", "/api/rooms/order",
                        body).status_code == 200
    once = data_bytes(app)
    assert json_request(client, "PUT", "/api/rooms/order",
                        body).status_code == 200
    assert data_bytes(app) == once
    assert ids(client) == [KITCHEN, LIVING]


def test_new_ids_never_collide_with_existing(monkeypatch):
    doc = sample_doc()
    suffixes = iter(["000000000001", "000000000002", "0a0b0c0d0e0f"])
    asked = []

    def token_hex(nbytes):
        asked.append(nbytes)
        return next(suffixes)

    monkeypatch.setattr(model.secrets, "token_hex", token_hex)
    assert model.new_id("r_", doc) == "r_0a0b0c0d0e0f"
    assert asked == [6, 6, 6]

    # A socket id is taken too, and a suffix of another kind is free.
    suffixes = iter(["000000000002", "0a0b0c0d0e0f"])
    assert model.new_id("s_", doc) == "s_0a0b0c0d0e0f"


def test_created_ids_differ(make_app):
    client = make_app().test_client()
    made = {create(client, "Raum %d" % i).get_json()["room"]["id"]
            for i in range(5)}
    assert len(made) == 5
