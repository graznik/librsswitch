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

"""The one JSON parser of the app, for the config file, the data file
and request bodies."""

from __future__ import annotations

import json


def _refuse_constant(name):
    raise ValueError("invalid JSON constant %s" % name)


def strict_loads(raw) -> object:
    """Parse raw, bytes or str, as JSON, or raise ValueError.

    Bytes must be UTF-8. NaN, Infinity and -Infinity are refused,
    although Python would read them, because no other JSON parser
    does. A document nested so deeply that the parser runs out of
    stack is a ValueError too. So a caller catches only ValueError,
    and never sees a UnicodeDecodeError, a RecursionError or a float
    that cannot be written back as JSON.
    """
    if isinstance(raw, (bytes, bytearray)):
        raw = bytes(raw).decode("utf-8")
    try:
        return json.loads(raw, parse_constant=_refuse_constant)
    except RecursionError:
        raise ValueError("JSON is nested too deeply") from None
