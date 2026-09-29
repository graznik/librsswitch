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

from collections.abc import Callable
from dataclasses import dataclass

from flask import Flask

from .api import register_blueprints
from .config import resolve_config
from .daemon import RsswitchdClient
from .errors import MAX_BODY_BYTES, content_type_gate, register_error_handlers
from .store import Store
from .switching import utc_now


@dataclass
class AppState:
    """What the request handlers share.

    daemon and clock are fields so that tests can replace them.
    """

    config: dict
    store: Store
    daemon: RsswitchdClient
    clock: Callable[[], str] = utc_now


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
    daemon = RsswitchdClient(cfg["daemon_host"], cfg["daemon_port"],
                             cfg["daemon_connect_timeout"],
                             cfg["daemon_read_timeout"])
    app.extensions["rsswitch_web"] = AppState(cfg, store, daemon)
    # Non-ASCII names go out as UTF-8 instead of \u escapes, and
    # objects keep the order of their fields.
    app.json.ensure_ascii = False
    app.json.sort_keys = False
    app.config["MAX_CONTENT_LENGTH"] = MAX_BODY_BYTES
    register_error_handlers(app)
    # Hooks run in the order they are registered. An authentication
    # hook must be registered before this gate, so that a client that
    # has not logged in gets 401 and not 415.
    app.before_request(content_type_gate)
    register_blueprints(app)
    return app
