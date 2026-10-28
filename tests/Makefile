CFLAGS += -Werror -Wall -W -pedantic -pthread
INCLUDES = -I..

TESTS = test_last_error test_socket_send test_codewords test_concurrency \
	test_codeword_api test_logging test_rsswitchd test_proto test_version \
	test_config test_log_handler
# The sources of the stub library. Only the ThreadSanitizer run compiles
# them into its binary, so that the library is instrumented; every other
# test links the library that the top-level Makefile builds.
LIB_SRCS = ../librsswitch.c ../boards/stub/stub.c \
	   ../boards/gpiod/gpiod_config.c

# The stub library, built by the top-level Makefile. BUILD names its
# directory, relative to this one; the default is build/stub of the
# repository. STUBLIB is the static library, which the unit tests link
# so that they still reach the test hooks of the stub board, as hidden
# visibility affects only the dynamic symbol table. STUBSO is the
# shared library, which the test builds of rsswitchd and rsssend link
# like the real programs do. They find it at run time through
# LD_LIBRARY_PATH, which `test` sets to the absolute STUBDIR for the
# commands that run a test, and for those only: exported for the whole
# makefile it would reach the compiler runs as well.
BUILD ?= ../build/stub
STUBDIR := $(abspath $(BUILD))
STUBLIB := $(BUILD)/librsswitch.a
STUBSO := $(BUILD)/librsswitch.so
TEST_DEPS = minitest.h pulse_decode.h ../librsswitch.c ../librsswitch.h \
	    ../boards/board.h ../boards/stub/stub.c ../boards/stub/stub.h \
	    ../boards/gpiod/gpiod_config.c ../boards/gpiod/gpiod_config.h

# The web app's tests run from `make test` too. WEB_DIR is the app,
# WEB_PYTHON the Python of the virtual environment that has pytest and
# the app's requirements (see ../examples/web/README.md). A venv
# elsewhere is named on the command line: make test
# WEB_PYTHON=/path/to/venv/bin/python. It must be the venv's own
# python, not the path that a symlink resolves to, or the venv's
# packages are not found. `make venv` makes that venv: it creates
# WEB_VENV with python3 -m venv when it has no pip, removing what is
# there first (for example after an interrupted run, or without
# python3-venv), and installs examples/web/requirements-dev.txt into
# it, which needs network access the first time. When the venv's python
# already imports pytest, it only prints that the venv is ready.
# WEB_VENV=/path moves both the venv and the WEB_PYTHON default.
WEB_DIR := $(abspath ../examples/web)
WEB_VENV ?= $(WEB_DIR)/venv
WEB_PYTHON ?= $(WEB_VENV)/bin/python

# test_gpiod links against libgpiod, whose
# headers are not guaranteed to be installed on every host that runs
# `make test`. GPIOD_CFLAGS/GPIOD_LIBS let a caller point at a
# different libgpiod (see "Against libgpiod 2.x" in ../README.md);
# by default they use the installed system headers/library. Probed
# once, at parse time, by compiling and linking a one-line program that
# includes <gpiod.h> against GPIOD_CFLAGS and GPIOD_LIBS, into a
# temporary file that is removed again; if that fails, both are left
# out of TESTS and one SKIP line is printed, instead of failing the
# whole `make test` run. The skip is repeated as a SKIPPED line after
# the results of `make test`, which are easy to take for a complete run,
# and the exit status does not change.
GPIOD_CFLAGS ?=
GPIOD_LIBS ?= -lgpiod
# When set to 1 or 2, the libgpiod tests refuse to compile against any
# other libgpiod major version, and a probe that does not find libgpiod
# stops make instead of skipping them. This is meant for runs against a
# libgpiod of one's own; README.md "Against libgpiod 2.x" shows one.
GPIOD_EXPECT_VERSION ?=
# A "#" inside a function call must come from a variable: GNU make 4.3
# and later keep the backslash of an escaped "\#" there, while older
# versions need it. Outside a function call the escape works in every
# version.
HASH := \#
GPIOD_AVAILABLE := $(shell t=$$(mktemp) && \
	printf '$(HASH)include <gpiod.h>\nint main(void) { return 0; }\n' | \
	$(CC) $(GPIOD_CFLAGS) -x c - -x none $(GPIOD_LIBS) -o "$$t" \
	>/dev/null 2>&1 && echo yes; rm -f "$$t")

