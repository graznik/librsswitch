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

"""Tests of All-off for a room and for the household."""

from __future__ import annotations

import json
import socket
import threading
import time

import pytest
from conftest import post_json, refused_port, write_doc

from rsswitch_web import daemon
from rsswitch_web.daemon import (DaemonProtocolError, DaemonReplyError,
                                 DaemonTimeout, DaemonUnreachable)
from rsswitch_web.errors import ApiError
from rsswitch_web.store import Store, StoreError
from rsswitch_web.switching import all_off

AT = "2026-09-29T10:00:00.000Z"
REV = "rev_008345"
POLLIN = "pollin_2605"


def make_doc(*rooms):
    """Build a document. A room is a list of (type, group, number).

    Room ids are r1, r2, ... and socket ids are s1, s2, ... in the
    order they are given.
    """
    n = 0
    out = []
    for i, sockets in enumerate(rooms, 1):
        entries = []
        for kind, group, number in sockets:
            n += 1
            entries.append({"id": "s%d" % n, "name": "Socket %d" % n,
                            "type": kind, "group": group,
                            "number": number, "last_sent": None})
        out.append({"id": "r%d" % i, "name": "Room %d" % i,
                    "sockets": entries})
    return {"version": 1, "rooms": out}


class FakeClient:
    """Records the commands and plays one scripted outcome per address.

    outcomes maps (dev, group, number) to an exception to raise. on_send
    is called with the number of the command before it is answered.
    """

    def __init__(self, outcomes=None, on_send=None, events=None):
        self.calls = []
        self.outcomes = outcomes or {}
        self.on_send = on_send
        self.events = events

    def send(self, dev, group, number, data):
        self.calls.append((dev, group, number, data))
        if self.events is not None:
            self.events.append(("send", dev, group, number))
        if self.on_send:
            self.on_send(len(self.calls))
        outcome = self.outcomes.get((dev, group, number))
        if outcome is not None:
            raise outcome


class Rig:
    """A store on a temporary file, a client and a recorded sleep."""

    def __init__(self, tmp_path, doc, client=None, name="data.json"):
        self.path = tmp_path / name
        write_doc(self.path, doc)
        self.store = Store(str(self.path))
        self.events = []
        self.client = client or FakeClient(events=self.events)
        self.sleeps = []

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.events.append(("sleep", seconds))

    def run(self, room_id=None, gap=0.2):
        return all_off(self.store, self.client, room_id, gap_seconds=gap,
                       sleep=self.sleep, clock=lambda: AT)

    def doc(self):
        with open(str(self.path), "rb") as f:
            return json.loads(f.read().decode("utf-8"))


def statuses(report):
    return [r["status"] for r in report["results"]]


def test_dedupes_addresses_in_first_seen_order(tmp_path):
    rig = Rig(tmp_path, make_doc(
        [(POLLIN, 1, 1), (REV, 0, 0), (POLLIN, 1, 1)],
        [(REV, 0, 0), (POLLIN, 2, 2), (POLLIN, 0, 0), (REV, 0, 0)]))
    report = rig.run()
    assert rig.client.calls == [(1, 1, 1, 0), (0, 0, 0, 0), (1, 2, 2, 0),
                                (1, 0, 0, 0)]
    assert len(report["results"]) == 7
    assert statuses(report) == ["ok"] * 7
    doc = rig.doc()
    stored = [s["last_sent"] for r in doc["rooms"] for s in r["sockets"]]
    assert stored == [{"state": "off", "at": AT}] * 7


def test_sockets_that_differ_only_in_type_are_sent_separately(tmp_path):
    rig = Rig(tmp_path, make_doc([(REV, 1, 1), (POLLIN, 1, 1)]))
    rig.run()
    assert rig.client.calls == [(0, 1, 1, 0), (1, 1, 1, 0)]


def test_sends_in_display_order(tmp_path):
    rig = Rig(tmp_path, make_doc(
        [(POLLIN, 9, 9), (POLLIN, 3, 3)], [(REV, 2, 2), (POLLIN, 1, 1)]))
    report = rig.run()
    assert rig.client.calls == [(1, 9, 9, 0), (1, 3, 3, 0), (0, 2, 2, 0),
                                (1, 1, 1, 0)]
    assert [(r["socket_id"], r["room_id"]) for r in report["results"]] == [
        ("s1", "r1"), ("s2", "r1"), ("s3", "r2"), ("s4", "r2")]


def test_gap_only_between_sends(tmp_path):
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2)],
                                 [(POLLIN, 3, 3), (POLLIN, 1, 1)]))
    rig.run(gap=0.35)
    assert len(rig.client.calls) == 3
    assert rig.sleeps == [0.35, 0.35]
    assert [e[0] for e in rig.events] == ["send", "sleep", "send", "sleep",
                                          "send"]


