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

"""The meta route: what the page needs to know about this app.

The limits of each type live in model.SOCKET_TYPES, which the
validation uses too. The route builds its answer from that table at
every request, so a type added to the table shows up here without
another change, and the page never carries a copy of the limits.

The answer also says how long one switch may take and how long an
All-off pauses between two sends. Both come from the configuration,
and the page needs them to wait for an answer at least that long.
"""

from __future__ import annotations

from flask import Blueprint, jsonify

from .. import model
from . import state

bp = Blueprint("meta", __name__, url_prefix="/api")


@bp.route("/meta", methods=["GET"])
def meta():
    """Return the limits of each type, and the timing of a switch.

    socket_types has the number of groups and of sockets per group of
    each type. switch_timeout is the longest that one command can take
    before the app answers, in seconds: the connect and the read
    timeout of rsswitchd together. all_off_gap_ms is the pause between
    two sends of an All-off.
    """
    config = state().config
    return jsonify({
        "socket_types": {
            name: {"groups": limits.groups, "sockets": limits.sockets}
            for name, limits in model.SOCKET_TYPES.items()},
        "switch_timeout": (config["daemon_connect_timeout"]
                           + config["daemon_read_timeout"]),
        "all_off_gap_ms": config["all_off_gap_ms"]})
