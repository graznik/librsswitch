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

"""Tests of the configuration: defaults and strict validation."""

from __future__ import annotations

import json
import re

import pytest
from werkzeug.security import generate_password_hash

from rsswitch_web import create_app
from rsswitch_web.config import (DEFAULT_CONFIG_PATH, DEFAULTS, ConfigError,
                                 load_config, resolve_config)

# A quick hash; the tests never log in.
HASH = generate_password_hash("pw", "pbkdf2:sha256:1000")

ACCEPTED = {
    "daemon_host": ["127.0.0.1", "localhost"],
    "daemon_port": [1, 65535],
    "daemon_connect_timeout": [0.05, 2, 60],
    "daemon_read_timeout": [0.05, 2, 60],
    "listen_address": ["0.0.0.0", "127.0.0.1", "::"],
    "listen_port": [0, 65535],
    "data_file": ["/tmp/x.json"],
    "all_off_gap_ms": [0, 200, 10000],
    "username": ["admin", "Ölmühle"],
}

REFUSED = {
    "daemon_host": ["", " x", "a b", "a\tb", 5, None, True],
    "daemon_port": [0, 65536, -1, "5000", True, 5000.0, None],
    "daemon_connect_timeout": [0, -1, 60.001, "2", True, None,
                               float("nan"), float("inf")],
    "daemon_read_timeout": [0, -1, 60.001, "2", True, None,
                            float("nan"), float("inf")],
    "listen_address": ["localhost", "", "999.1.1.1", "0.0.0", 5, None],
    "listen_port": [-1, 65536, True, "8080", 80.0, None],
    "data_file": ["data.json", "", 7, None, "/tmp/a\0b"],
    "all_off_gap_ms": [-1, 10001, 0.5, True, "200", None],
    "username": ["", "a:b", "a\nb", "a\x00b", "a\ud800b", 5, True],
}


def cases(table):
    return [(key, value) for key, values in table.items()
            for value in values]


def ids(table):
    return ["%s-%r" % (key, value) for key, value in cases(table)]


def with_hash(key, value):
    """The mapping for one key. A username needs a hash to be legal."""
    mapping = {key: value}
    if key == "username":
        mapping["password_hash"] = HASH
    return mapping


@pytest.mark.parametrize("key,value", cases(ACCEPTED), ids=ids(ACCEPTED))
def test_accepted_values(key, value):
    cfg = resolve_config(with_hash(key, value))
    assert cfg[key] == value
    assert list(cfg) == list(DEFAULTS)


@pytest.mark.parametrize("key,value", cases(REFUSED), ids=ids(REFUSED))
def test_refused_values(key, value):
    with pytest.raises(ConfigError) as exc:
        resolve_config(with_hash(key, value))
    assert str(exc.value).startswith("config: key '%s': " % key)


@pytest.mark.parametrize("key,value", cases(REFUSED), ids=ids(REFUSED))
def test_error_message_format(key, value):
    with pytest.raises(ConfigError) as exc:
        resolve_config(with_hash(key, value))
    assert re.fullmatch(r"config: key '[a-z_]+': [^\n]+", str(exc.value))


def test_boundaries_between_accepted_and_refused():
    assert resolve_config({"daemon_port": 1})["daemon_port"] == 1
    assert resolve_config({"daemon_port": 65535})["daemon_port"] == 65535
    assert resolve_config({"listen_port": 0})["listen_port"] == 0
    with pytest.raises(ConfigError, match="'daemon_port'"):
        resolve_config({"daemon_port": 0})
    with pytest.raises(ConfigError, match="'daemon_port'"):
        resolve_config({"daemon_port": 65536})
    with pytest.raises(ConfigError, match="'listen_port'"):
        resolve_config({"listen_port": -1})
    with pytest.raises(ConfigError, match="'listen_port'"):
        resolve_config({"listen_port": 65536})


def test_password_hash_formats():
    accepted = [
        None,
        generate_password_hash("pw"),
        generate_password_hash("pw", "pbkdf2:sha256:1000"),
        generate_password_hash("pw", "pbkdf2:sha512:600000"),
        "scrypt:2:1:1$s$h",
    ]
    for value in accepted:
        assert resolve_config({"password_hash": value})["password_hash"] \
            == value
    refused = [
        "", "garbage", "plain$s$h", 5, True, ["x"],
        "scrypt:abc:8:1$s$h",
        "scrypt:1000:8:1$s$h",       # N is not a power of two
        "scrypt:1:8:1$s$h",          # N is a power of two, but below 2
        "scrypt:0:8:1$s$h",
        "scrypt:-2:8:1$s$h",
        "scrypt:32768:0:1$s$h",
        "scrypt:32768:8:0$s$h",
        "scrypt:32768:8$s$h",
        "scrypt:32768:8:1:1$s$h",
        "scrypt:32768:8:1$$h",
        "scrypt:32768:8:1$s$",
        "scrypt:32768:8:1$s",
        "scrypt:32768:8:1$s$h$x",
        "pbkdf2:nosuchalgo:1000$s$h",
        "pbkdf2:sha256:0$s$h",
        "pbkdf2:sha256:x$s$h",
        "pbkdf2:sha256$s$h",
        "pbkdf2:sha256:1000:1$s$h",
        "pbkdf2:shake_128:1000$s$h",  # a hash that pbkdf2 cannot use
        "pbkdf2:sha256:1000$$h",
        "pbkdf2:sha256:1000$s$",
        "$s$h",
        "scrypt$s$h",
    ]
    for value in refused:
        with pytest.raises(ConfigError,
                           match="^config: key 'password_hash': "):
            resolve_config({"password_hash": value})


