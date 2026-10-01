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

"""Tests of POST /api/sockets/<id>/switch against a fake daemon."""

from __future__ import annotations

import json
import re
import time

import pytest
from conftest import (CLOSE, HANG, RESET, post_json, refused_port,
                      sample_doc)

from rsswitch_web.store import Store, StoreError

REV = "s_000000000001"
POLLIN = "s_000000000002"
EARLY = "2026-09-29T08:00:00.000Z"
LATE = "2026-09-29T10:00:00.000Z"


def data_path(app):
    return app.config["RSSWITCH_WEB"]["data_file"]


def file_bytes(app):
    with open(data_path(app), "rb") as f:
        return f.read()


def file_doc(app):
    return json.loads(file_bytes(app).decode("utf-8"))


def socket_of(doc, sid):
    for room in doc["rooms"]:
        for sock in room["sockets"]:
            if sock["id"] == sid:
                return sock
    return None


def set_clock(app, at):
    app.extensions["rsswitch_web"].clock = lambda: at


def switch(app, sid, state):
    return post_json(app, "/api/sockets/%s/switch" % sid, {"state": state})


def doc_with(sid, **changes):
    """Return sample_doc() with the fields of one socket changed."""
    doc = sample_doc()
    socket_of(doc, sid).update(changes)
    return doc


def test_switch_pollin_17_23_on_reaches_daemon_and_records_last_sent(
        fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake, doc_with(POLLIN, last_sent=None))
    resp = switch(app, POLLIN, "on")
    assert resp.status_code == 200
    body = resp.get_json()
    assert fake.lines == ["1 17 23 1"]
    assert fake.connections == 1
    assert body["ok"] is True
    assert body["recorded"] is True
    last_sent = body["socket"]["last_sent"]
    assert last_sent["state"] == "on"
    assert body["socket"]["id"] == POLLIN
    assert body["socket"]["group"] == 17
    assert body["socket"]["number"] == 23
    assert socket_of(file_doc(app), POLLIN)["last_sent"] == last_sent


def test_switch_rev_3_2_off_sends_data_0(fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake, doc_with(REV, group=3, number=2))
    set_clock(app, EARLY)
    resp = switch(app, REV, "off")
    assert resp.status_code == 200
    assert fake.lines == ["0 3 2 0"]
    assert resp.get_json()["socket"]["last_sent"] == {"state": "off",
                                                      "at": EARLY}


def test_last_sent_at_is_utc_milliseconds(fake_daemon, switch_app):
    app = switch_app(fake_daemon())
    at = switch(app, REV, "on").get_json()["socket"]["last_sent"]["at"]
    assert re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$", at)


def test_unknown_socket_is_404_without_daemon_contact(fake_daemon,
                                                      switch_app):
    fake = fake_daemon()
    app = switch_app(fake)
    before = file_bytes(app)
    resp = switch(app, "s_nothing", "on")
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"
    assert fake.connections == 0
    assert file_bytes(app) == before


@pytest.mark.parametrize("body", [{"state": "x"}, {"state": "on", "x": 1},
                                  {}, {"state": 1}])
def test_unknown_socket_is_404_before_the_fields_are_checked(
        fake_daemon, switch_app, body):
    fake = fake_daemon()
    app = switch_app(fake)
    before = file_bytes(app)
    resp = post_json(app, "/api/sockets/s_000000009999/switch", body)
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"
    assert fake.connections == 0
    assert file_bytes(app) == before


@pytest.mark.parametrize("raw", [b"", b"nope", b"[1]", b'"on"', b"null"])
def test_a_body_that_is_not_an_object_is_400_before_the_id(
        fake_daemon, switch_app, raw):
    fake = fake_daemon()
    app = switch_app(fake)
    for sid in ("s_000000009999", REV):
        resp = app.test_client().post(
            "/api/sockets/%s/switch" % sid, data=raw,
            headers={"Content-Type": "application/json"})
        assert resp.status_code == 400
        assert resp.get_json()["error"]["code"] == "bad_json"
    assert fake.connections == 0


def test_known_socket_with_a_bad_state_is_still_422(fake_daemon,
                                                    switch_app):
    fake = fake_daemon()
    app = switch_app(fake)
    resp = post_json(app, "/api/sockets/%s/switch" % REV, {"state": "x"})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "state"
    assert fake.connections == 0


MISSING = object()


@pytest.mark.parametrize("wanted", ["ON", "", 1, True, None, MISSING,
                                    "on\n", ["on"]])
def test_state_validation(fake_daemon, switch_app, wanted):
    fake = fake_daemon()
    app = switch_app(fake)
    before = file_bytes(app)
    body = {} if wanted is MISSING else {"state": wanted}
    resp = post_json(app, "/api/sockets/%s/switch" % REV, body)
    assert resp.status_code == 422
    error = resp.get_json()["error"]
    assert error["code"] == "validation"
    assert error["field"] == "state"
    assert fake.connections == 0
    assert file_bytes(app) == before