def test_err_reply_continues(tmp_path):
    client = FakeClient({(1, 2, 2): DaemonReplyError("send", "failed")})
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2),
                                  (POLLIN, 3, 3)]), client)
    report = rig.run()
    assert len(client.calls) == 3
    assert statuses(report) == ["ok", "error", "ok"]
    error = report["results"][1]["error"]
    assert error["code"] == "daemon_error"
    assert error["daemon_class"] == "send"
    assert error["daemon_message"] == "failed"
    stored = [s["last_sent"] for s in rig.doc()["rooms"][0]["sockets"]]
    assert stored == [{"state": "off", "at": AT}, None,
                      {"state": "off", "at": AT}]


def test_protocol_error_continues(tmp_path):
    client = FakeClient({(1, 1, 1): DaemonProtocolError("garbage")})
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2)]), client)
    report = rig.run()
    assert len(client.calls) == 2
    assert statuses(report) == ["error", "ok"]
    assert report["results"][0]["error"]["code"] == "daemon_protocol"


@pytest.mark.parametrize("failure, code", [
    (DaemonUnreachable("refused"), "daemon_unreachable"),
    (DaemonTimeout("slow"), "daemon_timeout"),
])
def test_unreachable_and_timeout_stop_and_skip_the_rest(tmp_path, failure,
                                                        code):
    client = FakeClient({(1, 2, 2): failure})
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2)],
                                 [(POLLIN, 3, 3), (POLLIN, 4, 4)]), client)
    report = rig.run()
    assert len(client.calls) == 2
    assert statuses(report) == ["ok", "error", "skipped", "skipped"]
    assert report["results"][1]["error"]["code"] == code
    for entry in report["results"][2:]:
        assert entry["error"] == report["results"][1]["error"]
    assert report["ok"] is False
    assert report["summary"] == {"ok": 1, "error": 1, "skipped": 2}
    stored = [s["last_sent"] for r in rig.doc()["rooms"]
              for s in r["sockets"]]
    assert stored == [{"state": "off", "at": AT}, None, None, None]


def test_ok_flag_and_summary_counts(tmp_path):
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 1, 1),
                                  (POLLIN, 2, 2)]))
    report = rig.run()
    assert report["ok"] is True
    assert report["summary"] == {"ok": 3, "error": 0, "skipped": 0}
    client = FakeClient({(1, 2, 2): DaemonReplyError("range", "x")})
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2)]), client,
              name="second.json")
    report = rig.run()
    assert report["ok"] is False
    assert report["summary"] == {"ok": 1, "error": 1, "skipped": 0}


def test_last_sent_written_per_address_before_next_send(tmp_path):
    seen = {}

    def on_send(n):
        if n == 2:
            seen["stored"] = rig.doc()["rooms"][0]["sockets"][0][
                "last_sent"]

    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2)]),
              FakeClient(on_send=on_send))
    rig.run()
    assert seen["stored"] == {"state": "off", "at": AT}


def test_socket_deleted_mid_batch_is_ok_not_recorded(tmp_path):
    def on_send(n):
        if n == 1:
            def delete(doc):
                doc["rooms"][0]["sockets"].pop(1)
            rig.store.mutate(delete)

    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 1, 1),
                                  (POLLIN, 2, 2)]),
              FakeClient(on_send=on_send))
    report = rig.run()
    assert rig.client.calls == [(1, 1, 1, 0), (1, 2, 2, 0)]
    assert statuses(report) == ["ok", "ok", "ok"]
    assert [r["recorded"] for r in report["results"]] == [True, False, True]
    assert "last_sent" not in report["results"][1]
    ids = [s["id"] for s in rig.doc()["rooms"][0]["sockets"]]
    assert ids == ["s1", "s3"]


def test_empty_room_and_empty_household(tmp_path):
    empty = {"ok": True, "results": [],
             "summary": {"ok": 0, "error": 0, "skipped": 0}}
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1)], []))
    assert rig.run("r2") == empty
    assert rig.client.calls == []
    assert rig.sleeps == []
    rig = Rig(tmp_path, make_doc(), name="none.json")
    assert rig.run() == empty
    assert rig.client.calls == []
    assert rig.sleeps == []


def test_unknown_room_is_404(tmp_path):
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1)]))
    with pytest.raises(ApiError) as info:
        rig.run("r9")
    assert info.value.status == 404
    assert info.value.code == "not_found"
    assert rig.client.calls == []


