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

"""Configuration keys of rsswitch-web, and the file they are read from."""

from __future__ import annotations

import json
import sys

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


class ConfigError(Exception):
    """The configuration cannot be used. The message is one line for the
    operator and starts with "config: "."""


def _reject_constant(name):
    raise ValueError("invalid JSON constant %s" % name)


def load_config(path=None) -> dict:
    """Return the mapping in the JSON file at path, unchecked.

    Without a path the default file is read, and when it does not exist
    the built-in defaults are used, which is said on stderr. A file that
    was asked for by name must exist. create_app() checks the keys and
    values of the mapping.
    """
    explicit = path is not None
    if not explicit:
        path = DEFAULT_CONFIG_PATH
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except FileNotFoundError as e:
        if explicit:
            raise ConfigError("config: cannot read %s: %s"
                              % (path, e.strerror))
        sys.stderr.write("rsswitch-web: warning: %s not found, "
                         "using built-in defaults\n" % path)
        sys.stderr.flush()
        return {}
    except OSError as e:
        raise ConfigError("config: cannot read %s: %s" % (path, e.strerror))
    try:
        mapping = json.loads(raw.decode("utf-8"),
                             parse_constant=_reject_constant)
    except (ValueError, RecursionError) as e:
        raise ConfigError("config: %s: not valid JSON: %s" % (path, e))
    if not isinstance(mapping, dict):
        raise ConfigError("config: %s: must be a JSON object" % path)
    return mapping


def resolve_config(mapping=None) -> dict:
    """Return a new dict made of DEFAULTS with mapping laid over it."""
    cfg = dict(DEFAULTS)
    if mapping:
        cfg.update(mapping)
    return cfg