ifeq ($(GPIOD_AVAILABLE),yes)
TESTS += test_gpiod
else
ifneq ($(GPIOD_EXPECT_VERSION),)
$(error GPIOD_EXPECT_VERSION=$(GPIOD_EXPECT_VERSION) is set, but gpiod.h or libgpiod was not found with GPIOD_CFLAGS and GPIOD_LIBS)
else
$(warning SKIP test_gpiod: gpiod.h or libgpiod not found)
GPIOD_SKIPPED := test_gpiod
endif
endif

# The ABI check of the top-level Makefile needs abidiff and a baseline
# for this machine, kept in ../abi. Without either, make test skips it
# and says so once after the results, like the libgpiod tests; nothing
# is announced while the makefile is read, and the exit status does not
# change. ABI_MACHINE is the key of the baseline, taken the way the
# top-level Makefile takes it.
ABIDIFF_FOUND := $(shell command -v abidiff 2>/dev/null)
ABI_MACHINE := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine)))
ABI_BASELINE_FOUND := $(wildcard ../abi/$(ABI_MACHINE).abi)
ifeq ($(ABIDIFF_FOUND),)
ABI_SKIPPED := abidiff not installed
else ifeq ($(ABI_BASELINE_FOUND),)
ABI_SKIPPED := no ABI baseline for $(ABI_MACHINE)
else
ABI_SKIPPED :=
endif

# The recipe of test is one shell line. GNU make runs a recipe line that
# names its make variable directly even under make -n, also inside an
# $(if) whose branch is not taken, and every dry run of test_build.sh
# would then run the whole recipe, test_build.sh included, again and
# again. Through another variable, a dry run only prints the line.
ABI_MAKE := $(MAKE)

.PHONY: test venv clean tsan FORCE

# One sub-make builds the shared and the static library, so the two
# never race under -j. It runs every time and builds only what changed.
# The shared library has no recipe of its own and depends on the static
# one, so a test program relinks only when the library changed.
$(STUBLIB): FORCE
	$(MAKE) -C .. BOARD=stub BUILD=$(STUBDIR)

$(STUBSO): $(STUBLIB)

FORCE:

