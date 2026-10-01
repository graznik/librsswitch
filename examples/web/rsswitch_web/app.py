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

import time
from collections.abc import Callable
from dataclasses import dataclass

from flask import Flask, request

from .api import register_blueprints
from .auth import LoginLimiter, make_auth_hook
from .config import resolve_config
from .daemon import RsswitchdClient
from .errors import MAX_BODY_BYTES, content_type_gate, register_error_handlers
from .page import bp as page_bp
from .store import Store
from .switching import utc_now


# Sent with every response. The policy lets the page load its own script
# and style sheet and talk to its own server, and nothing else: no other
# host, no inline code, no framing by another site.
SECURITY_HEADERS = {
    "Content-Security-Policy": (
        "default-src 'self'; base-uri 'none'; form-action 'self'; "
        "frame-ancestors 'none'; object-src 'none'"),
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "X-Frame-Options": "DENY",
}


def set_security_headers(response):
    """Add SECURITY_HEADERS to response, and make API answers no-store.

    A header that the response has already is left as it is. Flask runs
    this for the answers of the error handlers too, so a 401, 404, 415
    or 429 carries the headers as well.
    """
    for name, value in SECURITY_HEADERS.items():
        response.headers.setdefault(name, value)
    if request.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store"
    return response


@dataclass
class AppState:
    """What the request handlers share.

    daemon, clock and sleep are fields so that tests can replace them.
    """

    config: dict
    store: Store
    daemon: RsswitchdClient
    clock: Callable[[], str] = utc_now
    sleep: Callable[[float], None] = time.sleep


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
    hook = make_auth_hook(cfg["username"], cfg["password_hash"],
                          limiter=LoginLimiter())
    if hook is not None:
        app.before_request(hook)
    app.before_request(content_type_gate)
    register_blueprints(app)
    # The page comes after the hooks too, so it is behind the login.
    app.register_blueprint(page_bp)
    app.after_request(set_security_headers)
    return app