def test_report_shape(tmp_path):
    client = FakeClient({
        (1, 2, 2): DaemonReplyError("range", "group 2"),
        (1, 3, 3): DaemonUnreachable("refused"),
    })
    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2),
                                  (POLLIN, 3, 3), (POLLIN, 4, 4)]), client)
    report = rig.run()
    assert list(report) == ["ok", "results", "summary"]
    ok, err, dead, skipped = report["results"]
    assert list(ok) == ["socket_id", "room_id", "status", "recorded",
                        "last_sent"]
    assert list(ok["last_sent"]) == ["state", "at"]
    assert list(err) == ["socket_id", "room_id", "status", "error"]
    assert list(err["error"]) == ["code", "message", "daemon_class",
                                  "daemon_message"]
    assert list(dead) == ["socket_id", "room_id", "status", "error"]
    assert list(dead["error"]) == ["code", "message"]
    assert list(skipped) == ["socket_id", "room_id", "status", "error"]
    assert list(report["summary"]) == ["ok", "error", "skipped"]


def test_report_shape_of_an_unrecorded_ok(tmp_path):
    def on_send(n):
        rig.store.mutate(lambda doc: doc["rooms"][0]["sockets"].pop(0))

    rig = Rig(tmp_path, make_doc([(POLLIN, 1, 1)]),
              FakeClient(on_send=on_send))
    entry = rig.run()["results"][0]
    assert entry == {"socket_id": "s1", "room_id": "r1", "status": "ok",
                     "recorded": False}


# The API, against a fake daemon over TCP.

ROOMS = make_doc(
    [(POLLIN, 1, 1), (REV, 0, 1), (POLLIN, 1, 1)],
    [(POLLIN, 2, 2), (REV, 3, 2)],
)


@pytest.fixture
def api(fake_daemon, switch_app):
    """Return a factory: api(behaviour=None, doc=ROOMS, **cfg) -> (app, fake).

    The gap is 0 unless a test asks for another.
    """
    def factory(behaviour=None, doc=ROOMS, serial=False, **cfg):
        fake = fake_daemon(behaviour, serial)
        cfg.setdefault("all_off_gap_ms", 0)
        return switch_app(fake, doc, **cfg), fake

    return factory


def test_room_all_off_covers_only_that_room(api):
    app, fake = api()
    resp = post_json(app, "/api/rooms/r2/all-off")
    assert resp.status_code == 200
    body = resp.get_json()
    assert fake.lines == ["1 2 2 0", "0 3 2 0"]
    assert body["ok"] is True
    assert [r["socket_id"] for r in body["results"]] == ["s4", "s5"]
    assert body["summary"] == {"ok": 2, "error": 0, "skipped": 0}


def test_global_all_off_covers_every_room(api):
    app, fake = api()
    resp = post_json(app, "/api/all-off")
    assert resp.status_code == 200
    body = resp.get_json()
    assert fake.lines == ["1 1 1 0", "0 0 1 0", "1 2 2 0", "0 3 2 0"]
    assert fake.connections == 4
    assert [r["socket_id"] for r in body["results"]] == [
        "s1", "s2", "s3", "s4", "s5"]
    assert body["summary"]["ok"] == 5


def test_unknown_room_is_404_over_http(api):
    app, fake = api()
    resp = post_json(app, "/api/rooms/nothing/all-off")
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"
    assert fake.connections == 0


def test_unknown_room_is_404_before_the_fields_are_checked(api):
    app, fake = api()
    for body in ({"x": 1}, {"state": "on"}):
        resp = post_json(app, "/api/rooms/r_000000009999/all-off", body)
        assert resp.status_code == 404
        assert resp.get_json()["error"]["code"] == "not_found"
    assert fake.connections == 0


def test_a_body_that_is_not_an_object_is_400_before_the_room(api):
    app, fake = api()
    for path in ("/api/rooms/r_000000009999/all-off", "/api/rooms/r1/all-off",
                 "/api/all-off"):
        resp = app.test_client().post(
            path, data=b"[1]", headers={"Content-Type": "application/json"})
        assert resp.status_code == 400
        assert resp.get_json()["error"]["code"] == "bad_json"
    assert fake.connections == 0


def test_unknown_field_is_422_for_a_known_room_and_for_all(api):
    app, fake = api()
    for path in ("/api/rooms/r1/all-off", "/api/all-off"):
        resp = post_json(app, path, {"x": 1})
        assert resp.status_code == 422
        assert resp.get_json()["error"]["field"] == "x"
    assert fake.connections == 0


def test_all_off_needs_json_content_type(api):
    app, fake = api()
    client = app.test_client()
    for path in ("/api/all-off", "/api/rooms/r1/all-off"):
        resp = client.post(path)
        assert resp.status_code == 415
        assert resp.get_json()["error"]["code"] == "unsupported_media_type"
    assert fake.connections == 0
    resp = client.post("/api/all-off", content_type="application/json")
    assert resp.status_code == 200
    resp = post_json(app, "/api/all-off", {"x": 1})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "x"


