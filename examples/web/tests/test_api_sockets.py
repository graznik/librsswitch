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

"""Tests of the socket routes, from HTTP to the data file."""

from __future__ import annotations

import os
import re
import threading

import pytest
from conftest import data_bytes, data_doc, json_request, sample_doc

LIVING = "r_000000000001"
KITCHEN = "r_000000000002"
LAMP = "s_000000000001"     # REV group 0, socket 0, in the living room
HEATER = "s_000000000002"   # Pollin 17/23, switched on at 08:00
UNKNOWN = "s_ffffffffffff"
SWITCHED = {"state": "on", "at": "2026-09-29T08:00:00.000Z"}
PRECOMPOSED = "Küche"
COMBINING = "Küche"


def new_socket(**changes):
    body = {"name": "Kaffeemaschine", "type": "rev_008345", "group": 1,
            "number": 1}
    body.update(changes)
    return body


def add(client, rid, body=None):
    return json_request(client, "POST", "/api/rooms/%s/sockets" % rid,
                        new_socket() if body is None else body)


def edit(client, sid, body):
    return json_request(client, "PATCH", "/api/sockets/" + sid, body)


def sockets_of(client, rid):
    rooms = client.get("/api/rooms").get_json()["rooms"]
    return [r for r in rooms if r["id"] == rid][0]["sockets"]


def error_of(resp):
    return resp.get_json()["error"]


def test_create_socket_201_appended_with_null_last_sent_and_empty_warnings(
        make_app):
    client = make_app(sample_doc()).test_client()
    resp = add(client, LIVING)
    assert resp.status_code == 201
    body = resp.get_json()
    assert list(body) == ["socket", "warnings"]
    sock = body["socket"]
    assert re.fullmatch(r"s_[0-9a-f]{12}", sock["id"])
    assert sock == {"id": sock["id"], "room_id": LIVING,
                    "name": "Kaffeemaschine", "type": "rev_008345",
                    "group": 1, "number": 1, "last_sent": None}
    assert body["warnings"] == []
    assert [s["id"] for s in sockets_of(client, LIVING)] == [
        LAMP, HEATER, sock["id"]]


def test_first_socket_of_an_empty_room_is_accepted(make_app):
    client = make_app(sample_doc()).test_client()
    resp = add(client, KITCHEN, new_socket(name=" Herd ",
                                           type="pollin_2605", group=31,
                                           number=31))
    assert resp.status_code == 201
    assert resp.get_json()["socket"]["name"] == "Herd"
    assert [s["name"] for s in sockets_of(client, KITCHEN)] == ["Herd"]


def test_create_socket_persists_across_a_new_app(make_app):
    app = make_app(sample_doc())
    sid = add(app.test_client(), KITCHEN).get_json()["socket"]["id"]
    doc = data_doc(app)
    assert doc["rooms"][1]["sockets"] == [{
        "id": sid, "name": "Kaffeemaschine", "type": "rev_008345",
        "group": 1, "number": 1, "last_sent": None}]


