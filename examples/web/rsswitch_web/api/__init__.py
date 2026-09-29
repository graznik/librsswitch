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

"""The JSON API under /api."""

from __future__ import annotations

from flask import current_app


def state():
    """Return the AppState of the running app.

    It is looked up through current_app instead of importing app.py,
    which imports this package.
    """
    return current_app.extensions["rsswitch_web"]


def register_blueprints(app):
    """Register every API blueprint on app."""
    from .rooms import bp as rooms_bp
    from .switch import bp as switch_bp

    app.register_blueprint(rooms_bp)
    app.register_blueprint(switch_bp)
