# The service lives in fixed places, and rsswitch-web.service names the
# same ones, so they are not meant to be changed. The unit and the
# default of the app expect them.
APPDIR = /opt/rsswitch-web
CONFDIR = /etc/rsswitch-web
STATEDIR = /var/lib/rsswitch-web
SYSTEMD_UNIT_DIR = /etc/systemd/system
PYTHON ?= python3

.PHONY: all install

# The first rule, so that a bare make installs nothing: it only says
# what to run. There is nothing to build, the app is Python.
all:
	@echo "rsswitch-web has nothing to build; run make install as root, or make install DESTDIR=DIR to stage the files"

# Without DESTDIR this is the real install, which needs root, and it
# is idempotent: the system user is created only when missing, the
# code directory is replaced, and the configuration and the data are
# never overwritten, because the configuration may hold the password
# hash. The user is in no group but its own, the app needs no device.
#
# With DESTDIR only the files are copied below it, for packaging and
# tests. The user, the venv (a venv cannot be moved, it records its
# own path) and systemctl are left to the real install.
#
# The real install clears a failed state of the unit before it
# restarts it, because systemd refuses a restart within the start-limit
# interval after failed starts. That reset-failed may fail, since a
# unit that never failed makes it complain; the other systemctl calls
# report their status.
#
# The modes are set here, and not taken from the source tree, because
# a checkout on a shared folder can have group-writable files, and the
# code that a root-owned service runs must not be writable by others.
install:
	@if [ -z "$(DESTDIR)" ] && [ "$$(id -u)" != 0 ]; then \
		echo "run make install as root (sudo make install), or set DESTDIR" >&2; \
		exit 1; \
	fi
	@if [ -z "$(DESTDIR)" ]; then \
		getent passwd rsswitch-web > /dev/null || useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin rsswitch-web || exit 1; \
	fi
	rm -rf "$(DESTDIR)$(APPDIR)/rsswitch_web"
	install -d -m 0755 "$(DESTDIR)$(APPDIR)"
	cp -R rsswitch_web "$(DESTDIR)$(APPDIR)/"
	find "$(DESTDIR)$(APPDIR)/rsswitch_web" -name __pycache__ -type d -prune -exec rm -rf {} +
	find "$(DESTDIR)$(APPDIR)/rsswitch_web" -name "*.pyc" -type f -exec rm -f {} +
	find "$(DESTDIR)$(APPDIR)/rsswitch_web" -type d -exec chmod 0755 {} +
	find "$(DESTDIR)$(APPDIR)/rsswitch_web" -type f -exec chmod 0644 {} +
	install -m 0644 requirements.txt "$(DESTDIR)$(APPDIR)/requirements.txt"
	install -D -m 0644 rsswitch-web.service "$(DESTDIR)$(SYSTEMD_UNIT_DIR)/rsswitch-web.service"
	@if [ -n "$(DESTDIR)" ]; then \
		set -e; \
		install -d -m 0750 "$(DESTDIR)$(CONFDIR)" "$(DESTDIR)$(STATEDIR)"; \
		[ -e "$(DESTDIR)$(CONFDIR)/config.json" ] || install -m 0640 config.example.json "$(DESTDIR)$(CONFDIR)/config.json"; \
	else \
		set -e; \
		install -d -m 0750 -g rsswitch-web "$(CONFDIR)"; \
		[ -e "$(CONFDIR)/config.json" ] || install -m 0640 -g rsswitch-web config.example.json "$(CONFDIR)/config.json"; \
		install -d -m 0750 -o rsswitch-web -g rsswitch-web "$(STATEDIR)"; \
		[ -x "$(APPDIR)/venv/bin/python" ] || $(PYTHON) -m venv "$(APPDIR)/venv" || { echo "could not create the venv; install python3-venv and run make install again" >&2; exit 1; }; \
		"$(APPDIR)/venv/bin/pip" install -r "$(APPDIR)/requirements.txt"; \
		( umask 022 && "$(APPDIR)/venv/bin/python" -m compileall -q "$(APPDIR)/rsswitch_web" ); \
		systemctl daemon-reload; \
		systemctl enable rsswitch-web; \
		systemctl reset-failed rsswitch-web 2>/dev/null || true; \
		systemctl restart rsswitch-web; \
	fi
