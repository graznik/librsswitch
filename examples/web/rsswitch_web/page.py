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

"""The page that the owner opens on a phone.

It is a plain file, static/index.html, and it needs no template: the
script of the page fetches the rooms from the JSON API. The file is
served by the app, behind the same authentication hook as the API.
"""

from __future__ import annotations

from flask import Blueprint, current_app

bp = Blueprint("page", __name__)


@bp.route("/", methods=["GET"])
def index():
    """Serve the page."""
    return current_app.send_static_file("index.html")