def test_extra_field_is_422_and_daemon_untouched(fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake)
    resp = post_json(app, "/api/sockets/%s/switch" % REV,
                     {"state": "on", "group": 1})
    assert resp.status_code == 422
    assert resp.get_json()["error"]["field"] == "group"
    assert fake.connections == 0


def test_refused_daemon_is_502_daemon_unreachable_and_file_unchanged(
        switch_app):
    app = switch_app(refused_port())
    before = file_bytes(app)
    resp = switch(app, REV, "on")
    assert resp.status_code == 502
    assert resp.get_json()["error"]["code"] == "daemon_unreachable"
    assert file_bytes(app) == before
    assert socket_of(file_doc(app), REV)["last_sent"] is None


def test_hung_daemon_is_504_daemon_timeout_within_budget(fake_daemon,
                                                         switch_app):
    read_timeout = 0.4
    app = switch_app(fake_daemon(lambda line: HANG),
                     daemon_read_timeout=read_timeout)
    before = file_bytes(app)
    start = time.monotonic()
    resp = switch(app, POLLIN, "off")
    assert time.monotonic() - start < read_timeout + 1.0
    assert resp.status_code == 504
    assert resp.get_json()["error"]["code"] == "daemon_timeout"
    assert file_bytes(app) == before


def test_err_reply_is_502_daemon_error_with_class_and_message(fake_daemon,
                                                              switch_app):
    fake = fake_daemon(lambda line: b"ERR range group 40 not in 0-31\n")
    app = switch_app(fake)
    before = file_bytes(app)
    resp = switch(app, POLLIN, "on")
    assert resp.status_code == 502
    error = resp.get_json()["error"]
    assert error["code"] == "daemon_error"
    assert error["daemon_class"] == "range"
    assert error["daemon_message"] == "group 40 not in 0-31"
    assert file_bytes(app) == before


@pytest.mark.parametrize("reply", [b"WAT\n", CLOSE, RESET, b"x" * 300])
def test_garbage_eof_reset_are_502_daemon_protocol(fake_daemon, switch_app,
                                                   reply):
    app = switch_app(fake_daemon(lambda line: reply))
    before = file_bytes(app)
    resp = switch(app, POLLIN, "on")
    assert resp.status_code == 502
    assert resp.get_json()["error"]["code"] == "daemon_protocol"
    assert file_bytes(app) == before


def edit_during_send(fake_daemon, switch_app, change, **overrides):
    """Switch POLLIN on while change(doc) edits the file mid-send."""
    holder = {}

    def behaviour(line):
        holder["store"].mutate(change)
        return b"OK\n"

    fake = fake_daemon(behaviour)
    app = switch_app(fake, **overrides)
    holder["store"] = Store(data_path(app))
    set_clock(app, LATE)
    return app, switch(app, POLLIN, "off")


def test_socket_deleted_during_send_is_ok_not_recorded(fake_daemon,
                                                       switch_app):
    def delete(doc):
        doc["rooms"][0]["sockets"].pop(1)

    app, resp = edit_during_send(fake_daemon, switch_app, delete)
    assert resp.status_code == 200
    assert resp.get_json() == {"ok": True, "recorded": False,
                               "socket": None}
    assert socket_of(file_doc(app), POLLIN) is None


def test_address_changed_during_send_is_ok_not_recorded(fake_daemon,
                                                        switch_app):
    def move(doc):
        doc["rooms"][0]["sockets"][1]["group"] = 5

    app, resp = edit_during_send(fake_daemon, switch_app, move)
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is True
    assert body["recorded"] is False
    assert body["socket"]["group"] == 5
    assert body["socket"]["last_sent"] == {"state": "on", "at": EARLY}
    assert socket_of(file_doc(app), POLLIN)["last_sent"] == {
        "state": "on", "at": EARLY}


def test_rename_during_send_keeps_rename_and_records(fake_daemon,
                                                     switch_app):
    def rename(doc):
        doc["rooms"][0]["sockets"][1]["name"] = "Neu"

    app, resp = edit_during_send(fake_daemon, switch_app, rename)
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["recorded"] is True
    assert body["socket"]["name"] == "Neu"
    assert body["socket"]["last_sent"] == {"state": "off", "at": LATE}
    assert socket_of(file_doc(app), POLLIN)["name"] == "Neu"


def test_unrelated_edit_during_send_is_not_lost(fake_daemon, switch_app):
    def rename_room(doc):
        doc["rooms"][1]["name"] = "Bad"

    app, resp = edit_during_send(fake_daemon, switch_app, rename_room)
    assert resp.status_code == 200
    assert resp.get_json()["recorded"] is True
    doc = file_doc(app)
    assert doc["rooms"][1]["name"] == "Bad"
    assert socket_of(doc, POLLIN)["last_sent"] == {"state": "off",
                                                   "at": LATE}


