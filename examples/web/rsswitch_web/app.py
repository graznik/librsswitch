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

"""The application factory."""

from __future__ import annotations

from dataclasses import dataclass

from flask import Flask

from .api import register_blueprints
from .config import resolve_config
from .errors import MAX_BODY_BYTES, register_error_handlers
from .store import Store


@dataclass
class AppState:
    """What the request handlers share: the settings and the store."""

    config: dict
    store: Store


def create_app(config=None) -> Flask:
    """Build the Flask app. config is a mapping laid over the defaults.

    A data file that cannot be used raises StoreError, so no app exists
    that would serve or overwrite a corrupt file.
    """
    cfg = resolve_config(config)
    store = Store(cfg["data_file"])
    store.check()
    app = Flask(__name__)
    app.config["RSSWITCH_WEB"] = cfg
    app.extensions["rsswitch_web"] = AppState(cfg, store)
    # Non-ASCII names go out as UTF-8 instead of \u escapes, and
    # objects keep the order of their fields.
    app.json.ensure_ascii = False
    app.json.sort_keys = False
    app.config["MAX_CONTENT_LENGTH"] = MAX_BODY_BYTES
    register_error_handlers(app)
    register_blueprints(app)
    return app
