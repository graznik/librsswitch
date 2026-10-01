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

"""Tests of the command line: config file, exit status, run."""

from __future__ import annotations

import base64
import json
import signal
import socket
import urllib.error
import urllib.request

import pytest
from werkzeug.security import check_password_hash, generate_password_hash

from rsswitch_web import config as config_module
from rsswitch_web.__main__ import main
from rsswitch_web.config import (DEFAULT_CONFIG_PATH, DEFAULTS, ConfigError,
                                 load_config, resolve_config)

from conftest import (DEV_WARNING, LISTENING, WEB_DIR, Child, get_json,
                      run_cli, write_config)


def test_run_serves_api_from_config_file(tmp_path):
    child = Child("--config", write_config(tmp_path), "run")
    try:
        child.wait_ready()
        assert child.lines[:2] == [DEV_WARNING,
                                   LISTENING + "127.0.0.1:%d" % child.port]
        assert child.port > 0
        tree = get_json(child.port, "/api/rooms")
        assert [r["name"] for r in tree["rooms"]] == ["Wohnzimmer", "Küche"]
        child.proc.send_signal(signal.SIGTERM)
        assert child.proc.wait(5) == 0
    finally:
        child.stop()


def test_run_stops_on_sigint_with_status_0(tmp_path):
    child = Child("--config", write_config(tmp_path), "run")
    try:
        child.wait_ready()
        child.proc.send_signal(signal.SIGINT)
        assert child.proc.wait(5) == 0
    finally:
        child.stop()


def test_run_explicit_missing_config_exits_2(tmp_path):
    missing = str(tmp_path / "nope.json")
    status, err = run_cli("--config", missing, "run")
    assert status == 2
    assert err.startswith("rsswitch-web: config: cannot read %s: " % missing)
    assert err.count("\n") == 1


def test_run_invalid_json_config_exits_2(tmp_path):
    path = tmp_path / "config.json"
    path.write_text("{")
    status, err = run_cli("--config", str(path), "run")
    assert status == 2
    assert err.startswith("rsswitch-web: config: %s: not valid JSON: " % path)


def test_run_non_object_config_exits_2(tmp_path):
    path = tmp_path / "config.json"
    path.write_text("[1, 2]")
    status, err = run_cli("--config", str(path), "run")
    assert status == 2
    assert err == ("rsswitch-web: config: %s: must be a JSON object\n"
                   % path)


def test_run_with_typo_key_exits_2_with_suggestion(tmp_path):
    path = tmp_path / "config.json"
    path.write_text(json.dumps({"pasword_hash": "scrypt:32768:8:1$s$h"}))
    status, err = run_cli("--config", str(path), "run")
    assert status == 2
    assert err == ("rsswitch-web: config: unknown key 'pasword_hash' "
                   "(did you mean 'password_hash'?)\n")


def test_run_corrupt_data_file_exits_1(tmp_path):
    data = tmp_path / "data.json"
    data.write_text("{")
    status, err = run_cli("--config", write_config(tmp_path), "run")
    assert status == 1
    assert err.startswith("rsswitch-web: data file ")
    assert str(data) in err
    assert "data file: data file" not in err
    assert LISTENING not in err
    assert data.read_text() == "{"


def test_run_hash_of_the_empty_password_exits_2(tmp_path):
    config = write_config(tmp_path, password_hash=generate_password_hash(
        "", "pbkdf2:sha256:1000"))
    status, err = run_cli("--config", config, "run")
    assert status == 2
    assert err.startswith("rsswitch-web: config: key 'password_hash': "
                          "is the hash of an empty password")
    assert LISTENING not in err


def test_run_port_in_use_exits_1(tmp_path):
    taken = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        taken.bind(("127.0.0.1", 0))
        taken.listen(1)
        port = taken.getsockname()[1]
        status, err = run_cli(
            "--config", write_config(tmp_path, listen_port=port), "run")
    finally:
        taken.close()
    assert status == 1
    assert err.startswith("rsswitch-web: cannot listen on 127.0.0.1:%d: "
                          % port)
    # No readiness line for a server that is not listening.
    assert LISTENING not in err


