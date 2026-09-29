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

"""Configuration keys of rsswitch-web and their default values."""

from __future__ import annotations

DEFAULT_CONFIG_PATH = "/etc/rsswitch-web/config.json"

# Every configuration key with its default. Timeouts are in seconds,
# all_off_gap_ms is the pause between two sends of an All-off.
DEFAULTS = {
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


def resolve_config(mapping=None) -> dict:
    """Return a new dict made of DEFAULTS with mapping laid over it."""
    cfg = dict(DEFAULTS)
    if mapping:
        cfg.update(mapping)
    return cfg