def test_create_socket_in_unknown_room_is_404(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = add(app.test_client(), "r_ffffffffffff", {"bogus": 1})
    assert resp.status_code == 404
    assert error_of(resp)["code"] == "not_found"
    assert data_bytes(app) == before


@pytest.mark.parametrize("body, field", [
    ({}, "name"),
    ({"name": None, "type": "nope", "group": 99, "number": 99}, "name"),
    ({"name": "", "type": "rev_008345", "group": 0, "number": 0}, "name"),
    ({"name": "a" * 65, "type": "rev_008345", "group": 0, "number": 0},
     "name"),
    ({"name": "A"}, "type"),
    ({"name": "A", "type": None, "group": 0, "number": 0}, "type"),
    ({"name": "A", "type": "nope", "group": 99, "number": 99}, "type"),
    ({"name": "A", "type": "REV_008345", "group": 0, "number": 0}, "type"),
    ({"name": "A", "type": " rev_008345", "group": 0, "number": 0}, "type"),
    ({"name": "A", "type": "rev", "group": 0, "number": 0}, "type"),
    ({"name": "A", "type": "rev_008345"}, "group"),
    ({"name": "A", "type": "rev_008345", "group": 9, "number": 9}, "group"),
    ({"name": "A", "type": "rev_008345", "group": 0}, "number"),
    ({"name": "A", "type": "rev_008345", "group": 0, "number": 9},
     "number"),
])
def test_create_socket_field_validation_first_field_wins(make_app, body,
                                                         field):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = add(app.test_client(), KITCHEN, body)
    assert resp.status_code == 422
    assert error_of(resp)["code"] == "validation"
    assert error_of(resp)["field"] == field
    assert data_bytes(app) == before


@pytest.mark.parametrize("type_name, group, number, ok, field", [
    ("rev_008345", 0, 0, True, None),
    ("rev_008345", 3, 2, True, None),
    ("rev_008345", -1, 0, False, "group"),
    ("rev_008345", 4, 0, False, "group"),
    ("rev_008345", 0, 3, False, "number"),
    ("pollin_2605", 0, 0, True, None),
    ("pollin_2605", 31, 31, True, None),
    ("pollin_2605", 4, 3, True, None),
    ("pollin_2605", 32, 0, False, "group"),
    ("pollin_2605", 0, 32, False, "number"),
])
def test_create_socket_address_limits(make_app, type_name, group, number,
                                      ok, field):
    client = make_app(sample_doc()).test_client()
    resp = add(client, KITCHEN, new_socket(type=type_name, group=group,
                                           number=number))
    if ok:
        assert resp.status_code == 201
    else:
        assert resp.status_code == 422
        assert error_of(resp)["field"] == field


@pytest.mark.parametrize("value", [
    1.0, 3.0, "3", True, False, None, 2 ** 64, [1], {"a": 1},
])
@pytest.mark.parametrize("field", ["group", "number"])
def test_create_socket_needs_json_integers(make_app, field, value):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = add(app.test_client(), KITCHEN,
               new_socket(type="pollin_2605", **{field: value}))
    assert resp.status_code == 422
    assert error_of(resp)["field"] == field
    assert data_bytes(app) == before


@pytest.mark.parametrize("extra, field", [
    ({"id": "s_aaaaaaaaaaaa"}, "id"),
    ({"last_sent": None}, "last_sent"),
    ({"last_sent": SWITCHED}, "last_sent"),
    ({"room_id": KITCHEN}, "room_id"),
    ({"color": "red"}, "color"),
])
def test_create_socket_rejects_id_last_sent_room_id(make_app, extra, field):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = add(app.test_client(), KITCHEN, new_socket(**extra))
    assert resp.status_code == 422
    assert error_of(resp)["field"] == field
    assert data_bytes(app) == before


def test_socket_name_unique_per_room_409_and_ok_in_other_room(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    assert add(client, LIVING).status_code == 201
    before = data_bytes(app)
    for name in ("Kaffeemaschine", "kaffeemaschine", "  KAFFEEMASCHINE ",
                 "stehlampe"):
        resp = add(client, LIVING, new_socket(name=name, group=2))
        assert resp.status_code == 409, name
        assert error_of(resp)["code"] == "name_conflict"
        assert error_of(resp)["field"] == "name"
    assert data_bytes(app) == before
    assert add(client, KITCHEN, new_socket(name="Stehlampe")).status_code \
        == 201


def test_socket_names_conflict_across_normalisation_forms(make_app):
    client = make_app(sample_doc()).test_client()
    assert add(client, KITCHEN, new_socket(name=PRECOMPOSED)).status_code \
        == 201
    for name in (COMBINING, "KÜCHE"):
        resp = add(client, KITCHEN, new_socket(name=name, group=2))
        assert resp.status_code == 409


def test_concurrent_creates_in_one_room_both_persist(make_app):
    app = make_app(sample_doc())
    barrier = threading.Barrier(2)
    results = []

    def post(name, group):
        client = app.test_client()
        barrier.wait()
        results.append(add(client, KITCHEN, new_socket(name=name,
                                                       group=group)))

    threads = [threading.Thread(target=post, args=("A", 1)),
               threading.Thread(target=post, args=("B", 2))]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    assert [r.status_code for r in results] == [201, 201]
    made = {r.get_json()["socket"]["id"] for r in results}
    assert len(made) == 2
    stored = sockets_of(app.test_client(), KITCHEN)
    assert {s["id"] for s in stored} == made
    assert sorted(s["name"] for s in stored) == ["A", "B"]


@pytest.mark.parametrize("round_", range(5))
def test_concurrent_add_and_delete_of_a_room(make_app, round_):
    app = make_app(sample_doc())
    barrier = threading.Barrier(2)
    answers = {}

    def add_socket():
        client = app.test_client()
        barrier.wait()
        answers["add"] = add(client, KITCHEN).status_code

    def delete_room():
        client = app.test_client()
        barrier.wait()
        answers["delete"] = client.delete("/api/rooms/" + KITCHEN) \
            .status_code

    threads = [threading.Thread(target=add_socket),
               threading.Thread(target=delete_room)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    # Either the socket landed first and the room could not be deleted,
    # or the room went first and the socket had nowhere to go.
    assert (answers["add"], answers["delete"]) in ((201, 409), (404, 204))
    rooms = data_doc(app)["rooms"]
    if answers["delete"] == 204:
        assert [r["id"] for r in rooms] == [LIVING]
    else:
        assert len(rooms[1]["sockets"]) == 1


def test_get_socket_and_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = client.get("/api/sockets/" + HEATER)
    assert resp.status_code == 200
    assert resp.get_json() == {"socket": {
        "id": HEATER, "room_id": LIVING, "name": "Heizlüfter",
        "type": "pollin_2605", "group": 17, "number": 23,
        "last_sent": SWITCHED}}
    resp = client.get("/api/sockets/" + UNKNOWN)
    assert resp.status_code == 404
    assert error_of(resp)["code"] == "not_found"


def test_edit_validates_merged_record(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    sid = add(client, KITCHEN, new_socket(type="pollin_2605", group=20,
                                          number=1)).get_json()[
        "socket"]["id"]
    before = data_bytes(app)
    resp = edit(client, sid, {"type": "rev_008345"})
    assert resp.status_code == 422
    assert error_of(resp)["field"] == "group"
    assert data_bytes(app) == before
    resp = edit(client, sid, {"number": 3, "type": "rev_008345",
                              "group": 3})
    assert resp.status_code == 422
    assert error_of(resp)["field"] == "number"
    assert data_bytes(app) == before


def test_edit_type_and_group_together_ok(make_app):
    client = make_app(sample_doc()).test_client()
    sid = add(client, KITCHEN, new_socket(type="pollin_2605", group=20,
                                          number=1)).get_json()[
        "socket"]["id"]
    resp = edit(client, sid, {"type": "rev_008345", "group": 3})
    assert resp.status_code == 200
    sock = resp.get_json()["socket"]
    assert (sock["type"], sock["group"], sock["number"]) == (
        "rev_008345", 3, 1)


def test_edit_response_shape(make_app):
    client = make_app(sample_doc()).test_client()
    resp = edit(client, LAMP, {"name": "Leselampe"})
    assert resp.status_code == 200
    assert list(resp.get_json()) == ["socket", "warnings"]
    assert resp.get_json()["socket"] == {
        "id": LAMP, "room_id": LIVING, "name": "Leselampe",
        "type": "rev_008345", "group": 0, "number": 0, "last_sent": None}
    assert resp.get_json()["warnings"] == []


@pytest.mark.parametrize("body", [
    {"number": 22},
    {"group": 16},
    {"type": "rev_008345", "group": 1, "number": 1},
    {"name": "Neu", "number": 0},
])
def test_address_edit_clears_last_sent(make_app, body):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = edit(client, HEATER, body)
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["last_sent"] is None
    assert sockets_of(client, LIVING)[1]["last_sent"] is None


def test_rename_keeps_last_sent(make_app):
    client = make_app(sample_doc()).test_client()
    resp = edit(client, HEATER, {"name": "Radiator"})
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["last_sent"] == SWITCHED
    assert sockets_of(client, LIVING)[1]["last_sent"] == SWITCHED


def test_setting_same_address_keeps_last_sent(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = edit(app.test_client(), HEATER,
                {"type": "pollin_2605", "group": 17, "number": 23})
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["last_sent"] == SWITCHED
    assert data_bytes(app) == before


def test_patch_empty_is_noop(make_app):
    app = make_app(sample_doc())
    path = app.config["RSSWITCH_WEB"]["data_file"]
    before = data_bytes(app)
    mtime = os.stat(path).st_mtime_ns
    resp = edit(app.test_client(), HEATER, {})
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["id"] == HEATER
    assert resp.get_json()["socket"]["last_sent"] == SWITCHED
    assert resp.get_json()["warnings"] == []
    assert data_bytes(app) == before
    assert os.stat(path).st_mtime_ns == mtime


@pytest.mark.parametrize("field", ["name", "type", "group", "number"])
def test_patch_null_field_is_422(make_app, field):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = edit(app.test_client(), HEATER, {field: None})
    assert resp.status_code == 422
    assert error_of(resp)["field"] == field
    assert data_bytes(app) == before


@pytest.mark.parametrize("sid, body", [
    (LAMP, {"group": 3.0}),
    (LAMP, {"number": 1.0}),
    (HEATER, {"group": 3.0}),
    (HEATER, {"number": "3"}),
    (HEATER, {"group": True}),
])
def test_patch_group_float_is_422(make_app, sid, body):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = edit(app.test_client(), sid, body)
    assert resp.status_code == 422
    assert error_of(resp)["field"] == list(body)[0]
    assert data_bytes(app) == before


@pytest.mark.parametrize("body, field", [
    ({"id": "s_aaaaaaaaaaaa"}, "id"),
    ({"last_sent": None}, "last_sent"),
    ({"color": "red"}, "color"),
])
def test_patch_unknown_field_is_422(make_app, body, field):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = edit(app.test_client(), HEATER, body)
    assert resp.status_code == 422
    assert error_of(resp)["field"] == field
    assert data_bytes(app) == before


def test_patch_unknown_socket_is_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = edit(client, UNKNOWN, {"name": "X", "color": "red"})
    assert resp.status_code == 404
    assert error_of(resp)["code"] == "not_found"


def test_rename_case_only_ok_and_conflict_409(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    assert edit(client, LAMP, {"name": "STEHLAMPE"}).status_code == 200
    assert edit(client, LAMP, {"name": "  Stehlampe "}).status_code == 200
    before = data_bytes(app)
    resp = edit(client, LAMP, {"name": "heizlÜfter"})
    assert resp.status_code == 409
    assert error_of(resp)["code"] == "name_conflict"
    assert data_bytes(app) == before


def test_editing_another_field_ignores_a_name_that_is_not_unique(make_app):
    doc = sample_doc()
    doc["rooms"][0]["sockets"][1]["name"] = "Stehlampe"
    client = make_app(doc).test_client()
    assert edit(client, HEATER, {"number": 22}).status_code == 200


def test_edit_keeps_position(make_app):
    client = make_app(sample_doc()).test_client()
    resp = edit(client, LAMP, {"name": "Leselampe", "group": 2})
    assert resp.status_code == 200
    assert [s["id"] for s in sockets_of(client, LIVING)] == [LAMP, HEATER]


def test_concurrent_patches_of_different_fields_both_apply(make_app):
    app = make_app(sample_doc())
    barrier = threading.Barrier(2)
    statuses = []

    def patch(body):
        client = app.test_client()
        barrier.wait()
        statuses.append(edit(client, LAMP, body).status_code)

    threads = [threading.Thread(target=patch, args=({"name": "Leselampe"},)),
               threading.Thread(target=patch, args=({"group": 2},))]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    assert statuses == [200, 200]
    sock = sockets_of(app.test_client(), LIVING)[0]
    assert (sock["name"], sock["group"]) == ("Leselampe", 2)


def test_delete_socket_204_then_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = client.delete("/api/sockets/" + LAMP)
    assert resp.status_code == 204
    assert resp.data == b""
    assert [s["id"] for s in sockets_of(client, LIVING)] == [HEATER]
    resp = client.delete("/api/sockets/" + LAMP)
    assert resp.status_code == 404
    assert error_of(resp)["code"] == "not_found"


def test_delete_unknown_socket_changes_nothing(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    assert app.test_client().delete("/api/sockets/" + UNKNOWN) \
        .status_code == 404
    assert data_bytes(app) == before


def test_deleted_during_switch_stays_deleted(fake_daemon, switch_app):
    holder = []

    def behaviour(line):
        resp = holder[0].delete("/api/sockets/" + HEATER)
        assert resp.status_code == 204
        return b"OK\n"

    fake = fake_daemon(behaviour)
    app = switch_app(fake)
    holder.append(app.test_client())
    resp = json_request(app.test_client(), "POST",
                        "/api/sockets/%s/switch" % HEATER,
                        {"state": "on"})
    assert fake.lines == ["1 17 23 1"]
    assert resp.status_code == 200
    assert resp.get_json() == {"ok": True, "recorded": False,
                               "socket": None}
    assert [s["id"] for s in sockets_of(app.test_client(), LIVING)] == [LAMP]
    assert HEATER not in str(data_doc(app))


def order(client, rid, ids):
    return json_request(client, "PUT", "/api/rooms/%s/sockets/order" % rid,
                        {"ids": ids})


def test_reorder_sockets_persists(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    resp = order(client, LIVING, [HEATER, LAMP])
    assert resp.status_code == 200
    assert resp.get_json() == {"ids": [HEATER, LAMP]}
    assert [s["id"] for s in sockets_of(client, LIVING)] == [HEATER, LAMP]
    assert [s["id"] for s in data_doc(app)["rooms"][0]["sockets"]] == [
        HEATER, LAMP]


@pytest.mark.parametrize("wanted", [
    [LAMP],
    [LAMP, HEATER, "s_ffffffffffff"],
    [LAMP, LAMP],
    [LAMP, HEATER, LAMP],
    [LAMP, "s_ffffffffffff"],
    [],
], ids=["missing", "extra", "repeated", "repeated-and-full", "stale",
        "empty"])
def test_reorder_sockets_mismatch_is_409(make_app, wanted):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = order(app.test_client(), LIVING, wanted)
    assert resp.status_code == 409
    assert error_of(resp)["code"] == "order_mismatch"
    assert data_bytes(app) == before


def test_reorder_sockets_with_a_socket_of_another_room_is_409(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    other = add(client, KITCHEN).get_json()["socket"]["id"]
    before = data_bytes(app)
    resp = order(client, LIVING, [LAMP, HEATER, other])
    assert resp.status_code == 409
    assert data_bytes(app) == before


def test_reorder_sockets_made_stale_by_a_delete_is_409(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    assert client.delete("/api/sockets/" + HEATER).status_code == 204
    before = data_bytes(app)
    resp = order(client, LIVING, [HEATER, LAMP])
    assert resp.status_code == 409
    assert data_bytes(app) == before


@pytest.mark.parametrize("body", [
    {}, {"ids": None}, {"ids": LAMP}, {"ids": [LAMP, 1]}, {"ids": [None]},
])
def test_reorder_sockets_ids_type_is_422(make_app, body):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = json_request(app.test_client(), "PUT",
                        "/api/rooms/%s/sockets/order" % LIVING, body)
    assert resp.status_code == 422
    assert error_of(resp)["field"] == "ids"
    assert data_bytes(app) == before


def test_reorder_sockets_of_an_unknown_room_is_404(make_app):
    client = make_app(sample_doc()).test_client()
    resp = order(client, "r_ffffffffffff", [])
    assert resp.status_code == 404
    assert error_of(resp)["code"] == "not_found"


def test_reorder_sockets_twice_is_idempotent(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    assert order(client, LIVING, [HEATER, LAMP]).status_code == 200
    once = data_bytes(app)
    assert order(client, LIVING, [HEATER, LAMP]).status_code == 200
    assert data_bytes(app) == once


def test_reorder_sockets_leaves_the_rooms_alone(make_app):
    client = make_app(sample_doc()).test_client()
    order(client, LIVING, [HEATER, LAMP])
    rooms = client.get("/api/rooms").get_json()["rooms"]
    assert [r["id"] for r in rooms] == [LIVING, KITCHEN]


def move(client, sid, rid, **more):
    return edit(client, sid, dict(more, room_id=rid))


def test_move_appends_to_target_and_keeps_id_and_last_sent(make_app):
    client = make_app(sample_doc()).test_client()
    first = add(client, KITCHEN, new_socket(name="Herd")).get_json()[
        "socket"]["id"]
    resp = move(client, HEATER, KITCHEN)
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["socket"] == {
        "id": HEATER, "room_id": KITCHEN, "name": "Heizl\u00fcfter",
        "type": "pollin_2605", "group": 17, "number": 23,
        "last_sent": SWITCHED}
    assert body["warnings"] == []
    assert [s["id"] for s in sockets_of(client, KITCHEN)] == [first, HEATER]
    assert [s["id"] for s in sockets_of(client, LIVING)] == [LAMP]
    assert sockets_of(client, KITCHEN)[1]["last_sent"] == SWITCHED


def test_move_to_unknown_room_is_422_room_id_and_file_untouched(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = move(app.test_client(), HEATER, "r_ffffffffffff")
    assert resp.status_code == 422
    assert error_of(resp)["code"] == "validation"
    assert error_of(resp)["field"] == "room_id"
    assert data_bytes(app) == before


@pytest.mark.parametrize("value", [None, 5, True, [KITCHEN], {"id": KITCHEN}])
def test_move_room_id_not_a_string_is_422(make_app, value):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = move(app.test_client(), HEATER, value)
    assert resp.status_code == 422
    assert error_of(resp)["field"] == "room_id"
    assert data_bytes(app) == before


@pytest.mark.parametrize("rid", [
    KITCHEN.upper(), " " + KITCHEN, KITCHEN + " ", "",
])
def test_move_room_id_is_matched_exactly(make_app, rid):
    app = make_app(sample_doc())
    before = data_bytes(app)
    resp = move(app.test_client(), HEATER, rid)
    assert resp.status_code == 422
    assert error_of(resp)["field"] == "room_id"
    assert data_bytes(app) == before


def test_move_reports_a_bad_name_before_a_bad_room(make_app):
    client = make_app(sample_doc()).test_client()
    resp = edit(client, HEATER, {"room_id": "r_ffffffffffff", "name": ""})
    assert error_of(resp)["field"] == "name"
    resp = edit(client, HEATER, {"room_id": "r_ffffffffffff", "group": 99})
    assert error_of(resp)["field"] == "group"


def test_move_name_conflict_in_target_is_409(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    assert add(client, KITCHEN, new_socket(name="HEIZL\u00dcFTER")) \
        .status_code == 201
    before = data_bytes(app)
    resp = move(client, HEATER, KITCHEN)
    assert resp.status_code == 409
    assert error_of(resp)["code"] == "name_conflict"
    assert error_of(resp)["field"] == "name"
    assert data_bytes(app) == before


def test_move_with_a_new_name_avoids_the_conflict(make_app):
    client = make_app(sample_doc()).test_client()
    add(client, KITCHEN, new_socket(name="Heizl\u00fcfter"))
    resp = move(client, HEATER, KITCHEN, name="Radiator")
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["name"] == "Radiator"
    assert resp.get_json()["socket"]["room_id"] == KITCHEN


def test_move_to_own_room_keeps_position(make_app):
    app = make_app(sample_doc())
    before = data_bytes(app)
    client = app.test_client()
    resp = move(client, LAMP, LIVING)
    assert resp.status_code == 200
    assert resp.get_json()["socket"]["room_id"] == LIVING
    assert [s["id"] for s in sockets_of(client, LIVING)] == [LAMP, HEATER]
    assert data_bytes(app) == before


def test_move_with_address_change_clears_last_sent(make_app):
    client = make_app(sample_doc()).test_client()
    resp = move(client, HEATER, KITCHEN, number=22)
    assert resp.status_code == 200
    sock = resp.get_json()["socket"]
    assert (sock["room_id"], sock["number"], sock["last_sent"]) == (
        KITCHEN, 22, None)
    assert sockets_of(client, KITCHEN)[0]["last_sent"] is None


def test_move_leaves_id_exactly_once_in_file(make_app):
    app = make_app(sample_doc())
    assert move(app.test_client(), HEATER, KITCHEN).status_code == 200
    text = data_bytes(app).decode("utf-8")
    assert text.count(HEATER) == 1
    doc = data_doc(app)
    assert [s["id"] for s in doc["rooms"][0]["sockets"]] == [LAMP]
    assert [s["id"] for s in doc["rooms"][1]["sockets"]] == [HEATER]


def test_concurrent_moves_leave_the_socket_exactly_once(make_app):
    app = make_app(sample_doc())
    client = app.test_client()
    hall = json_request(client, "POST", "/api/rooms",
                        {"name": "Flur"}).get_json()["room"]["id"]
    barrier = threading.Barrier(2)
    statuses = []

    def go(rid):
        own = app.test_client()
        barrier.wait()
        statuses.append(move(own, HEATER, rid).status_code)

    threads = [threading.Thread(target=go, args=(KITCHEN,)),
               threading.Thread(target=go, args=(hall,))]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
    assert statuses == [200, 200]
    assert data_bytes(app).decode("utf-8").count(HEATER) == 1
    homes = [r["id"] for r in client.get("/api/rooms").get_json()["rooms"]
             if any(s["id"] == HEATER for s in r["sockets"])]
    assert len(homes) == 1 and homes[0] in (KITCHEN, hall)
    heater = [s for r in data_doc(app)["rooms"] for s in r["sockets"]
              if s["id"] == HEATER][0]
    assert heater["last_sent"] == SWITCHED


def test_source_room_can_be_deleted_after_moving_its_last_socket(make_app):
    client = make_app(sample_doc()).test_client()
    assert client.delete("/api/rooms/" + LIVING).status_code == 409
    assert move(client, LAMP, KITCHEN).status_code == 200
    assert move(client, HEATER, KITCHEN).status_code == 200
    assert client.delete("/api/rooms/" + LIVING).status_code == 204
    assert [s["id"] for s in sockets_of(client, KITCHEN)] == [LAMP, HEATER]


def test_switch_after_move_records_on_moved_socket(fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake)
    app.extensions["rsswitch_web"].clock = lambda: "2026-09-29T10:00:00.000Z"
    client = app.test_client()
    assert move(client, HEATER, KITCHEN).status_code == 200
    resp = json_request(client, "POST", "/api/sockets/%s/switch" % HEATER,
                        {"state": "off"})
    assert resp.status_code == 200
    assert resp.get_json()["recorded"] is True
    assert fake.lines == ["1 17 23 0"]
    kitchen = data_doc(app)["rooms"][1]["sockets"]
    assert [s["id"] for s in kitchen] == [HEATER]
    assert kitchen[0]["last_sent"] == {"state": "off",
                                       "at": "2026-09-29T10:00:00.000Z"}