def test_load_config_default_missing_warns_and_returns_empty(
        tmp_path, monkeypatch, capsys):
    path = str(tmp_path / "config.json")
    monkeypatch.setattr(config_module, "DEFAULT_CONFIG_PATH", path)
    assert load_config() == {}
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err == ("rsswitch-web: warning: %s not found, "
                       "using built-in defaults\n" % path)


def test_load_config_explicit_missing_raises(tmp_path):
    with pytest.raises(ConfigError, match="^config: cannot read .*nope"):
        load_config(str(tmp_path / "nope.json"))


def test_load_config_directory_raises(tmp_path):
    with pytest.raises(ConfigError, match="^config: cannot read "):
        load_config(str(tmp_path))


def test_load_config_returns_the_mapping_unchecked(tmp_path):
    path = tmp_path / "config.json"
    path.write_text('{"listen_port": "x", "whatever": 1}')
    assert load_config(str(path)) == {"listen_port": "x", "whatever": 1}


@pytest.mark.parametrize("text", ["NaN", '{"listen_port": NaN}', "",
                                  '{"a": 1} x'],
                         ids=["nan", "nan-value", "empty", "trailing"])
def test_load_config_refuses_bad_json(tmp_path, text):
    path = tmp_path / "config.json"
    path.write_text(text)
    with pytest.raises(ConfigError, match="not valid JSON"):
        load_config(str(path))


def test_load_config_refuses_bytes_that_are_not_utf8(tmp_path):
    path = tmp_path / "config.json"
    path.write_bytes(b'{"data_file": "\xff"}')
    with pytest.raises(ConfigError, match="not valid JSON"):
        load_config(str(path))


def test_no_subcommand_exits_2(capsys):
    with pytest.raises(SystemExit) as exc:
        main([])
    assert exc.value.code == 2
    assert "required" in capsys.readouterr().err


def test_help_exits_0(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["--help"])
    assert exc.value.code == 0
    out = capsys.readouterr().out
    assert "python -m rsswitch_web" in out
    assert "--config" in out
    assert "run" in out


def test_config_example_matches_defaults():
    path = WEB_DIR / "config.example.json"
    text = path.read_text()
    example = json.loads(text)
    assert list(example) == list(DEFAULTS)
    assert example == DEFAULTS
    assert resolve_config(example) == DEFAULTS
    assert text == json.dumps(example, indent=2) + "\n"
    assert DEFAULT_CONFIG_PATH == "/etc/rsswitch-web/config.json"


def type_passwords(monkeypatch, *answers):
    """Make getpass return the answers in turn, and raise EOFError after."""
    pending = list(answers)
    asked = []

    def fake(prompt="Password: ", stream=None):
        asked.append(prompt)
        if not pending:
            raise EOFError
        return pending.pop(0)

    monkeypatch.setattr("rsswitch_web.__main__.getpass.getpass", fake)
    return asked


def test_hash_password_prints_one_verifiable_hash(monkeypatch, capsys):
    asked = type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password"]) == 0
    out = capsys.readouterr()
    assert asked == ["Password: ", "Repeat password: "]
    lines = out.out.splitlines()
    assert len(lines) == 1 and out.out.endswith("\n")
    assert check_password_hash(lines[0], "s3cret")
    assert not check_password_hash(lines[0], "s3cre")
    assert resolve_config({"password_hash": lines[0]})["username"] == "admin"
    assert "s3cret" not in out.out
    assert "s3cret" not in out.err


def test_hash_password_default_is_scrypt(monkeypatch, capsys):
    type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password"]) == 0
    assert capsys.readouterr().out.startswith("scrypt:")


def test_hash_password_method_pbkdf2(monkeypatch, capsys):
    type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password", "--method", "pbkdf2:sha256:600000"]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith("pbkdf2:sha256:600000$")
    assert check_password_hash(line, "s3cret")
    assert resolve_config({"password_hash": line})["password_hash"] == line


