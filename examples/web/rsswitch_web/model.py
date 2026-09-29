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