def test_all_off_with_daemon_answers_err_is_200_with_ok_false(api):
    def behaviour(line):
        if line == "0 0 1 0":
            return b"ERR send transmission failed\n"
        return b"OK\n"

    app, fake = api(behaviour)
    resp = post_json(app, "/api/all-off")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is False
    assert body["summary"] == {"ok": 4, "error": 1, "skipped": 0}
    error = body["results"][1]["error"]
    assert error["daemon_class"] == "send"
    assert len(fake.lines) == 4


def test_dead_daemon_costs_one_connect(switch_app, monkeypatch):
    attempts = []
    real = daemon.socket.create_connection

    def counting(*args, **kwargs):
        attempts.append(args)
        return real(*args, **kwargs)

    monkeypatch.setattr(daemon.socket, "create_connection", counting)
    doc = make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2), (POLLIN, 3, 3)],
                   [(POLLIN, 4, 4), (POLLIN, 5, 5)])
    app = switch_app(refused_port(), doc, all_off_gap_ms=0,
                     daemon_connect_timeout=0.5)
    start = time.monotonic()
    resp = post_json(app, "/api/all-off")
    assert time.monotonic() - start < 0.5 + 1.0
    assert resp.status_code == 200
    body = resp.get_json()
    assert len(attempts) == 1
    assert body["ok"] is False
    assert body["summary"] == {"ok": 0, "error": 1, "skipped": 4}
    assert body["results"][0]["error"]["code"] == "daemon_unreachable"


def test_single_switch_is_served_during_the_gap(api):
    doc = make_doc([(POLLIN, 1, 1), (POLLIN, 2, 2), (POLLIN, 3, 3)],
                   [(POLLIN, 9, 9)])
    # The gap is long, so that the single switch is served inside it
    # even when the machine is slow. The switch is sent as soon as the
    # first command of the All-off has arrived.
    read_timeout = 1.0
    app, fake = api(doc=doc, serial=True, all_off_gap_ms=1500,
                    daemon_read_timeout=read_timeout)
    outcome = {}

    def run_all_off():
        outcome["resp"] = post_json(app, "/api/rooms/r1/all-off")

    worker = threading.Thread(target=run_all_off)
    worker.start()
    deadline = time.monotonic() + 5
    while not fake.lines and time.monotonic() < deadline:
        time.sleep(0.005)
    assert fake.lines
    start = time.monotonic()
    resp = post_json(app, "/api/sockets/s4/switch", {"state": "on"})
    took = time.monotonic() - start
    worker.join(15)
    assert not worker.is_alive()
    assert resp.status_code == 200
    assert took < read_timeout + 1.0
    assert outcome["resp"].status_code == 200
    assert outcome["resp"].get_json()["ok"] is True
    assert "1 9 9 1" in fake.lines
    at = fake.lines.index("1 9 9 1")
    assert 0 < at < len(fake.lines) - 1
    assert fake.lines.count("1 1 1 0") == 1
    assert len(fake.lines) == 4
    assert fake.connections == 4


def test_a_serial_fake_blocks_while_a_connection_is_held(api):
    """The fake serves one connection at a time, like rsswitchd.

    This is what gives test_single_switch_is_served_during_the_gap its
    meaning: a connection held open would make the switch time out.
    """
    app, fake = api(serial=True, daemon_read_timeout=0.4)
    held = socket.create_connection(("127.0.0.1", fake.port))
    try:
        deadline = time.monotonic() + 2
        while fake.connections < 1 and time.monotonic() < deadline:
            time.sleep(0.005)
        resp = post_json(app, "/api/sockets/s1/switch", {"state": "on"})
        assert resp.status_code == 504
    finally:
        held.close()


def test_store_failure_does_not_stop_the_batch(tmp_path, caplog):
    rig = Rig(tmp_path, make_doc([(REV, 0, 0), (POLLIN, 1, 1)],
                                 [(POLLIN, 2, 2)]))

    def refuse(data):
        raise StoreError("data file %s: cannot be written: disk full"
                         % rig.path)

    rig.store._write = refuse
    before = rig.doc()
    report = rig.run()
    assert rig.client.calls == [(0, 0, 0, 0), (1, 1, 1, 0), (1, 2, 2, 0)]
    assert report["ok"] is True
    assert report["summary"] == {"ok": 3, "error": 0, "skipped": 0}
    assert report["results"] == [
        {"socket_id": "s1", "room_id": "r1", "status": "ok",
         "recorded": False},
        {"socket_id": "s2", "room_id": "r1", "status": "ok",
         "recorded": False},
        {"socket_id": "s3", "room_id": "r2", "status": "ok",
         "recorded": False},
    ]
    assert rig.doc() == before
    assert "cannot be written" in caplog.text