def test_hash_password_mismatch_exits_1_and_prints_nothing(
        monkeypatch, capsys):
    type_passwords(monkeypatch, "s3cret", "s3cret2")
    assert main(["hash-password"]) == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err == "rsswitch-web: passwords do not match\n"


def test_hash_password_empty_exits_1(monkeypatch, capsys):
    type_passwords(monkeypatch, "", "")
    assert main(["hash-password"]) == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err == "rsswitch-web: refusing an empty password\n"


@pytest.mark.parametrize("method", [None, "pbkdf2:sha256:1000"],
                         ids=["default", "pbkdf2"])
def test_hash_password_not_valid_text_exits_1(monkeypatch, capsys, method):
    type_passwords(monkeypatch, "pw\udcff", "pw\udcff")
    argv = ["hash-password"]
    if method:
        argv += ["--method", method]
    assert main(argv) == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err == "rsswitch-web: password is not valid text\n"


def test_hash_password_eof_exits_1(monkeypatch, capsys):
    type_passwords(monkeypatch)
    assert main(["hash-password"]) == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err == "rsswitch-web: no password given\n"


def test_hash_password_eof_at_the_repeat_exits_1(monkeypatch, capsys):
    type_passwords(monkeypatch, "s3cret")
    assert main(["hash-password"]) == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert "s3cret" not in out.err


@pytest.mark.parametrize("method", ["md5", "nonsense", "pbkdf2:nope:1",
                                    "scrypt:x:y:z"])
def test_hash_password_invalid_method_exits_2(monkeypatch, capsys, method):
    type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password", "--method", method]) == 2
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err.startswith("rsswitch-web: invalid --method: ")
    assert "s3cret" not in out.err


def test_hash_password_refuses_a_hash_the_app_would_refuse(
        monkeypatch, capsys):
    # Werkzeug accepts this method, the app does not start with its hash.
    type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password", "--method", "pbkdf2:shake_128:1"]) == 2
    out = capsys.readouterr()
    assert out.out == ""
    assert out.err.startswith("rsswitch-web: ")
    assert "s3cret" not in out.err


def test_hash_password_reads_no_config(monkeypatch, tmp_path, capsys):
    type_passwords(monkeypatch, "s3cret", "s3cret")
    missing = str(tmp_path / "nope.json")
    assert main(["--config", missing, "hash-password"]) == 0
    out = capsys.readouterr()
    assert len(out.out.splitlines()) == 1
    assert out.err == ""


def test_hash_password_takes_no_password_argument(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["hash-password", "s3cret"])
    assert exc.value.code == 2
    assert "s3cret" not in capsys.readouterr().out


def test_generated_hash_locks_the_running_app(tmp_path, monkeypatch, capsys):
    type_passwords(monkeypatch, "s3cret", "s3cret")
    assert main(["hash-password"]) == 0
    hashed = capsys.readouterr().out.strip()
    config = write_config(tmp_path, password_hash=hashed)
    child = Child("--config", config, "run")
    try:
        child.wait_ready()
        url = "http://127.0.0.1:%d/api/rooms" % child.port
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with pytest.raises(urllib.error.HTTPError) as exc:
            opener.open(url, timeout=5)
        assert exc.value.code == 401
        assert exc.value.headers["WWW-Authenticate"].startswith("Basic ")
        exc.value.close()
        token = base64.b64encode(b"admin:s3cret").decode("ascii")
        req = urllib.request.Request(
            url, headers={"Authorization": "Basic " + token})
        with opener.open(req, timeout=5) as r:
            assert r.status == 200
            tree = json.loads(r.read().decode("utf-8"))
        assert [r["name"] for r in tree["rooms"]] == ["Wohnzimmer", "Küche"]
        bad = base64.b64encode(b"admin:s3cre").decode("ascii")
        req = urllib.request.Request(
            url, headers={"Authorization": "Basic " + bad})
        with pytest.raises(urllib.error.HTTPError) as exc:
            opener.open(req, timeout=5)
        assert exc.value.code == 401
        exc.value.close()
        child.proc.send_signal(signal.SIGTERM)
        assert child.proc.wait(5) == 0
    finally:
        child.stop()
