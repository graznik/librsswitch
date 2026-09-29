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

"""Shared helpers and fixtures. Tests write only below tmp_path."""

from __future__ import annotations

import json

import pytest

from rsswitch_web import create_app


def write_doc(path, doc):
    """Write doc to path the way the store writes it."""
    text = json.dumps(doc, ensure_ascii=False, indent=2) + "\n"
    with open(str(path), "wb") as f:
        f.write(text.encode("utf-8"))


def sample_doc():
    """Return a household with two rooms and two sockets."""
    return {
        "version": 1,
        "rooms": [
            {
                "id": "r_000000000001",
                "name": "Wohnzimmer",
                "sockets": [
                    {
                        "id": "s_000000000001",
                        "name": "Stehlampe",
                        "type": "rev_008345",
                        "group": 0,
                        "number": 0,
                        "last_sent": None,
                    },
                    {
                        "id": "s_000000000002",
                        "name": "Heizlüfter",
                        "type": "pollin_2605",
                        "group": 17,
                        "number": 23,
                        "last_sent": {"state": "on",
                                      "at": "2026-09-29T08:00:00.000Z"},
                    },
                ],
            },
            {
                "id": "r_000000000002",
                "name": "Küche",
                "sockets": [],
            },
        ],
    }


@pytest.fixture
def make_app(tmp_path):
    """Return a factory: make_app(doc=None, **overrides) -> Flask app.

    The data file is tmp_path/data.json. It is written when doc is
    given and left missing otherwise.
    """
    def factory(doc=None, **overrides):
        path = tmp_path / "data.json"
        if doc is not None:
            write_doc(path, doc)
        return create_app({"data_file": str(path), **overrides})

    return factory
