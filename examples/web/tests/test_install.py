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

"""Tests of make install, staged below a DESTDIR below tmp_path.

The real install needs root, a system manager and the network, so it is
not run here. The staged install copies the files and nothing else.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess

import pytest

from conftest import WEB_DIR

pytestmark = pytest.mark.skipif(shutil.which("make") is None,
                                reason="make is not installed")

# A stand-in for the commands of the real install. They only record
# that they were called.
SHIMS = ("useradd", "groupadd", "getent", "systemctl", "chgrp", "chown",
         "pip")


def make(*args, extra_path=None):
    """Run make in the web directory; return the CompletedProcess."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "DESTDIR")}
    if extra_path:
        env["PATH"] = extra_path + os.pathsep + env["PATH"]
    return subprocess.run(
        ["make"] + list(args), cwd=str(WEB_DIR), env=env,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=120)


def staged(dest):
    proc = make("install", "DESTDIR=%s" % dest)
    assert proc.returncode == 0, proc.stderr
    return proc


def mode(path):
    return stat.S_IMODE(os.lstat(str(path)).st_mode)


def tree(root):
    """Return the relative paths of everything below root, sorted."""
    found = []
    for base, dirs, files in os.walk(str(root)):
        for name in dirs + files:
            found.append(os.path.relpath(os.path.join(base, name), str(root)))
    return sorted(found)


def source_files():
    """Return the files of the package in the source tree, as relative
    paths, without the byte code that a test run leaves there."""
    found = []
    for base, dirs, files in os.walk(str(WEB_DIR / "rsswitch_web")):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for name in files:
            if not name.endswith(".pyc"):
                found.append(os.path.relpath(
                    os.path.join(base, name), str(WEB_DIR / "rsswitch_web")))
    return sorted(found)


def test_staged_install_lays_out_the_tree(tmp_path):
    staged(tmp_path)
    app = tmp_path / "opt" / "rsswitch-web"
    code = app / "rsswitch_web"
    # Every directory is 0755 and every file 0644, whatever the modes
    # of the source tree are, and nothing is byte code or a venv.
    assert mode(app) == 0o755
    for base, dirs, files in os.walk(str(app)):
        for name in dirs:
            assert mode(os.path.join(base, name)) == 0o755, (base, name)
            assert name != "__pycache__"
        for name in files:
            assert mode(os.path.join(base, name)) == 0o644, (base, name)
            assert not name.endswith(".pyc")
    assert not (app / "venv").exists()
    # The package is complete, static assets included.
    assert sorted(os.path.relpath(os.path.join(base, name), str(code))
                  for base, _, files in os.walk(str(code))
                  for name in files) == source_files()
    assert (code / "static" / "index.html").is_file()
    assert (code / "__main__.py").is_file()
    assert (app / "requirements.txt").read_bytes() \
        == (WEB_DIR / "requirements.txt").read_bytes()
    # The unit, the configuration and the directories of the service.
    unit = tmp_path / "etc" / "systemd" / "system" / "rsswitch-web.service"
    assert unit.read_bytes() == (WEB_DIR / "rsswitch-web.service").read_bytes()
    assert mode(unit) == 0o644
    config = tmp_path / "etc" / "rsswitch-web" / "config.json"
    assert config.read_bytes() \
        == (WEB_DIR / "config.example.json").read_bytes()
    assert mode(config) == 0o640
    assert mode(config.parent) == 0o750
    state = tmp_path / "var" / "lib" / "rsswitch-web"
    assert state.is_dir() and mode(state) == 0o750
    assert list(state.iterdir()) == []


def test_staged_install_creates_nothing_else(tmp_path):
    assert tree(tmp_path) == []
    staged(tmp_path)
    top = sorted(p.name for p in tmp_path.iterdir())
    assert top == ["etc", "opt", "var"]
    assert sorted(p.name for p in (tmp_path / "etc").iterdir()) \
        == ["rsswitch-web", "systemd"]
    assert sorted(p.name for p in (tmp_path / "opt").iterdir()) \
        == ["rsswitch-web"]


def test_staged_install_creates_no_user_and_runs_no_systemctl(tmp_path):
    shims = tmp_path / "shims"
    shims.mkdir()
    log = tmp_path / "calls.log"
    for name in SHIMS:
        path = shims / name
        path.write_text('#!/bin/sh\necho %s >> "%s"\n' % (name, log))
        path.chmod(0o755)
    dest = tmp_path / "dest"
    proc = make("install", "DESTDIR=%s" % dest, "PYTHON=/nonexistent/python",
                extra_path=str(shims))
    assert proc.returncode == 0, proc.stderr
    assert not log.exists()
    assert not (dest / "opt" / "rsswitch-web" / "venv").exists()


def test_reinstall_keeps_config_and_data(tmp_path):
    staged(tmp_path)
    config = tmp_path / "etc" / "rsswitch-web" / "config.json"
    config.write_text('{"username": "owner", "listen_port": 9000}\n')
    config.chmod(0o600)
    state = tmp_path / "var" / "lib" / "rsswitch-web"
    data = state / "data.json"
    data.write_bytes(b'{"version": 1, "rooms": []}\n')
    venv = tmp_path / "opt" / "rsswitch-web" / "venv" / "bin"
    venv.mkdir(parents=True)
    (venv / "python").write_text("kept")
    code = tmp_path / "opt" / "rsswitch-web" / "rsswitch_web"
    (code / "stale.py").write_text("# a module that was removed\n")
    (code / "static" / "gone.css").write_text("/* removed */\n")
    before = (config.read_bytes(), data.read_bytes())
    staged(tmp_path)
    assert (config.read_bytes(), data.read_bytes()) == before
    assert mode(config) == 0o600
    assert not (code / "stale.py").exists()
    assert not (code / "static" / "gone.css").exists()
    assert (venv / "python").read_text() == "kept"
    assert (code / "app.py").is_file()


def test_make_without_a_target_installs_nothing(tmp_path):
    listing = sorted(os.listdir(str(WEB_DIR)))
    proc = make("DESTDIR=%s" % tmp_path)
    assert proc.returncode == 0, proc.stderr
    assert "make install" in proc.stdout
    assert tree(tmp_path) == []
    assert sorted(os.listdir(str(WEB_DIR))) == listing


@pytest.mark.skipif(os.geteuid() == 0, reason="would install for real")
def test_unstaged_install_needs_root():
    proc = make("install")
    assert proc.returncode != 0
    assert "run make install as root" in proc.stderr
    assert "DESTDIR" in proc.stderr


def test_makefile_never_adds_the_user_to_a_group():
    text = (WEB_DIR / "Makefile").read_text()
    assert "gpio" not in text
    assert "--groups" not in text
    assert " -G " not in text


def test_install_resets_the_unit_before_the_restart():
    """A unit that hit its start limit refuses a restart until it is
    reset. Only that call may fail silently."""
    lines = [line.strip() for line in
             (WEB_DIR / "Makefile").read_text().splitlines()]
    restart = [i for i, line in enumerate(lines)
               if line.startswith("systemctl restart rsswitch-web")]
    assert len(restart) == 1
    assert lines[restart[0] - 1] == ("systemctl reset-failed rsswitch-web "
                                     "2>/dev/null || true; \\")
    silenced = [line for line in lines
                if line.startswith("systemctl") and "|| true" in line]
    assert len(silenced) == 1
