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

import difflib
import hashlib
import ipaddress
import os
import re
import sys
import unicodedata
from collections.abc import Mapping

from werkzeug.security import check_password_hash

from .jsonutil import strict_loads

DEFAULT_CONFIG_PATH = "/etc/rsswitch-web/config.json"

# Every configuration key with its default. Timeouts are in seconds,
# all_off_gap_ms is the pause between two sends of an All-off, threads
# is the number of worker threads of waitress under the serve command.
# They bound how many requests, password checks and rsswitchd calls run
# at once.
DEFAULTS = {
    "daemon_host": "127.0.0.1",
    "daemon_port": 5000,
    "daemon_connect_timeout": 2,
    "daemon_read_timeout": 5,
    "listen_address": "0.0.0.0",
    "listen_port": 8080,
    "threads": 4,
    "data_file": "/var/lib/rsswitch-web/data.json",
    "all_off_gap_ms": 200,
    "username": None,
    "password_hash": None,
}


class ConfigError(Exception):
    """The configuration cannot be used. The message is one line for the
    operator and starts with "config: "."""


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
        mapping = strict_loads(raw)
    except ValueError as e:
        raise ConfigError("config: %s: not valid JSON: %s" % (path, e))
    if not isinstance(mapping, dict):
        raise ConfigError("config: %s: must be a JSON object" % path)
    return mapping


def _suggest(key):
    """Return the key of DEFAULTS that key was probably meant to be."""
    matches = difflib.get_close_matches(key, DEFAULTS, n=1)
    return matches[0] if matches else None


def _is_int(value, low, high):
    # bool is a subclass of int, and true is not a port.
    return type(value) is int and low <= value <= high


def _check_host(value):
    if type(value) is not str or not value:
        return "must be a non-empty string"
    if any(c.isspace() for c in value):
        return "must not contain whitespace"
    return None


def _port_check(low):
    def check(value):
        if not _is_int(value, low, 65535):
            return "must be an integer from %d to 65535" % low
        return None
    return check


def _check_timeout(value):
    if type(value) not in (int, float) or not 0 < value <= 60:
        return "must be a number greater than 0 and at most 60 (seconds)"
    return None


def _check_threads(value):
    if not _is_int(value, 1, 16):
        return "must be an integer from 1 to 16"
    return None


def _check_listen_address(value):
    if type(value) is not str:
        return "must be an IP address such as 0.0.0.0 or ::"
    try:
        ipaddress.ip_address(value)
    except ValueError:
        return "must be an IP address such as 0.0.0.0 or ::, not a name"
    return None


def _check_data_file(value):
    if type(value) is not str or not os.path.isabs(value):
        return "must be an absolute path"
    if "\0" in value:
        return "must not contain a NUL character"
    return None


def _check_gap(value):
    if not _is_int(value, 0, 10000):
        return "must be an integer from 0 to 10000 (milliseconds)"
    return None


def _check_username(value):
    if value is None:
        return None
    if type(value) is not str or not value:
        return "must be null or a non-empty string"
    if ":" in value:
        return "must not contain ':', which Basic authentication cannot carry"
    # Cc is a control character. A lone surrogate (Cs) cannot be encoded
    # as UTF-8, so it could never match a login.
    if any(unicodedata.category(c) in ("Cc", "Cs") for c in value):
        return "must not contain control characters"
    return None


def _positive_int(text):
    """Return the int that text spells in decimal digits, else 0."""
    return int(text) if re.fullmatch("[0-9]+", text) else 0


# The largest scrypt cost that is accepted. Every login pays it, on a
# board with little memory and a slow CPU. These are far above what
# anybody sets, and far below what makes a login fail or hang.
MAX_SCRYPT_MEMORY = 256 * 1024 * 1024
MAX_SCRYPT_P = 16
MAX_PBKDF2_ITERATIONS = 10 ** 7