def test_older_timestamp_does_not_overwrite_newer(fake_daemon, switch_app):
    newer = {"state": "on", "at": "2026-09-29T08:00:00.500Z"}
    app = switch_app(fake_daemon(), doc_with(POLLIN, last_sent=newer))
    set_clock(app, EARLY)
    before = file_bytes(app)
    resp = switch(app, POLLIN, "off")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is True
    assert body["recorded"] is False
    assert body["socket"]["last_sent"] == newer
    assert file_bytes(app) == before


def test_equal_timestamp_overwrites(fake_daemon, switch_app):
    app = switch_app(fake_daemon(), doc_with(
        POLLIN, last_sent={"state": "on", "at": LATE}))
    set_clock(app, LATE)
    body = switch(app, POLLIN, "off").get_json()
    assert body["recorded"] is True
    assert body["socket"]["last_sent"] == {"state": "off", "at": LATE}
    assert socket_of(file_doc(app), POLLIN)["last_sent"] == {
        "state": "off", "at": LATE}


def test_record_last_sent_for_missing_ids_writes_nothing(make_app):
    app = make_app(sample_doc())
    before = file_bytes(app)
    store = Store(data_path(app))
    written = store.record_last_sent(["s_gone"], ("rev_008345", 0, 0),
                                     "on", LATE)
    assert written == []
    assert file_bytes(app) == before


def fail_writes(app):
    """Make every write of the data file of app fail, as a full disk does."""
    def refuse(data):
        raise StoreError("data file %s: cannot be written: disk full"
                         % data_path(app))

    app.extensions["rsswitch_web"].store._write = refuse


def test_store_failure_after_ok_is_still_a_success(fake_daemon, switch_app,
                                                   caplog):
    fake = fake_daemon()
    app = switch_app(fake)
    fail_writes(app)
    before = file_bytes(app)
    resp = switch(app, POLLIN, "off")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is True
    assert body["recorded"] is False
    assert body["socket"]["id"] == POLLIN
    assert body["socket"]["last_sent"] == {"state": "on", "at": EARLY}
    assert fake.lines == ["1 17 23 0"]
    assert file_bytes(app) == before
    assert "cannot be written" in caplog.text


def test_store_unreadable_after_ok_is_still_a_success(fake_daemon,
                                                      switch_app):
    fake = fake_daemon()
    app = switch_app(fake)
    store = app.extensions["rsswitch_web"].store
    real = store.snapshot

    def snapshot():
        # Readable until rsswitchd has been sent the command.
        if fake.lines:
            raise StoreError("data file: cannot be read")
        return real()

    store.snapshot = snapshot
    resp = switch(app, POLLIN, "off")
    assert resp.status_code == 200
    assert resp.get_json()["ok"] is True
    assert resp.get_json()["socket"] is None
    assert fake.lines == ["1 17 23 0"]


def test_timestamp_from_a_clock_that_went_back_is_overwritten(
        fake_daemon, switch_app, caplog):
    future = {"state": "on", "at": "2100-01-01T00:00:00.000Z"}
    app = switch_app(fake_daemon(), doc_with(POLLIN, last_sent=future))
    set_clock(app, LATE)
    body = switch(app, POLLIN, "off").get_json()
    assert body["recorded"] is True
    assert body["socket"]["last_sent"] == {"state": "off", "at": LATE}
    assert socket_of(file_doc(app), POLLIN)["last_sent"] == {
        "state": "off", "at": LATE}
    assert "clock went back" in caplog.text


def test_timestamp_just_outside_the_race_window_is_overwritten(
        fake_daemon, switch_app):
    stored = {"state": "on", "at": "2026-09-29T10:00:02.001Z"}
    app = switch_app(fake_daemon(), doc_with(POLLIN, last_sent=stored))
    set_clock(app, LATE)
    assert switch(app, POLLIN, "off").get_json()["recorded"] is True


def test_timestamp_at_the_edge_of_the_race_window_is_kept(fake_daemon,
                                                          switch_app):
    stored = {"state": "on", "at": "2026-09-29T10:00:02.000Z"}
    app = switch_app(fake_daemon(), doc_with(POLLIN, last_sent=stored))
    set_clock(app, LATE)
    body = switch(app, POLLIN, "off").get_json()
    assert body["recorded"] is False
    assert body["socket"]["last_sent"] == stored


def test_unparsable_stored_time_is_overwritten(make_app):
    doc = doc_with(POLLIN, last_sent={"state": "on", "at": "yesterday"})
    app = make_app(doc)
    store = Store(data_path(app))
    written = store.record_last_sent([POLLIN], ("pollin_2605", 17, 23),
                                     "off", LATE)
    assert written == [POLLIN]
    assert socket_of(file_doc(app), POLLIN)["last_sent"] == {
        "state": "off", "at": LATE}
