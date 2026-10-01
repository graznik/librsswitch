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

"""The meta route: what the page needs to know about the socket types.

The limits of each type live in model.SOCKET_TYPES, which the
validation uses too. The route builds its answer from that table at
every request, so a type added to the table shows up here without
another change, and the page never carries a copy of the limits.
"""

from __future__ import annotations

from flask import Blueprint, jsonify

from .. import model

bp = Blueprint("meta", __name__, url_prefix="/api")


@bp.route("/meta", methods=["GET"])
def meta():
    """Return the number of groups and of sockets per group of each type."""
    return jsonify({"socket_types": {
        name: {"groups": limits.groups, "sockets": limits.sockets}
        for name, limits in model.SOCKET_TYPES.items()}})