def _check_hash(value):
    """Return why value is not a Werkzeug password hash, or None.

    A hash is method$salt$hash. The method is scrypt:N:r:p or
    pbkdf2:<hash function>:<iterations>. Anything else would make
    every login fail, or fail with an error, so it is refused here.
    A hash is also tried once with an empty password. For scrypt this
    shows whether OpenSSL accepts the parameters, which cannot be told
    from the numbers alone. For both methods a hash that the empty
    password matches is refused, because the login would let anybody
    in. That costs one hash at start-up, bounded by the caps below.
    """
    if value is None:
        return None
    what = ("must be null or a Werkzeug password hash such as "
            "scrypt:32768:8:1$salt$hash")
    empty = ("is the hash of an empty password; make a hash of a real "
             "password with hash-password")
    if type(value) is not str:
        return what
    parts = value.split("$")
    if len(parts) != 3 or not parts[1] or not parts[2]:
        return what
    method = parts[0].split(":")
    if method[0] == "scrypt" and len(method) == 4:
        n, r, p = (_positive_int(m) for m in method[1:])
        if not (n >= 2 and n & (n - 1) == 0 and r >= 1 and p >= 1):
            return ("the scrypt parameters must be positive integers "
                    "and N a power of two")
        if 128 * n * r > MAX_SCRYPT_MEMORY or p > MAX_SCRYPT_P:
            return "the scrypt parameters are too large"
        try:
            matches = check_password_hash(value, "")
        except (ValueError, MemoryError) as e:
            return "the scrypt parameters cannot be used: %s" % e
        return empty if matches else None
    if method[0] == "pbkdf2" and len(method) == 3:
        if method[1] not in hashlib.algorithms_guaranteed:
            return "unknown pbkdf2 hash function '%s'" % method[1]
        try:
            hashlib.pbkdf2_hmac(method[1], b"", b"", 1)
        except ValueError:
            return "the hash function '%s' cannot be used" % method[1]
        iterations = _positive_int(method[2])
        if iterations < 1:
            return "the pbkdf2 iterations must be a positive integer"
        if iterations > MAX_PBKDF2_ITERATIONS:
            return "the pbkdf2 iterations are too many"
        return empty if check_password_hash(value, "") else None
    return what


# The checks of every key, in the order of DEFAULTS. Each returns why a
# value is not acceptable, or None.
_CHECKS = {
    "daemon_host": _check_host,
    "daemon_port": _port_check(1),
    "daemon_connect_timeout": _check_timeout,
    "daemon_read_timeout": _check_timeout,
    "listen_address": _check_listen_address,
    "listen_port": _port_check(0),
    "threads": _check_threads,
    "data_file": _check_data_file,
    "all_off_gap_ms": _check_gap,
    "username": _check_username,
    "password_hash": _check_hash,
}


def resolve_config(mapping=None) -> dict:
    """Return a new dict made of DEFAULTS with mapping laid over it.

    Every key and value is checked, and the first problem raises a
    ConfigError that names the key and the reason. An unknown key is
    one of them. A misspelt password_hash would otherwise be ignored,
    and the app would start without the password its operator meant to
    set, and nobody would notice.
    """
    if mapping is None:
        mapping = {}
    if not isinstance(mapping, Mapping):
        raise ConfigError("config: must be a JSON object")
    unknown = sorted((k for k in mapping if k not in DEFAULTS), key=str)
    if unknown:
        text = "config: unknown key '%s'" % unknown[0]
        match = _suggest(str(unknown[0]))
        if match:
            text += " (did you mean '%s'?)" % match
        raise ConfigError(text)
    cfg = dict(DEFAULTS)
    cfg.update(mapping)
    for key, check in _CHECKS.items():
        reason = check(cfg[key])
        if reason:
            raise ConfigError("config: key '%s': %s" % (key, reason))
    if cfg["username"] is not None and cfg["password_hash"] is None:
        raise ConfigError("config: key 'username': needs a password_hash, "
                          "or the app would not ask for a login")
    if cfg["password_hash"] is not None and cfg["username"] is None:
        cfg["username"] = "admin"
    return cfg
