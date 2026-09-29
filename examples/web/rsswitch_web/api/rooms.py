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

"""The room routes.

Every route that changes something does all of its work in one
Store.mutate(): the lookup of the id, the checks of the body, the checks
against the other rooms and the change itself. Checking on a snapshot
first would let two phones that ask for the same name at the same time
both pass. A check that fails raises ApiError inside the function,
which writes nothing. The checks run in this order: 404 for the id in
the URL, 422 for the body, 409 for a conflict.
"""

from __future__ import annotations

from flask import Blueprint, jsonify

from ..errors import ApiError, json_body, reject_unknown_fields
from ..model import new_id
from ..store import find_room, room_json, tree_json
from ..validation import clean_name, name_key
from . import state

bp = Blueprint("rooms", __name__, url_prefix="/api")


def get_room(data, rid):
    """Return the room with the id rid, or raise ApiError 404."""
    room = find_room(data, rid)
    if room is None:
        raise ApiError(404, "not_found", "room not found")
    return room


def check_room_name(data, name, own=None):
    """Raise ApiError 409 when a room other than own has this name."""
    key = name_key(name)
    for room in data["rooms"]:
        if room is not own and name_key(room["name"]) == key:
            raise ApiError(409, "name_conflict",
                           "a room with this name already exists",
                           field="name")


def id_list(body):
    """Return the ids of a reorder body, or raise ApiError 422."""
    ids = body.get("ids")
    if not isinstance(ids, list) or not all(isinstance(i, str)
                                            for i in ids):
        raise ApiError(422, "validation", "must be a list of strings",
                       field="ids")
    return ids


def check_permutation(ids, current):
    """Raise ApiError 409 unless ids has every id of current once."""
    if len(ids) != len(set(ids)) or set(ids) != set(current):
        raise ApiError(409, "order_mismatch",
                       "the list is not the current set of ids; "
                       "reload and try again")


@bp.route("/rooms", methods=["GET"])
def list_rooms():
    """Return every room with its sockets, in display order."""
    return jsonify(tree_json(state().store.snapshot()))


@bp.route("/rooms/<rid>", methods=["GET"])
def read_room(rid):
    """Return one room with its sockets."""
    return jsonify({"room": room_json(get_room(state().store.snapshot(),
                                               rid))})


@bp.route("/rooms", methods=["POST"])
def create_room():
    """Add a room at the end. The body is {"name": ...}."""
    body = json_body()
    reject_unknown_fields(body, {"name"})

    def create(data):
        name = clean_name(body.get("name"))
        check_room_name(data, name)
        room = {"id": new_id("r_", data), "name": name, "sockets": []}
        data["rooms"].append(room)
        return room_json(room)

    return jsonify({"room": state().store.mutate(create)}), 201


@bp.route("/rooms/<rid>", methods=["PATCH"])
def edit_room(rid):
    """Rename a room. The body is {"name": ...}, or {} to change nothing."""
    body = json_body()

    def edit(data):
        room = get_room(data, rid)
        reject_unknown_fields(body, {"name"})
        if "name" in body:
            name = clean_name(body["name"])
            check_room_name(data, name, own=room)
            room["name"] = name
        return room_json(room)

    return jsonify({"room": state().store.mutate(edit)})


@bp.route("/rooms/<rid>", methods=["DELETE"])
def delete_room(rid):
    """Delete a room that holds no sockets."""
    def delete(data):
        room = get_room(data, rid)
        if room["sockets"]:
            raise ApiError(409, "room_not_empty",
                           "the room still contains sockets; move or "
                           "delete them first")
        data["rooms"].remove(room)

    state().store.mutate(delete)
    return "", 204


@bp.route("/rooms/order", methods=["PUT"])
def order_rooms():
    """Put the rooms in the order of a list of all their ids."""
    body = json_body()
    reject_unknown_fields(body, {"ids"})
    ids = id_list(body)

    def order(data):
        check_permutation(ids, [r["id"] for r in data["rooms"]])
        by_id = {r["id"]: r for r in data["rooms"]}
        data["rooms"][:] = [by_id[i] for i in ids]
        return ids

    return jsonify({"ids": state().store.mutate(order)})
