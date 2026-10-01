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

"""The command line: python -m rsswitch_web [--config PATH] COMMAND

The commands are run, which serves the app, and hash-password, which
turns a password into the hash that goes into the configuration.

The run command is for development and testing. It serves with the
development server of Werkzeug, which is not meant to face a network
that anyone can reach. A production setup runs the app under a real
WSGI server.

Exit status is 0 for --help or a clean stop by SIGTERM or SIGINT, 1
for a data file that cannot be used or an address that cannot be
listened on, and 2 for a usage or configuration error. The
hash-password command exits with 1 when no usable password was typed
and with 2 for a method that cannot be used. Every
diagnostic is one line on stderr that starts with "rsswitch-web: ".
"""

from __future__ import annotations

import argparse
import getpass
import signal
import socket
import sys

from werkzeug.security import generate_password_hash
from werkzeug.serving import make_server

from . import create_app
from .config import (DEFAULT_CONFIG_PATH, ConfigError, load_config,
                     resolve_config)
from .store import StoreError


def _say(text):
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


def _stop(signum, frame):
    raise KeyboardInterrupt


def _listen(address, port):
    """Return a socket that listens on address and port.

    The server of Werkzeug would bind by itself, but it answers a
    failure with its own text and exits. Binding here keeps the
    diagnostic in the format of this program.
    """
    family = socket.AF_INET6 if ":" in address else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_STREAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((address, port))
        sock.listen(128)
    except OSError:
        sock.close()
        raise
    return sock


def _run(args) -> int:
    try:
        app = create_app(load_config(args.config))
    except ConfigError as e:
        _say("rsswitch-web: %s" % e)
        return 2
    except StoreError as e:
        _say("rsswitch-web: data file: %s" % e)
        return 1
    cfg = app.config["RSSWITCH_WEB"]
    try:
        sock = _listen(cfg["listen_address"], cfg["listen_port"])
    except OSError as e:
        _say("rsswitch-web: cannot listen on %s:%s: %s"
             % (cfg["listen_address"], cfg["listen_port"], e.strerror))
        return 1
    try:
        server = make_server(cfg["listen_address"], cfg["listen_port"], app,
                             threaded=True, fd=sock.fileno())
    finally:
        # The server works on a copy of the descriptor.
        sock.close()
    host, port = server.server_address[:2]
    try:
        # The handler is in place before the readiness line, so that a
        # signal sent as soon as a script has read it stops the server
        # cleanly.
        signal.signal(signal.SIGTERM, _stop)
        _say("rsswitch-web: development server only, "
             "not for production use")
        _say("rsswitch-web: listening on %s:%s" % (host, port))
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def _hash_password(args) -> int:
    """Print the hash of a password that is typed twice, without echo.

    The password never goes through the command line, where the shell
    history and the process list would keep it, and is never printed.
    The configuration file is not read.
    """
    try:
        first = getpass.getpass("Password: ")
        second = getpass.getpass("Repeat password: ")
    except EOFError:
        _say("rsswitch-web: no password given")
        return 1
    if first != second:
        _say("rsswitch-web: passwords do not match")
        return 1
    if not first:
        _say("rsswitch-web: refusing an empty password")
        return 1
    try:
        if args.method:
            hashed = generate_password_hash(first, method=args.method)
        else:
            hashed = generate_password_hash(first)
    except UnicodeEncodeError:
        # A password from a terminal with another encoding can hold a
        # lone surrogate, which has no UTF-8 form. It is not a problem
        # of --method, although UnicodeEncodeError is a ValueError.
        _say("rsswitch-web: password is not valid text")
        return 1
    except ValueError as e:
        _say("rsswitch-web: invalid --method: %s" % e)
        return 2
    # A method that Werkzeug accepts, but that the app would refuse at
    # start-up, is reported now and not when the app is started.
    try:
        resolve_config({"password_hash": hashed})
    except ConfigError as e:
        _say("rsswitch-web: %s" % e)
        return 2
    print(hashed)
    return 0


def main(argv=None) -> int:
    """Run the command line and return the exit status."""
    parser = argparse.ArgumentParser(prog="python -m rsswitch_web",
                                     description="Web app for rsswitchd")
    parser.add_argument("--config", metavar="PATH",
                        help="JSON configuration file (default: %s)"
                        % DEFAULT_CONFIG_PATH)
    commands = parser.add_subparsers(dest="command", required=True,
                                     metavar="COMMAND")
    run = commands.add_parser("run", help="serve the app with the "
                              "development server")
    run.set_defaults(func=_run)
    hashing = commands.add_parser(
        "hash-password",
        help="print the hash of a password for password_hash")
    hashing.add_argument(
        "--method", metavar="METHOD",
        help="hash method, scrypt by default, which needs about 32 MiB "
        "per check; pbkdf2:sha256:600000 is cheaper for slow boards")
    hashing.set_defaults(func=_hash_password)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