# Runs the C tests, then the web app's tests, and exits non-zero when
# any of them failed. The web tests run also after a C failure, so one
# run shows both results. They are never skipped: without the venv, its
# pytest or the daemon (test_rsswitchd_bin, a prerequisite, which they
# start through RSSWITCHD_BIN) the target fails instead.
# test_rsswitchd_service.sh checks how rsswitchd is run as a service
# from examples/network, and needs no root and no system manager.
# test_build.sh checks the plumbing of this Makefile offline.
# The commands that run a test binary, test_rsssend.sh and the web
# tests get LD_LIBRARY_PATH with the stub library; the web tests also
# get BUILD, from which their fixture takes the same directory.
# test_exports.sh builds the libraries of the top-level Makefile and
# checks their exports and SONAME. test_install.sh checks its install
# and uninstall goals in a staged tree, with stand-ins for the tools
# that write.
# test_abi.sh and the abicheck goal run when abidiff and a baseline for
# this machine exist, as the ABI_SKIPPED block above explains.
test: $(TESTS) test_rsssend_bin test_rsswitchd_bin test_rsswitchd_failbin
	@status=0; \
	for t in $(TESTS); do \
		LD_LIBRARY_PATH="$(STUBDIR)" ./$$t > /dev/null || status=1; \
	done; \
	LD_LIBRARY_PATH="$(STUBDIR)" sh ./test_rsssend.sh ./test_rsssend_bin > /dev/null || status=1; \
	sh ./test_rsswitchd_service.sh ../examples/network > /dev/null || status=1; \
	sh ./test_build.sh .. > /dev/null || status=1; \
	sh ./test_exports.sh .. > /dev/null || status=1; \
	sh ./test_install.sh .. > /dev/null || status=1; \
	$(if $(ABI_SKIPPED),,$(ABI_MAKE) -s -C .. BUILD=$(STUBDIR) BOARD=stub abicheck > /dev/null || status=1; \
	LD_LIBRARY_PATH="$(STUBDIR)" sh ./test_abi.sh .. > /dev/null || status=1;) \
	py="$(abspath $(WEB_PYTHON))"; \
	if [ ! -x "$$py" ]; then \
		echo "web tests: $$py not found; run make -C tests venv (see examples/web/README.md), or make test WEB_PYTHON=/path/to/venv/bin/python" >&2; \
		status=1; \
	elif ! "$$py" -c 'import pytest' > /dev/null 2>&1; then \
		echo "web tests: pytest missing in $$py; run make -C tests venv, or install requirements-dev.txt into that venv (see examples/web/README.md)" >&2; \
		status=1; \
	else \
		(cd $(WEB_DIR) && RSSWITCHD_BIN="$(CURDIR)/test_rsswitchd_bin" \
			LD_LIBRARY_PATH="$(STUBDIR)" BUILD="$(STUBDIR)" \
			RSSWITCH_REQUIRE_DAEMON=1 \
			"$$py" -m pytest -q -p no:cacheprovider) || status=1; \
	fi; \
	$(if $(GPIOD_SKIPPED),echo "SKIPPED: $(GPIOD_SKIPPED) (libgpiod not linkable)" >&2;) \
	$(if $(ABI_SKIPPED),echo "SKIPPED: abicheck ($(ABI_SKIPPED))" >&2;) \
	exit $$status

# When both goals are given, test waits for venv, under -j as well. The
# prerequisite is declared only then, so plain `make test` never runs
# venv and still fails instead of installing anything. It is order-only
# because a newer venv must not make test run again.
ifneq ($(filter venv,$(MAKECMDGOALS)),)
test: | venv
endif

# Makes the web app's virtual environment, on request only. It is never
# a prerequisite of test, so test still fails instead of installing
# anything on its own.
venv:
	@v="$(abspath $(WEB_VENV))"; \
	if "$$v/bin/python" -c 'import pytest' > /dev/null 2>&1; then \
		echo "$$v is ready"; \
	else \
		[ -x "$$v/bin/pip" ] || { rm -rf "$$v"; python3 -m venv "$$v" || exit 1; }; \
		"$$v/bin/pip" install -r "$(WEB_DIR)/requirements-dev.txt" || exit 1; \
	fi

test_socket_send: test_socket_send.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_socket_send test_socket_send.c $(STUBLIB)

test_version: test_version.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_version test_version.c $(STUBLIB)

test_config: test_config.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_config test_config.c $(STUBLIB)

# Built with -DDEBUG like test_logging, so the notice that rss_send()
# logs for every call is among the messages. Includes ../librsswitch.c
# directly, so the library member of the archive is never pulled in a
# second time.
test_log_handler: test_log_handler.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) -DDEBUG $(INCLUDES) -o test_log_handler test_log_handler.c $(STUBLIB)

test_codewords: test_codewords.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codewords test_codewords.c $(STUBLIB)

test_concurrency: test_concurrency.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_concurrency test_concurrency.c $(STUBLIB)

# Includes ../librsswitch.c directly, so the test sees its static
# state; the library member that holds the same code is never pulled
# from the archive, because the test defines every symbol it needs.
test_codeword_api: test_codeword_api.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codeword_api test_codeword_api.c $(STUBLIB)

