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

import json
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import pytest

from rsswitch_web import config as config_module
from rsswitch_web.__main__ import main
from rsswitch_web.config import (DEFAULT_CONFIG_PATH, DEFAULTS, ConfigError,
                                 load_config, resolve_config)

from conftest import refused_port, sample_doc, write_doc

WEB_DIR = Path(__file__).resolve().parents[1]
LISTENING = "rsswitch-web: listening on "
DEV_WARNING = "rsswitch-web: development server only, not for production use"


class Child:
    """A python -m rsswitch_web process whose stderr is read to the end.

    Werkzeug logs one line per request on stderr, and a pipe that nobody
    empties would stop the server once it is full.
    """

    def __init__(self, *args):
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "rsswitch_web"] + list(args),
            cwd=str(WEB_DIR), stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.lines = []
        self.port = None
        self._ready = threading.Event()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        for line in self.proc.stderr:
            line = line.rstrip("\n")
            self.lines.append(line)
            if self.port is None and line.startswith(LISTENING):
                self.port = int(line.rsplit(":", 1)[1])
                self._ready.set()

    def wait_ready(self, timeout=10):
        """Wait for the listening line. Fail when the child exits first."""
        deadline = time.monotonic() + timeout
        while not self._ready.wait(0.02):
            if self.proc.poll() is not None:
                self._reader.join(2)
                pytest.fail("exited with %s: %s"
                            % (self.proc.returncode, self.lines))
            if time.monotonic() > deadline:
                pytest.fail("no listening line: %s" % self.lines)

    def stop(self):
        """Terminate the child if it still runs, and return its status."""
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            status = self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            status = self.proc.wait()
        self._reader.join(2)
        self.proc.stderr.close()
        return status


def run_cli(*args):
    """Run the command line to its end and return (status, stderr)."""
    proc = subprocess.run(
        [sys.executable, "-m", "rsswitch_web"] + list(args),
        cwd=str(WEB_DIR), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=30)
    return proc.returncode, proc.stderr


def get_json(port, path):
    """GET path from 127.0.0.1:port, without any proxy."""
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open("http://127.0.0.1:%d%s" % (port, path), timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def write_config(tmp_path, **overrides):
    """Write a config file for a data file below tmp_path, and return it."""
    data = tmp_path / "data.json"
    if not data.exists():
        write_doc(data, sample_doc())
    cfg = {
        "data_file": str(data),
        "listen_address": "127.0.0.1",
        "listen_port": 0,
        "daemon_port": refused_port(),
    }
    cfg.update(overrides)
    path = tmp_path / "config.json"
    path.write_text(json.dumps(cfg))
    return str(path)


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
    assert err.startswith("rsswitch-web: data file:")
    assert LISTENING not in err
    assert data.read_text() == "{"


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
