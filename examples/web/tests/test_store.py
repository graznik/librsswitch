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

import errno
import multiprocessing
import os
import stat
import threading
import time

import pytest
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


def test_mutate_missing_parent_dir_creates_nothing(tmp_path):
    store = Store(str(tmp_path / "nodir" / "data.json"))
    with pytest.raises(StoreError):
        store.mutate(incr)
    assert not (tmp_path / "nodir").exists()
