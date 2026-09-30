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

"""Tests of the locked, atomic JSON store."""

from __future__ import annotations

import copy
import errno
import multiprocessing
import os
import stat
import threading
import time

import pytest
from conftest import write_doc
from filelock import FileLock

from rsswitch_web import store as store_module
from rsswitch_web.model import empty_document
from rsswitch_web.store import Store, StoreError


def incr(data):
    """Count in the name of the first room, creating it when needed."""
    rooms = data["rooms"]
    if not rooms:
        rooms.append({"id": "r_counter", "name": "0", "sockets": []})
    rooms[0]["name"] = str(int(rooms[0]["name"]) + 1)


def counter(store):
    return int(store.snapshot()["rooms"][0]["name"])


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def temp_files(directory):
    return [n for n in os.listdir(str(directory)) if n.endswith(".tmp")]


def worker(path, n):
    """Increment the counter n times. Runs in a spawned process."""
    store = Store(path, lock_timeout=60)
    for _ in range(n):
        store.mutate(incr)


def test_missing_file_reads_empty_document(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))
    assert store.snapshot() == {"version": 1, "rooms": []}
    assert store.snapshot() == empty_document()
    assert not path.exists()


def test_mutate_round_trip_and_returns_result(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))

    def add_room(data):
        data["rooms"].append({"id": "r_1", "name": "Küche",
                              "sockets": []})
        return "added"

    assert store.mutate(add_room) == "added"
    expected = {"version": 1,
                "rooms": [{"id": "r_1", "name": "Küche",
                           "sockets": []}]}
    assert store.snapshot() == expected
    assert Store(str(path)).snapshot() == expected
    raw = read_bytes(path)
    assert raw.endswith(b"\n")
    assert "Küche".encode("utf-8") in raw
    assert b"\n  " in raw


def test_mutate_raising_writes_nothing(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))

    def boom(data):
        data["rooms"].append({"id": "x"})
        raise RuntimeError("abort")

    with pytest.raises(RuntimeError):
        store.mutate(boom)
    assert not path.exists()

    store.mutate(incr)
    before = read_bytes(path)
    with pytest.raises(RuntimeError):
        store.mutate(boom)
    assert read_bytes(path) == before
    assert temp_files(tmp_path) == []


def test_unchanged_document_is_not_written(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))
    store.mutate(lambda data: None)
    assert not path.exists()

    store.mutate(incr)
    before = read_bytes(path)
    mtime = os.stat(str(path)).st_mtime_ns
    store.mutate(lambda data: None)
    assert read_bytes(path) == before
    assert os.stat(str(path)).st_mtime_ns == mtime
    assert temp_files(tmp_path) == []


def test_written_file_mode_is_0600(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))
    store.mutate(incr)
    assert stat.S_IMODE(os.stat(str(path)).st_mode) == 0o600
    store.mutate(incr)
    assert stat.S_IMODE(os.stat(str(path)).st_mode) == 0o600


def test_directory_is_fsynced(tmp_path, monkeypatch):
    store = Store(str(tmp_path / "data.json"))
    kinds = []
    real_fsync = os.fsync

    def counting_fsync(fd):
        kinds.append("dir" if stat.S_ISDIR(os.fstat(fd).st_mode)
                     else "file")
        real_fsync(fd)

    monkeypatch.setattr(store_module.os, "fsync", counting_fsync)
    store.mutate(incr)
    assert len(kinds) >= 2
    assert kinds[0] == "file"
    assert "dir" in kinds


def test_replace_failure_keeps_old_file_and_removes_temp(tmp_path,
                                                         monkeypatch):
    path = tmp_path / "data.json"
    store = Store(str(path))
    store.mutate(incr)
    before = read_bytes(path)

    def failing_replace(src, dst):
        raise OSError(errno.EIO, "injected failure")

    monkeypatch.setattr(store_module.os, "replace", failing_replace)
    with pytest.raises(StoreError):
        store.mutate(incr)
    assert read_bytes(path) == before
    assert temp_files(tmp_path) == []
    monkeypatch.undo()
    store.mutate(incr)
    assert counter(store) == 2


