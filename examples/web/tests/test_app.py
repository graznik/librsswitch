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

"""Tests of the configuration defaults and the application factory."""

from __future__ import annotations

from rsswitch_web import create_app
from rsswitch_web.config import DEFAULT_CONFIG_PATH, DEFAULTS, resolve_config


def test_resolve_config_defaults():
    cfg = resolve_config()
    assert cfg == {
        "daemon_host": "127.0.0.1",
        "daemon_port": 5000,
        "daemon_connect_timeout": 2,
        "daemon_read_timeout": 5,
        "listen_address": "0.0.0.0",
        "listen_port": 8080,
        "data_file": "/var/lib/rsswitch-web/data.json",
        "all_off_gap_ms": 200,
        "username": None,
        "password_hash": None,
    }
    assert list(cfg) == list(DEFAULTS)
    assert len(cfg) == 10
    assert DEFAULT_CONFIG_PATH == "/etc/rsswitch-web/config.json"


def test_resolve_config_returns_a_new_dict():
    cfg = resolve_config()
    cfg["daemon_port"] = 1
    assert DEFAULTS["daemon_port"] == 5000
    assert resolve_config()["daemon_port"] == 5000


def test_resolve_config_overrides_one_key():
    cfg = resolve_config({"daemon_port": 6000})
    assert cfg["daemon_port"] == 6000
    expected = dict(DEFAULTS, daemon_port=6000)
    assert cfg == expected


def test_create_app_keeps_resolved_config():
    app = create_app({"listen_port": 9090})
    assert app.config["RSSWITCH_WEB"] == resolve_config({"listen_port": 9090})
