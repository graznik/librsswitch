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

"""The data file format: its version and the supported socket types."""

from __future__ import annotations

import re
from collections import namedtuple

# The version of the data file. A file with any other version is refused.
FORMAT_VERSION = 1

# dev is the value rsswitchd expects on the wire, groups and sockets are
# the number of groups and of sockets per group of that device.
SocketType = namedtuple("SocketType", "dev groups sockets")

# Mirrors enum socket_type (REV_008345 = 0, POLLIN_2605 = 1) and the
# limits reported by socket_addr_limits() in librsswitch.h.
SOCKET_TYPES = {
    "rev_008345": SocketType(0, 4, 3),
    "pollin_2605": SocketType(1, 32, 32),
}


def empty_document() -> dict:
    """Return the document of a household without any room."""
    return {"version": FORMAT_VERSION, "rooms": []}


# Ids are made by the server. They end up in URLs and log lines, so the
# alphabet is small. Match with ID_PATTERN.fullmatch().
ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,64}$")

_DOCUMENT_KEYS = ("version", "rooms")
_ROOM_KEYS = ("id", "name", "sockets")
_SOCKET_KEYS = ("id", "name", "type", "group", "number", "last_sent")
_LAST_SENT_KEYS = ("state", "at")


class DocumentError(ValueError):
    """The document is not a valid data file.

    The message starts with the JSON path of the first problem, for
    example "rooms[1].sockets[0].group: must be an integer 0-31".
    """


def is_strict_int(value) -> bool:
    """Tell whether value is a real integer.

    True, 3.0 and "3" are not: a hand edited file must not smuggle them
    past the address checks.
    """
    return type(value) is int


def address_problem(type_name, group, number):
    """Check a socket address against the limits of its type.

    Return None when it is valid, else a tuple (field, message) for the
    first field that is not. type_name must be a key of SOCKET_TYPES.
    """
    limits = SOCKET_TYPES[type_name]
    if not is_strict_int(group) or not 0 <= group < limits.groups:
        return ("group", "must be an integer 0-%d" % (limits.groups - 1))
    if not is_strict_int(number) or not 0 <= number < limits.sockets:
        return ("number",
                "must be an integer 0-%d" % (limits.sockets - 1))
    return None


def _join(path, key):
    return "%s.%s" % (path, key) if path else key


def _check_keys(obj, keys, path):
    """Accept only objects that have exactly the given keys."""
    for key in sorted(k for k in obj if k not in keys):
        raise DocumentError("%s: unknown key" % _join(path, key))
    for key in keys:
        if key not in obj:
            raise DocumentError("%s: missing" % _join(path, key))


def _check_id(value, path, seen):
    if not isinstance(value, str) or not ID_PATTERN.fullmatch(value):
        raise DocumentError("%s: must be 1-64 characters of A-Z a-z 0-9 "
                            "_ -" % path)
    if value in seen:
        raise DocumentError("%s: duplicate id '%s'" % (path, value))
    seen.add(value)


def _check_str(value, path):
    if not isinstance(value, str):
        raise DocumentError("%s: must be a string" % path)


def _check_last_sent(value, path):
    if value is None:
        return
    if not isinstance(value, dict):
        raise DocumentError("%s: must be null or an object" % path)
    _check_keys(value, _LAST_SENT_KEYS, path)
    if not isinstance(value["state"], str) or value["state"] not in (
            "on", "off"):
        raise DocumentError("%s: must be \"on\" or \"off\""
                            % _join(path, "state"))
    _check_str(value["at"], _join(path, "at"))


def _check_socket(sock, path, seen):
    if not isinstance(sock, dict):
        raise DocumentError("%s: must be an object" % path)
    _check_keys(sock, _SOCKET_KEYS, path)
    _check_id(sock["id"], _join(path, "id"), seen)
    _check_str(sock["name"], _join(path, "name"))
    if not isinstance(sock["type"], str) or sock["type"] not in SOCKET_TYPES:
        raise DocumentError("%s: must be one of %s"
                            % (_join(path, "type"), ", ".join(SOCKET_TYPES)))
    problem = address_problem(sock["type"], sock["group"], sock["number"])
    if problem is not None:
        raise DocumentError("%s: %s" % (_join(path, problem[0]), problem[1]))
    _check_last_sent(sock["last_sent"], _join(path, "last_sent"))


def check_document(doc) -> None:
    """Raise DocumentError unless doc is a valid data file.

    Every part of the document is checked in order and the first
    problem is reported. Unknown keys are refused instead of ignored:
    the API never writes them, so they mean a hand edit or a newer
    format, and a later rewrite would drop them silently. Names are
    not checked for uniqueness or length, so that a hand edit cannot
    make the app refuse to start.
    """
    if not isinstance(doc, dict):
        raise DocumentError("document: must be a JSON object")
    _check_keys(doc, _DOCUMENT_KEYS, "")
    if not is_strict_int(doc["version"]) or doc["version"] != FORMAT_VERSION:
        raise DocumentError("version: must be the integer %d"
                            % FORMAT_VERSION)
    rooms = doc["rooms"]
    if not isinstance(rooms, list):
        raise DocumentError("rooms: must be a list")
    seen = set()
    for i, room in enumerate(rooms):
        rpath = "rooms[%d]" % i
        if not isinstance(room, dict):
            raise DocumentError("%s: must be an object" % rpath)
        _check_keys(room, _ROOM_KEYS, rpath)
        _check_id(room["id"], _join(rpath, "id"), seen)
        _check_str(room["name"], _join(rpath, "name"))
        if not isinstance(room["sockets"], list):
            raise DocumentError("%s: must be a list"
                                % _join(rpath, "sockets"))
        for j, sock in enumerate(room["sockets"]):
            _check_socket(sock, "%s.sockets[%d]" % (rpath, j), seen)
