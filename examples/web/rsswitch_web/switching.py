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

from datetime import datetime, timezone

from .errors import ApiError
from .model import SOCKET_TYPES
from .store import find_socket, socket_json


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