# Includes ../librsswitch.c directly and defines its own board, so the
# cause a board returns can be chosen per test: it links no board
# source, not even the stub board.
test_last_error: test_last_error.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_last_error test_last_error.c

# Builds ../examples/basic/rsssend.c against the shared stub library,
# so test_rsssend.sh can drive the real CLI (argument parsing, exit
# codes, stdout/stderr) end to end without any hardware. Not part of
# $(TESTS): it is a shell script, not a minitest binary, so `test`
# runs it explicitly below.
test_rsssend_bin: ../examples/basic/rsssend.c $(TEST_DEPS) $(STUBSO)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsssend_bin ../examples/basic/rsssend.c -L$(BUILD) -lrsswitch

# Builds ../examples/network/rsswitchd.c and rsswitchd_proto.c against
# the shared stub library, so test_rsswitchd.c can spawn the real
# daemon and drive it over TCP on an ephemeral port. Not part of
# $(TESTS): test_rsswitchd (below) is the minitest binary that spawns
# and drives this one; `test` builds it explicitly as a dependency of
# both.
test_rsswitchd_bin: ../examples/network/rsswitchd.c \
		     ../examples/network/rsswitchd_proto.c \
		     ../examples/network/rsswitchd_proto.h $(TEST_DEPS) \
		     $(STUBSO)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd_bin \
		../examples/network/rsswitchd.c \
		../examples/network/rsswitchd_proto.c -L$(BUILD) -lrsswitch

# Unit-tests the protocol parser with no socket or GPIO dependency of
# its own. It links the stub library anyway: the parser calls
# rss_addr_limits() for its per-device range check, and this rule
# never opens a board (see test_proto.c's test_err_names, which
# asserts stub_board_init_count() stays 0).
test_proto: test_proto.c minitest.h ../examples/network/rsswitchd_proto.c \
	    ../examples/network/rsswitchd_proto.h $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_proto test_proto.c \
		../examples/network/rsswitchd_proto.c $(STUBLIB)

# Built like test_rsswitchd_bin, but with failing_board.c in front of
# the static stub library, so test_rsswitchd's test_err_send can reach
# the daemon's ERR send reply path: a well-formed command still passes
# the daemon's own checks and reaches the library, and it is the
# board_init() call of failing_board.c that then fails. The board comes
# before the archive, so the stub board member is never pulled in and
# its functions are not defined twice.
test_rsswitchd_failbin: ../examples/network/rsswitchd.c \
			 ../examples/network/rsswitchd_proto.c \
			 ../examples/network/rsswitchd_proto.h \
			 failing_board.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd_failbin \
		../examples/network/rsswitchd.c \
		../examples/network/rsswitchd_proto.c \
		failing_board.c $(STUBLIB)

test_rsswitchd: test_rsswitchd.c minitest.h test_rsswitchd_bin \
		test_rsswitchd_failbin
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd test_rsswitchd.c

# Built with -DDEBUG so the DEBUG-only "dev: ..., group: ..., socket:
# ..., data: ..." line in rss_send() is exercised.
# Includes ../librsswitch.c directly (like test_codeword_api), so the
# library member of the archive is never pulled in a second time.
test_logging: test_logging.c $(TEST_DEPS) $(STUBLIB)
	$(CC) $(CFLAGS) -DDEBUG $(INCLUDES) -o test_logging test_logging.c $(STUBLIB)

