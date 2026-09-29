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

"""The switch routes."""

from __future__ import annotations

from flask import Blueprint, jsonify

from ..errors import ApiError, json_body, reject_unknown_fields
from ..switching import all_off, switch_socket
from . import state

bp = Blueprint("switch", __name__, url_prefix="/api")


@bp.route("/sockets/<sid>/switch", methods=["POST"])
def switch(sid):
    """Switch one socket on or off. The body is {"state": "on"|"off"}."""
    body = json_body()
    reject_unknown_fields(body, {"state"})
    wanted = body.get("state")
    if not isinstance(wanted, str) or wanted not in ("on", "off"):
        raise ApiError(422, "validation", 'state must be "on" or "off"',
                       field="state")
    app_state = state()
    return jsonify(switch_socket(app_state.store, app_state.daemon, sid,
                                 wanted, clock=app_state.clock))


def _all_off(room_id):
    body = json_body(required=False)
    reject_unknown_fields(body, set())
    app_state = state()
    return jsonify(all_off(
        app_state.store, app_state.daemon, room_id,
        gap_seconds=app_state.config["all_off_gap_ms"] / 1000,
        sleep=app_state.sleep, clock=app_state.clock))


@bp.route("/rooms/<rid>/all-off", methods=["POST"])
def room_all_off(rid):
    """Switch off every socket of one room. The body is empty or {}."""
    return _all_off(rid)


@bp.route("/all-off", methods=["POST"])
def household_all_off():
    """Switch off every socket of every room. The body is empty or {}."""
    return _all_off(None)
