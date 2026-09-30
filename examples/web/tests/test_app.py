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

"""Tests of the application factory."""

from __future__ import annotations

import pytest

from rsswitch_web import create_app
from rsswitch_web.app import AppState
from rsswitch_web.config import ConfigError, resolve_config
from rsswitch_web.store import Store, StoreError


def test_create_app_keeps_resolved_config(tmp_path):
    path = str(tmp_path / "data.json")
    app = create_app({"data_file": path, "listen_port": 9090})
    expected = resolve_config({"data_file": path, "listen_port": 9090})
    assert app.config["RSSWITCH_WEB"] == expected
    state = app.extensions["rsswitch_web"]
    assert isinstance(state, AppState)
    assert state.config is app.config["RSSWITCH_WEB"]
    assert isinstance(state.store, Store)
    assert state.store.path == path


def test_create_app_with_missing_data_dir_raises_store_error(tmp_path):
    path = tmp_path / "nodir" / "data.json"
    with pytest.raises(StoreError):
        create_app({"data_file": str(path)})
    assert not (tmp_path / "nodir").exists()


@pytest.mark.parametrize("content", [b"{", b'{"version": 2, "rooms": []}'],
                         ids=["not-json", "unknown-version"])
def test_create_app_with_corrupt_file_raises_and_keeps_bytes(tmp_path,
                                                             content):
    path = tmp_path / "data.json"
    path.write_bytes(content)
    with pytest.raises(StoreError):
        create_app({"data_file": str(path)})
    assert path.read_bytes() == content
    assert sorted(p.name for p in tmp_path.iterdir()) == ["data.json"]


@pytest.mark.parametrize("config", [
    {"pasword_hash": "scrypt:32768:8:1$salt$hash"},
    {"username": "admin"},
    {"password_hash": "garbage"},
    {"listen_port": "8080"},
], ids=["typo-key", "username-without-hash", "bad-hash", "bad-port"])
def test_create_app_with_invalid_config_raises_and_creates_no_file(
        tmp_path, config):
    path = tmp_path / "data.json"
    with pytest.raises(ConfigError):
        create_app({"data_file": str(path), **config})
    assert list(tmp_path.iterdir()) == []
