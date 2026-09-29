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

"""Switching sockets: what a request does between the store and rsswitchd.

The store lock is never held while the daemon is talked to. A command
takes as long as the radio needs, and a lock held for that time would
stall every other change of the data file. The price is that the
document may change in the meantime, so what is recorded afterwards is
checked again under the lock (see Store.record_last_sent).
"""

from __future__ import annotations

import time
from collections import OrderedDict
from datetime import datetime, timezone

from .daemon import DaemonError, DaemonTimeout, DaemonUnreachable
from .errors import ApiError, daemon_error_info
from .model import SOCKET_TYPES
from .store import find_room, find_socket, socket_json


def utc_now() -> str:
    """Return the current UTC time as 2026-09-29T08:23:04.273Z."""
    now = datetime.now(timezone.utc)
    return now.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def switch_socket(store, client, sid, state, clock=utc_now) -> dict:
    """Switch the socket sid to state ("on" or "off").

    Raise ApiError 404 for an unknown id, and let a DaemonError of the
    client through. In both cases nothing is recorded: last_sent is
    only ever written for a command that rsswitchd answered with OK.

    Return {"ok": True, "recorded": bool, "socket": dict or None}.
    recorded is False when the command went out but the socket was
    deleted or given another address in the meantime, or a newer
    command was already confirmed. socket is the record as it is now.
    """
    _, sock = find_socket(store.snapshot(), sid)
    if sock is None:
        raise ApiError(404, "not_found", "socket not found")
    address = (sock["type"], sock["group"], sock["number"])
    dev = SOCKET_TYPES[sock["type"]].dev
    client.send(dev, sock["group"], sock["number"],
                1 if state == "on" else 0)
    # After the OK, so that of two commands that finish close together
    # the one that was confirmed last is the one that is recorded.
    at = clock()
    written = store.record_last_sent([sid], address, state, at)
    room, sock = find_socket(store.snapshot(), sid)
    return {
        "ok": True,
        "recorded": bool(written),
        "socket": None if sock is None else socket_json(room, sock),
    }


def all_off(store, client, room_id=None, *, gap_seconds, sleep=time.sleep,
            clock=utc_now) -> dict:
    """Switch off every socket of a room, or of the whole household.

    One "off" command goes out for each distinct address (type, group
    and number) in display order, so sockets that share an address get
    one command and one shared outcome. Every command uses a
    connection of its own. Between two commands the function waits
    gap_seconds, with no connection open, so that a single switch can
    be served in the gap. There is no wait after the last command.

    The document is read once, at the start. A refused command or a
    protocol error is recorded and the next address is tried. A daemon
    that cannot be reached or that times out ends the batch, so a dead
    daemon costs one attempt, not one per socket: every address that
    is left is skipped and carries that error. last_sent is written
    for each address right after its OK, before the next command.

    Raise ApiError 404 for an unknown room_id. Return the report:
    {"ok": bool, "results": [...], "summary": {"ok", "error",
    "skipped"}}. results has one entry per socket in display order:
    {"socket_id", "room_id", "status": "ok", "recorded", "last_sent"}
    with last_sent only when it was recorded, or {"socket_id",
    "room_id", "status": "error" or "skipped", "error"}. ok is true
    when every socket has the status ok.
    """
    data = store.snapshot()
    if room_id is None:
        rooms = data["rooms"]
    else:
        room = find_room(data, room_id)
        if room is None:
            raise ApiError(404, "not_found", "room not found")
        rooms = [room]
    sockets = [(room["id"], sock) for room in rooms
               for sock in room["sockets"]]
    members = OrderedDict()
    for _, sock in sockets:
        address = (sock["type"], sock["group"], sock["number"])
        members.setdefault(address, []).append(sock["id"])

    outcome = {}
    stop_error = None
    sent = 0
    for address, ids in members.items():
        if stop_error is not None:
            for sid in ids:
                outcome[sid] = {"status": "skipped",
                                "error": dict(stop_error)}
            continue
        if sent:
            sleep(gap_seconds)
        sent += 1
        kind, group, number = address
        try:
            client.send(SOCKET_TYPES[kind].dev, group, number, 0)
        except DaemonError as e:
            info = daemon_error_info(e)
            for sid in ids:
                outcome[sid] = {"status": "error", "error": dict(info)}
            if isinstance(e, (DaemonUnreachable, DaemonTimeout)):
                stop_error = info
            continue
        at = clock()
        written = store.record_last_sent(ids, address, "off", at)
        for sid in ids:
            if sid in written:
                outcome[sid] = {"status": "ok", "recorded": True,
                                "last_sent": {"state": "off", "at": at}}
            else:
                outcome[sid] = {"status": "ok", "recorded": False}

    results = []
    summary = {"ok": 0, "error": 0, "skipped": 0}
    for room_of, sock in sockets:
        entry = {"socket_id": sock["id"], "room_id": room_of}
        entry.update(outcome[sock["id"]])
        summary[entry["status"]] += 1
        results.append(entry)
    return {"ok": summary["ok"] == len(results), "results": results,
            "summary": summary}