def test_lock_timeout_raises_store_error(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path), lock_timeout=0.2)
    with FileLock(str(path) + ".lock"):
        started = time.monotonic()
        with pytest.raises(StoreError, match="locked"):
            store.mutate(incr)
        assert time.monotonic() - started < 5
        assert not path.exists()
    store.mutate(incr)
    assert counter(store) == 1


@pytest.mark.parametrize("shared", [True, False],
                         ids=["shared-store", "store-per-thread"])
def test_threads_lose_no_update(tmp_path, shared):
    path = str(tmp_path / "data.json")
    shared_store = Store(path, lock_timeout=60)
    errors = []

    def work():
        try:
            store = shared_store if shared else Store(path,
                                                      lock_timeout=60)
            for _ in range(25):
                store.mutate(incr)
        except BaseException as e:
            errors.append(e)

    threads = [threading.Thread(target=work) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    assert counter(shared_store) == 200


def test_processes_lose_no_update(tmp_path):
    path = str(tmp_path / "data.json")
    ctx = multiprocessing.get_context("spawn")
    procs = [ctx.Process(target=worker, args=(path, 25))
             for _ in range(2)]
    for p in procs:
        p.start()
    for p in procs:
        p.join(120)
    assert [p.exitcode for p in procs] == [0, 0]
    assert counter(Store(path)) == 50


@pytest.mark.parametrize("content", [
    b"{",
    b"",
    b"\xff\xfe",
    b"NaN",
    b'{"version": 1, "rooms": [], "x": NaN}',
    b'{"version": 1, "rooms": [], "x": Infinity}',
    b"[]",
    b'{"rooms": []}',
    b'{"version": 2, "rooms": []}',
    b'{"version": true, "rooms": []}',
    b'{"version": "1", "rooms": []}',
    b'{"version": 1.0, "rooms": []}',
    b'{"version": 1, "rooms": {}}',
    b'{"version": 1}',
], ids=["truncated", "empty", "not-utf8", "nan-document",
        "nan-value", "infinity-value", "list", "no-version",
        "version-2", "version-true", "version-string", "version-float",
        "rooms-not-a-list", "no-rooms"])
def test_not_json_version_and_shape_rejected(tmp_path, content):
    path = tmp_path / "data.json"
    path.write_bytes(content)
    store = Store(str(path))
    with pytest.raises(StoreError):
        store.snapshot()
    with pytest.raises(StoreError):
        store.check()
    with pytest.raises(StoreError):
        store.mutate(incr)
    assert read_bytes(path) == content
    assert temp_files(tmp_path) == []


def test_deeply_nested_file_is_rejected_not_crashed(tmp_path):
    path = tmp_path / "data.json"
    content = b"[" * 5000
    path.write_bytes(content)
    with pytest.raises(StoreError):
        Store(str(path)).snapshot()
    assert read_bytes(path) == content


def test_check_never_writes(tmp_path):
    path = tmp_path / "data.json"
    store = Store(str(path))
    store.check()
    assert sorted(os.listdir(str(tmp_path))) == []

    store.mutate(incr)
    before = read_bytes(path)
    mtime = os.stat(str(path)).st_mtime_ns
    listing = sorted(os.listdir(str(tmp_path)))
    store.check()
    store.snapshot()
    assert read_bytes(path) == before
    assert os.stat(str(path)).st_mtime_ns == mtime
    assert sorted(os.listdir(str(tmp_path))) == listing


def test_check_rejects_missing_parent_dir(tmp_path):
    store = Store(str(tmp_path / "nodir" / "data.json"))
    with pytest.raises(StoreError):
        store.check()


def test_snapshot_rejects_missing_parent_dir(tmp_path):
    store = Store(str(tmp_path / "nodir" / "data.json"))
    with pytest.raises(StoreError, match="does not exist"):
        store.snapshot()
    assert not (tmp_path / "nodir").exists()


def test_mutate_missing_parent_dir_creates_nothing(tmp_path):
    store = Store(str(tmp_path / "nodir" / "data.json"))
    with pytest.raises(StoreError):
        store.mutate(incr)
    assert not (tmp_path / "nodir").exists()


def base_doc():
    """A valid document with two rooms and three sockets."""
    return {
        "version": 1,
        "rooms": [
            {"id": "r1", "name": "One", "sockets": [
                {"id": "s1", "name": "A", "type": "rev_008345",
                 "group": 3, "number": 2, "last_sent": None},
                {"id": "s2", "name": "B", "type": "pollin_2605",
                 "group": 17, "number": 23,
                 "last_sent": {"state": "on", "at": "2026-09-29T08:00Z"}},
            ]},
            {"id": "r2", "name": "Two", "sockets": [
                {"id": "s3", "name": "C", "type": "pollin_2605",
                 "group": 31, "number": 31, "last_sent": None},
            ]},
        ],
    }


def set_at(path, value):
    """Return an edit that stores value at the given path of keys."""
    def edit(doc):
        node = doc
        for key in path[:-1]:
            node = node[key]
        node[path[-1]] = value
    return edit


def drop_at(path):
    def edit(doc):
        node = doc
        for key in path[:-1]:
            node = node[key]
        del node[path[-1]]
    return edit


S0 = ("rooms", 0, "sockets", 0)
S1 = ("rooms", 0, "sockets", 1)

MALFORMED = [
    ("unknown-top-level-key", set_at(("extra",), 1), "extra"),
    ("room-not-an-object", set_at(("rooms", 1), "x"), "rooms[1]"),
    ("room-without-sockets", drop_at(("rooms", 1, "sockets")),
     "rooms[1].sockets"),
    ("sockets-not-a-list", set_at(("rooms", 1, "sockets"), {}),
     "rooms[1].sockets"),
    ("unknown-room-key", set_at(("rooms", 0, "color"), "red"),
     "rooms[0].color"),
    ("room-id-int", set_at(("rooms", 0, "id"), 7), "rooms[0].id"),
    ("room-id-space", set_at(("rooms", 0, "id"), "a b"), "rooms[0].id"),
    ("room-id-newline", set_at(("rooms", 0, "id"), "r1\n"),
     "rooms[0].id"),
    ("room-id-empty", set_at(("rooms", 0, "id"), ""), "rooms[0].id"),
    ("room-id-65-chars", set_at(("rooms", 0, "id"), "x" * 65),
     "rooms[0].id"),
    ("duplicate-room-id", set_at(("rooms", 1, "id"), "r1"),
     "rooms[1].id"),
    ("socket-id-equals-room-id", set_at(S1 + ("id",), "r1"),
     "rooms[0].sockets[1].id"),
    ("duplicate-socket-id", set_at(("rooms", 1, "sockets", 0, "id"), "s1"),
     "rooms[1].sockets[0].id"),
    ("room-name-int", set_at(("rooms", 0, "name"), 1), "rooms[0].name"),
    ("socket-name-null", set_at(S0 + ("name",), None),
     "rooms[0].sockets[0].name"),
    ("socket-not-an-object", set_at(S0, []), "rooms[0].sockets[0]"),
    ("unknown-socket-key", set_at(S0 + ("color",), "red"),
     "rooms[0].sockets[0].color"),
    ("socket-without-last-sent", drop_at(S0 + ("last_sent",)),
     "rooms[0].sockets[0].last_sent"),
    ("type-upper-case", set_at(S0 + ("type",), "REV_008345"),
     "rooms[0].sockets[0].type"),
    ("type-list", set_at(S0 + ("type",), ["rev_008345"]),
     "rooms[0].sockets[0].type"),
    ("group-string", set_at(S0 + ("group",), "3"),
     "rooms[0].sockets[0].group"),
    ("group-true", set_at(S0 + ("group",), True),
     "rooms[0].sockets[0].group"),
    ("group-float", set_at(S0 + ("group",), 3.0),
     "rooms[0].sockets[0].group"),
    ("group-negative", set_at(S0 + ("group",), -1),
     "rooms[0].sockets[0].group"),
    ("number-true", set_at(S0 + ("number",), True),
     "rooms[0].sockets[0].number"),
    ("rev-group-4", set_at(S0 + ("group",), 4),
     "rooms[0].sockets[0].group"),
    ("rev-number-3", set_at(S0 + ("number",), 3),
     "rooms[0].sockets[0].number"),
    ("pollin-group-32", set_at(S1 + ("group",), 32),
     "rooms[0].sockets[1].group"),
    ("pollin-number-32", set_at(S1 + ("number",), 32),
     "rooms[0].sockets[1].number"),
    ("last-sent-string", set_at(S1 + ("last_sent",), "on"),
     "rooms[0].sockets[1].last_sent"),
    ("last-sent-state-upper", set_at(S1 + ("last_sent",),
                                     {"state": "ON", "at": "x"}),
     "rooms[0].sockets[1].last_sent.state"),
    ("last-sent-state-int", set_at(S1 + ("last_sent", "state"), 1),
     "rooms[0].sockets[1].last_sent.state"),
    ("last-sent-without-at", drop_at(S1 + ("last_sent", "at")),
     "rooms[0].sockets[1].last_sent.at"),
    ("last-sent-at-int", set_at(S1 + ("last_sent", "at"), 5),
     "rooms[0].sockets[1].last_sent.at"),
    ("last-sent-unknown-key", set_at(S1 + ("last_sent", "by"), "me"),
     "rooms[0].sockets[1].last_sent.by"),
]


@pytest.mark.parametrize("edit, fragment",
                         [(c[1], c[2]) for c in MALFORMED],
                         ids=[c[0] for c in MALFORMED])
def test_malformed_document_rejected(tmp_path, edit, fragment):
    doc = copy.deepcopy(base_doc())
    edit(doc)
    path = tmp_path / "data.json"
    write_doc(path, doc)
    before = read_bytes(path)
    store = Store(str(path))
    with pytest.raises(StoreError) as info:
        store.snapshot()
    assert fragment in str(info.value)
    with pytest.raises(StoreError):
        store.check()
    with pytest.raises(StoreError):
        store.mutate(incr)
    assert read_bytes(path) == before
    assert temp_files(tmp_path) == []


def test_valid_base_document_is_accepted(tmp_path):
    path = tmp_path / "data.json"
    write_doc(path, base_doc())
    assert Store(str(path)).snapshot() == base_doc()


def test_valid_edge_document_accepted(tmp_path):
    doc = base_doc()
    # Duplicate names, an empty name and a long name are not checked on
    # load, and neither are duplicate addresses.
    doc["rooms"][0]["sockets"][1]["name"] = "A"
    doc["rooms"][1]["name"] = doc["rooms"][0]["name"]
    doc["rooms"][1]["sockets"][0]["name"] = ""
    doc["rooms"][1]["sockets"].append(
        {"id": "s4", "name": "x" * 500, "type": "rev_008345", "group": 3,
         "number": 2, "last_sent": {"state": "off", "at": ""}})
    path = tmp_path / "data.json"
    write_doc(path, doc)
    store = Store(str(path))
    assert store.snapshot() == doc
    store.check()


def test_mutate_on_malformed_file_writes_nothing(tmp_path):
    doc = base_doc()
    doc["rooms"][0]["sockets"][0]["group"] = "3"
    path = tmp_path / "data.json"
    write_doc(path, doc)
    before = read_bytes(path)
    mtime = os.stat(str(path)).st_mtime_ns
    with pytest.raises(StoreError):
        Store(str(path)).mutate(incr)
    assert read_bytes(path) == before
    assert os.stat(str(path)).st_mtime_ns == mtime
    assert temp_files(tmp_path) == []
