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

"""Tests of GET /api/rooms, from the data file to JSON over HTTP."""

from __future__ import annotations

import os

from conftest import sample_doc, write_doc

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