def test_username_without_hash_is_error():
    for username in ("admin", "bob"):
        with pytest.raises(ConfigError, match="^config: key 'username': "):
            resolve_config({"username": username})
    with pytest.raises(ConfigError, match="^config: key 'username': "):
        resolve_config({"username": "bob", "password_hash": None})


def test_hash_without_username_means_admin():
    assert resolve_config({"password_hash": HASH})["username"] == "admin"
    cfg = resolve_config({"username": "bob", "password_hash": HASH})
    assert cfg["username"] == "bob"


def test_no_hash_means_no_authentication():
    cfg = resolve_config({})
    assert cfg["password_hash"] is None
    assert cfg["username"] is None


def test_unknown_key_suggests_close_match():
    with pytest.raises(ConfigError) as exc:
        resolve_config({"pasword_hash": HASH})
    assert str(exc.value) == ("config: unknown key 'pasword_hash' "
                              "(did you mean 'password_hash'?)")
    with pytest.raises(ConfigError) as exc:
        resolve_config({"listen_adress": "127.0.0.1"})
    assert "(did you mean 'listen_address'?)" in str(exc.value)


def test_unknown_key_without_close_match_has_no_suggestion():
    with pytest.raises(ConfigError) as exc:
        resolve_config({"zzzz": 1})
    assert str(exc.value) == "config: unknown key 'zzzz'"


def test_unknown_key_reported_first_alphabetically():
    mapping = {"zebra": 1, "apple": 2, "listen_port": -1}
    with pytest.raises(ConfigError, match="unknown key 'apple'"):
        resolve_config(mapping)
    with pytest.raises(ConfigError, match="unknown key 'apple'"):
        resolve_config(dict(reversed(list(mapping.items()))))


def test_first_problem_is_the_first_key_in_defaults_order():
    first = {"daemon_port": 0, "all_off_gap_ms": -1, "listen_port": -1}
    for mapping in (first, dict(reversed(list(first.items())))):
        with pytest.raises(ConfigError, match="key 'daemon_port'"):
            resolve_config(mapping)


def test_empty_mapping_equals_defaults():
    assert resolve_config({}) == DEFAULTS
    assert resolve_config() == DEFAULTS
    assert resolve_config(None) == DEFAULTS
    assert len(DEFAULTS) == 10
    assert DEFAULT_CONFIG_PATH == "/etc/rsswitch-web/config.json"


def test_resolve_config_returns_a_new_dict():
    cfg = resolve_config()
    cfg["daemon_port"] = 1
    assert DEFAULTS["daemon_port"] == 5000
    assert resolve_config()["daemon_port"] == 5000


def test_resolve_config_overrides_one_key():
    cfg = resolve_config({"daemon_port": 6000})
    assert cfg == dict(DEFAULTS, daemon_port=6000)


def test_file_key_order_is_irrelevant():
    mapping = {"listen_port": 9090, "daemon_port": 6000, "username": "bob",
               "password_hash": HASH, "all_off_gap_ms": 50}
    forward = resolve_config(mapping)
    backward = resolve_config(dict(reversed(list(mapping.items()))))
    assert forward == backward
    assert list(forward) == list(DEFAULTS)


@pytest.mark.parametrize("value", [[], [1], "x", 5, True])
def test_a_mapping_that_is_not_an_object_is_an_error(value):
    with pytest.raises(ConfigError, match="^config: must be a JSON object"):
        resolve_config(value)


def test_the_file_is_read_once_when_the_app_is_created(tmp_path):
    path = tmp_path / "config.json"
    data = tmp_path / "data.json"
    path.write_text(json.dumps({"data_file": str(data), "listen_port": 9090}))
    app = create_app(load_config(str(path)))
    path.write_text(json.dumps({"data_file": str(data), "listen_port": 1,
                                "not_a_key": True}))
    assert app.test_client().get("/api/rooms").status_code == 200
    assert app.config["RSSWITCH_WEB"]["listen_port"] == 9090
