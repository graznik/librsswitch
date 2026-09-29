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

"""The JSON data file: every change is one locked read-modify-write and
reaches the disk as a complete file put in place atomically."""

from __future__ import annotations

import json
import os
import tempfile

from filelock import FileLock, Timeout

from .model import DocumentError, check_document, empty_document


class StoreError(Exception):
    """The data file is unreadable, not what we wrote, or not writable.

    The message may name the path. It is meant for the server log and
    must never be sent to a client.
    """


def _reject_constant(name):
    raise ValueError("invalid JSON constant %s" % name)


class Store:
    """The household data file.

    Every change goes through mutate(). It takes an exclusive lock on
    <data file>.lock, so threads and processes cannot lose each other's
    updates, and it replaces the file atomically, so readers never see
    a half-written file and a crash never truncates it.
    """

    def __init__(self, path, lock_timeout=10.0):
        self.path = os.path.abspath(path)
        self.lock_timeout = lock_timeout
        self._lock = FileLock(self.path + ".lock", mode=0o600)

    def check(self):
        """Raise StoreError unless the file can be used. Never writes."""
        self._require_parent()
        self._load()

    def snapshot(self) -> dict:
        """Return the document without taking the lock.

        This is safe because every write puts a complete file in place
        with os.replace().
        """
        return self._load()

    def mutate(self, fn):
        """Run fn(document) under the lock and return its result.

        The file is written only when fn changed the document. An
        exception raised by fn leaves the file untouched.
        """
        self._require_parent()
        try:
            self._lock.acquire(timeout=self.lock_timeout,
                               poll_interval=0.01)
        except Timeout:
            raise StoreError("data file %s: locked for more than %s s"
                             % (self.path, self.lock_timeout))
        except OSError as e:
            raise StoreError("data file %s: cannot take the lock: %s"
                             % (self.path, e))
        try:
            data = self._load()
            before = json.dumps(data, sort_keys=True)
            result = fn(data)
            if json.dumps(data, sort_keys=True) != before:
                self._write(data)
            return result
        finally:
            self._lock.release()

    def _require_parent(self):
        parent = os.path.dirname(self.path)
        if not os.path.isdir(parent):
            raise StoreError("data directory %s does not exist" % parent)

    def _fail(self, reason):
        return StoreError("data file %s: %s" % (self.path, reason))

    def _load(self) -> dict:
        try:
            with open(self.path, "rb") as f:
                raw = f.read()
        except FileNotFoundError:
            return empty_document()
        except OSError as e:
            raise self._fail("cannot be read: %s" % e)
        try:
            data = json.loads(raw.decode("utf-8"),
                              parse_constant=_reject_constant)
        except (ValueError, RecursionError) as e:
            raise self._fail("is not UTF-8 JSON: %s" % e)
        try:
            check_document(data)
        except DocumentError as e:
            raise self._fail(str(e))
        return data

    def _write(self, data):
        parent = os.path.dirname(self.path)
        text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
        try:
            payload = text.encode("utf-8")
        except UnicodeEncodeError as e:
            raise self._fail("cannot be written: %s" % e)
        tmp = None
        try:
            fd, tmp = tempfile.mkstemp(dir=parent, prefix=".data.",
                                       suffix=".tmp")
            with os.fdopen(fd, "wb") as f:
                f.write(payload)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.path)
            # Make the rename itself durable.
            dfd = os.open(parent, os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
        except OSError as e:
            if tmp is not None:
                try:
                    os.unlink(tmp)
                except FileNotFoundError:
                    pass
            raise self._fail("cannot be written: %s" % e)


def socket_json(room, sock) -> dict:
    """Return a socket as the API shows it, with the id of its room."""
    return {
        "id": sock["id"],
        "room_id": room["id"],
        "name": sock["name"],
        "type": sock["type"],
        "group": sock["group"],
        "number": sock["number"],
        "last_sent": sock["last_sent"],
    }


def room_json(room) -> dict:
    """Return a room with all of its sockets, in their stored order."""
    return {
        "id": room["id"],
        "name": room["name"],
        "sockets": [socket_json(room, s) for s in room["sockets"]],
    }


def tree_json(data) -> dict:
    """Return the whole household, in the stored order of its rooms."""
    return {"rooms": [room_json(r) for r in data["rooms"]]}
