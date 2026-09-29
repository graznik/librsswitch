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

"""Checks of what a client sends. The functions are pure: they keep no
state and raise ApiError 422 for the first problem they find."""

from __future__ import annotations

import unicodedata

from .errors import ApiError
from .model import SOCKET_TYPES, address_problem

# The longest name, counted in code points after normalisation.
MAX_NAME_LEN = 64


def clean_name(value, field="name") -> str:
    """Return value as a name that can be stored, or raise ApiError 422.

    A name is trimmed and put in NFC, so that a precomposed and a
    combining "ü", typed on two different phones, are the same name.
    It has 1 to 64 code points. Control characters are refused,
    because they would break the UI and the lines of the log, and so
    are lone surrogates, which JSON can carry but no file can hold.
    field names the request field in the error.
    """
    if not isinstance(value, str):
        raise ApiError(422, "validation", "must be a string", field=field)
    name = unicodedata.normalize("NFC", value).strip()
    if not name:
        raise ApiError(422, "validation", "must not be empty", field=field)
    if len(name) > MAX_NAME_LEN:
        raise ApiError(422, "validation",
                       "must be at most %d characters" % MAX_NAME_LEN,
                       field=field)
    for char in name:
        category = unicodedata.category(char)
        if category == "Cc":
            raise ApiError(422, "validation",
                           "must not contain control characters",
                           field=field)
        if category == "Cs":
            raise ApiError(422, "validation",
                           "must not contain invalid characters",
                           field=field)
    return name


def name_key(name) -> str:
    """Return the key that decides whether two names are the same.

    The name is put in NFC, casefolded, so that "Küche" and "küche"
    collide, and put in NFC again, because folding can leave a string
    that is not normalised.
    """
    folded = unicodedata.normalize("NFC", name).casefold()
    return unicodedata.normalize("NFC", folded)


def check_socket_fields(fields) -> dict:
    """Check the type and the address of a socket, or raise ApiError 422.

    fields is a mapping with the keys type, group and number. The
    first bad field is reported, in that order, so an unknown type is
    named before a group that only makes sense for a known one. A
    missing key counts as None. group and number must be JSON
    integers within the limits of the type: a float, a string or a
    boolean is refused, never converted, and a number that is too large
    is refused, never clamped. The limits have one source, SOCKET_TYPES.

    Return {"type", "group", "number"}.
    """
    type_name = fields.get("type")
    if not isinstance(type_name, str) or type_name not in SOCKET_TYPES:
        raise ApiError(422, "validation",
                       "must be one of %s" % ", ".join(SOCKET_TYPES),
                       field="type")
    group = fields.get("group")
    number = fields.get("number")
    problem = address_problem(type_name, group, number)
    if problem is not None:
        raise ApiError(422, "validation", problem[1], field=problem[0])
    return {"type": type_name, "group": group, "number": number}
