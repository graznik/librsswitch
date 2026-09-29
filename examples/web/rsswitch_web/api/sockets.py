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

"""The socket routes.

They follow the rules of the room routes (see rooms.py): one
Store.mutate() does the lookup, the checks and the change, and the
checks run in this order: 404 for the id in the URL, 422 for the body
in the order name, type, group, number, room_id, 409 for a conflict.

An edit is validated as the record it produces, not as the fields the
client sent: the type of a Pollin socket at group 20 cannot be changed
to REV on its own, however valid the new type is by itself.
"""

from __future__ import annotations

from flask import Blueprint, jsonify

from ..errors import ApiError, json_body, reject_unknown_fields
from ..model import new_id
from ..store import find_room, find_socket, socket_json
from ..validation import check_socket_fields, clean_name, name_key
from . import state
from .rooms import check_permutation, get_room, id_list

bp = Blueprint("sockets", __name__, url_prefix="/api")

FIELDS = {"name", "type", "group", "number"}
EDIT_FIELDS = FIELDS | {"room_id"}


def get_socket(data, sid):
    """Return (room, socket) for the id sid, or raise ApiError 404."""
    room, sock = find_socket(data, sid)
    if sock is None:
        raise ApiError(404, "not_found", "socket not found")
    return room, sock


def check_socket_name(room, name, own=None):
    """Raise ApiError 409 when a socket of room other than own has name."""
    key = name_key(name)
    for sock in room["sockets"]:
        if sock is not own and name_key(sock["name"]) == key:
            raise ApiError(409, "name_conflict",
                           "a socket with this name already exists in "
                           "the room", field="name")


def address_of(sock):
    """Return the address (type, group, number) that a socket switches."""
    return (sock["type"], sock["group"], sock["number"])


def duplicate_warnings(data, sid) -> list:
    """Return the warnings about the address of the socket sid.

    Two sockets with the same type, group and number switch the same
    receivers. That is sometimes intended, so it is a warning and never
    a refusal. The list is empty when no other socket has the address of
    sid, and else holds one warning that names those sockets in display
    order, the sockets of every room included and sid itself left out.
    """
    _, sock = find_socket(data, sid)
    address = address_of(sock)
    others = [other["id"] for room in data["rooms"]
              for other in room["sockets"]
              if other["id"] != sid and address_of(other) == address]
    if not others:
        return []
    return [{"code": "duplicate_address",
             "message": "another socket uses the same type, group and "
                        "number",
             "socket_ids": others}]


@bp.route("/sockets/<sid>", methods=["GET"])
def read_socket(sid):
    """Return one socket."""
    room, sock = get_socket(state().store.snapshot(), sid)
    return jsonify({"socket": socket_json(room, sock)})


@bp.route("/rooms/<rid>/sockets", methods=["POST"])
def create_socket(rid):
    """Add a socket at the end of a room.

    The body has name, type, group and number, and nothing else.
    """
    body = json_body()

    def create(data):
        room = get_room(data, rid)
        reject_unknown_fields(body, FIELDS)
        name = clean_name(body.get("name"))
        address = check_socket_fields(body)
        check_socket_name(room, name)
        sock = {"id": new_id("s_", data), "name": name,
                "type": address["type"], "group": address["group"],
                "number": address["number"], "last_sent": None}
        room["sockets"].append(sock)
        return {"socket": socket_json(room, sock),
                "warnings": duplicate_warnings(data, sock["id"])}

    return jsonify(state().store.mutate(create)), 201


def target_room(data, room, body):
    """Return the room the socket is in after an edit of body.

    That is room, unless body has a room_id of another room. Raise
    ApiError 422 when room_id is not a string or names no room: the
    socket in the URL exists, what is wrong is the reference in the body.
    """
    if "room_id" not in body:
        return room
    rid = body["room_id"]
    if not isinstance(rid, str):
        raise ApiError(422, "validation", "must be a string",
                       field="room_id")
    if rid == room["id"]:
        return room
    target = find_room(data, rid)
    if target is None:
        raise ApiError(422, "validation", "no room with this id",
                       field="room_id")
    return target


@bp.route("/sockets/<sid>", methods=["PATCH"])
def edit_socket(sid):
    """Change the name, type, group or number of a socket, or move it.

    Only the fields that are in the body change. room_id moves the
    socket to the end of another room. It keeps its id, and its
    last_sent unless the address changes with it. A change of the
    address makes last_sent null, because what was sent before was
    meant for another receiver.
    """
    body = json_body()

    def edit(data):
        room, sock = get_socket(data, sid)
        reject_unknown_fields(body, EDIT_FIELDS)
        merged = {key: sock[key] for key in FIELDS}
        merged.update((key, body[key]) for key in FIELDS if key in body)
        if "name" in body:
            merged["name"] = clean_name(body["name"])
        address = check_socket_fields(merged)
        target = target_room(data, room, body)
        if "name" in body or target is not room:
            check_socket_name(target, merged["name"], own=sock)
        changed = tuple(address.values()) != address_of(sock)
        sock["name"] = merged["name"]
        sock.update(address)
        if changed:
            sock["last_sent"] = None
        if target is not room:
            room["sockets"].remove(sock)
            target["sockets"].append(sock)
        return {"socket": socket_json(target, sock),
                "warnings": duplicate_warnings(data, sid)}

    return jsonify(state().store.mutate(edit))


@bp.route("/sockets/<sid>", methods=["DELETE"])
def delete_socket(sid):
    """Delete a socket."""
    def delete(data):
        room, sock = get_socket(data, sid)
        room["sockets"].remove(sock)

    state().store.mutate(delete)
    return "", 204


@bp.route("/rooms/<rid>/sockets/order", methods=["PUT"])
def order_sockets(rid):
    """Put the sockets of a room in the order of a list of all their ids."""
    body = json_body()

    def order(data):
        room = get_room(data, rid)
        reject_unknown_fields(body, {"ids"})
        ids = id_list(body)
        check_permutation(ids, [s["id"] for s in room["sockets"]])
        by_id = {s["id"]: s for s in room["sockets"]}
        room["sockets"][:] = [by_id[i] for i in ids]
        return ids

    return jsonify({"ids": state().store.mutate(order)})