# Intercepts, at link time, the libgpiod calls boards/gpiod/gpiod_board.c
# makes to open the chip, to get and request the line, to build the 2.x
# request and to drive the line (both API variants -- the options for
# the version not in use have no references in the link and are
# silently ignored), so test_gpiod.c's __wrap_* functions can drive
# board_init(), transmit() and board_exit() against dummy chip and
# line or request handles
# without ever opening a real chip. Belongs only to this test
# rule: it must never appear in examples/basic/Makefile or
# examples/network/Makefile, which link the real libgpiod for shipped
# binaries.
GPIOD_TEST_WRAPS = -Wl,--wrap=gpiod_line_set_value \
		   -Wl,--wrap=gpiod_line_release \
		   -Wl,--wrap=gpiod_line_request_set_value \
		   -Wl,--wrap=gpiod_line_request_release \
		   -Wl,--wrap=gpiod_chip_close \
		   -Wl,--wrap=gpiod_chip_open_by_name \
		   -Wl,--wrap=gpiod_chip_get_line \
		   -Wl,--wrap=gpiod_line_request_output \
		   -Wl,--wrap=gpiod_chip_open \
		   -Wl,--wrap=gpiod_line_settings_new \
		   -Wl,--wrap=gpiod_line_settings_free \
		   -Wl,--wrap=gpiod_line_config_new \
		   -Wl,--wrap=gpiod_line_config_free \
		   -Wl,--wrap=gpiod_line_config_add_line_settings \
		   -Wl,--wrap=gpiod_request_config_new \
		   -Wl,--wrap=gpiod_request_config_free \
		   -Wl,--wrap=gpiod_chip_request_lines

GPIOD_TEST_DEPS = test_gpiod.c minitest.h ../boards/gpiod/gpiod_board.c \
		  ../boards/gpiod/gpiod_board.h ../boards/gpiod/gpiod_config.c \
		  ../boards/gpiod/gpiod_config.h ../boards/board.h

# test_gpiod is one binary for every board that drives its line through
# libgpiod, and needs no board macro. The test includes
# ../boards/gpiod/gpiod_config.c and ../boards/gpiod/gpiod_board.c
# directly (like test_logging includes ../librsswitch.c), so it can
# capture the parser's log lines and drive board_init()/board_exit(),
# transmit() and parse_config() and their static state. It only ever
# names a chip that cannot exist for board_init(), and only ever touches
# dummy handles under GPIOD_TEST_WRAPS interception for
# transmit()/board_exit(), so it never opens a real GPIO line. Links no
# other source; GPIOD_CFLAGS/GPIOD_LIBS above select which libgpiod
# headers and library it builds and links against.
test_gpiod: $(GPIOD_TEST_DEPS)
	$(CC) $(CFLAGS) $(GPIOD_CFLAGS) $(if $(GPIOD_EXPECT_VERSION),-DEXPECT_GPIOD_VERSION=$(GPIOD_EXPECT_VERSION)) $(INCLUDES) -o test_gpiod test_gpiod.c $(GPIOD_TEST_WRAPS) $(GPIOD_LIBS)

# Optional ThreadSanitizer target. `make tsan` builds
# test_concurrency with -fsanitize=thread -g -O1 into
# test_concurrency_tsan (the name matches the tests/test_* ignore
# pattern) and runs it. It compiles the library sources (LIB_SRCS) into
# the binary instead of linking the stub library, so that the library
# is instrumented too. It is opt-in and never a prerequisite of
# `test`, since not every host ships libtsan. On kernels with high
# mmap randomization, a GCC ThreadSanitizer binary can abort with
# "unexpected memory mapping" when run directly, so it runs under
# `setarch "$(uname -m)" -R` when setarch exists, and directly
# otherwise.
tsan: test_concurrency_tsan
	@if command -v setarch > /dev/null 2>&1; then \
		setarch "$$(uname -m)" -R ./test_concurrency_tsan > /dev/null; \
	else \
		./test_concurrency_tsan > /dev/null; \
	fi

test_concurrency_tsan: test_concurrency.c $(TEST_DEPS)
	$(CC) $(CFLAGS) -fsanitize=thread -g -O1 $(INCLUDES) -o test_concurrency_tsan test_concurrency.c $(LIB_SRCS)

clean:
	-@rm -f $(TESTS) test_rsssend_bin test_rsswitchd_bin \
		test_rsswitchd_failbin test_concurrency_tsan 2>/dev/null || true
