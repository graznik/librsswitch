CFLAGS += -Werror -Wall -W -pedantic -DX86_64 -pthread
INCLUDES = -I..

TESTS = test_socket_send test_codewords test_concurrency test_codeword_api \
	test_logging test_rsswitchd test_proto
LIB_SRCS = ../librsswitch.c ../boards/x86_64/x86_64.c
TEST_DEPS = minitest.h pulse_decode.h ../librsswitch.c ../librsswitch.h \
	    ../boards/board.h ../boards/x86_64/x86_64.c ../boards/x86_64/x86_64.h

# The web app's tests run from `make test` too. WEB_DIR is the app,
# WEB_PYTHON the Python of the virtual environment that has pytest and
# the app's requirements (see ../examples/web/README.md). A venv
# elsewhere is named on the command line: make test
# WEB_PYTHON=/path/to/venv/bin/python. It must be the venv's own
# python, not the path that a symlink resolves to, or the venv's
# packages are not found. `make venv` makes that venv: it creates
# WEB_VENV with python3 -m venv when it has no python, and installs
# examples/web/requirements-dev.txt into it, which needs network access
# the first time. When the venv's python already imports pytest, it
# only prints that the venv is ready. WEB_VENV=/path moves both the
# venv and the WEB_PYTHON default.
WEB_DIR := $(abspath ../examples/web)
WEB_VENV ?= $(WEB_DIR)/venv
WEB_PYTHON ?= $(WEB_VENV)/bin/python

# test_gpiod_rpi and test_gpiod_bbb link against libgpiod, whose
# headers are not guaranteed to be installed on every host that runs
# `make test`. GPIOD_CFLAGS/GPIOD_LIBS let a caller point at a
# different libgpiod (see the D-15 verification in 01-05-SUMMARY.md);
# by default they use the installed system headers/library. Probed
# once, at parse time, by asking the compiler whether it can
# preprocess a one-line <gpiod.h> include; if it can't, both are left
# out of TESTS and one SKIP line is printed, instead of failing the
# whole `make test` run.
GPIOD_CFLAGS ?=
GPIOD_LIBS ?= -lgpiod
# When set to 1 or 2, the libgpiod tests refuse to compile against any
# other libgpiod major version, and a probe that does not find libgpiod
# stops make instead of skipping them. This is meant for runs against a
# libgpiod of one's own; README.md "Against libgpiod 2.x" shows one.
GPIOD_EXPECT_VERSION ?=
GPIOD_AVAILABLE := $(shell printf '\#include <gpiod.h>\n' | \
	$(CC) $(GPIOD_CFLAGS) -E - >/dev/null 2>&1 && echo yes)

ifeq ($(GPIOD_AVAILABLE),yes)
TESTS += test_gpiod_rpi test_gpiod_bbb
else
ifneq ($(GPIOD_EXPECT_VERSION),)
$(error GPIOD_EXPECT_VERSION=$(GPIOD_EXPECT_VERSION) is set, but gpiod.h was not found with GPIOD_CFLAGS)
else
$(warning SKIP test_gpiod_rpi test_gpiod_bbb: gpiod.h not found)
endif
endif

.PHONY: test venv clean tsan

# Runs the C tests, then the web app's tests, and exits non-zero when
# any of them failed. The web tests run also after a C failure, so one
# run shows both results. They are never skipped: without the venv, its
# pytest or the daemon (test_rsswitchd_bin, a prerequisite, which they
# start through RSSWITCHD_BIN) the target fails instead.
# test_rsswitchd_service.sh checks how rsswitchd is run as a service
# from examples/network, and needs no root and no system manager.
test: $(TESTS) test_rsssend_bin test_rsswitchd_bin test_rsswitchd_failbin
	@status=0; \
	for t in $(TESTS); do \
		./$$t > /dev/null || status=1; \
	done; \
	sh ./test_rsssend.sh ./test_rsssend_bin > /dev/null || status=1; \
	sh ./test_rsswitchd_service.sh ../examples/network > /dev/null || status=1; \
	py="$(abspath $(WEB_PYTHON))"; \
	if [ ! -x "$$py" ]; then \
		echo "web tests: $$py not found; run make -C tests venv (see examples/web/README.md), or make test WEB_PYTHON=/path/to/venv/bin/python" >&2; \
		status=1; \
	elif ! "$$py" -c 'import pytest' > /dev/null 2>&1; then \
		echo "web tests: pytest missing in $$py; run make -C tests venv, or install requirements-dev.txt into that venv (see examples/web/README.md)" >&2; \
		status=1; \
	else \
		(cd $(WEB_DIR) && RSSWITCHD_BIN="$(CURDIR)/test_rsswitchd_bin" \
			RSSWITCH_REQUIRE_DAEMON=1 \
			"$$py" -m pytest -q -p no:cacheprovider) || status=1; \
	fi; \
	exit $$status

# Makes the web app's virtual environment, on request only. It is never
# a prerequisite of test, so test still fails instead of installing
# anything on its own.
venv:
	@v="$(abspath $(WEB_VENV))"; \
	if "$$v/bin/python" -c 'import pytest' > /dev/null 2>&1; then \
		echo "$$v is ready"; \
	else \
		[ -x "$$v/bin/python" ] || python3 -m venv "$$v" || exit 1; \
		"$$v/bin/pip" install -r "$(WEB_DIR)/requirements-dev.txt" || exit 1; \
	fi

test_socket_send: test_socket_send.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_socket_send test_socket_send.c $(LIB_SRCS)

test_codewords: test_codewords.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codewords test_codewords.c $(LIB_SRCS)

test_concurrency: test_concurrency.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_concurrency test_concurrency.c $(LIB_SRCS)

test_codeword_api: test_codeword_api.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codeword_api test_codeword_api.c ../boards/x86_64/x86_64.c

# Builds ../examples/basic/rsssend.c against the x86_64 board, so
# test_rsssend.sh can drive the real CLI (argument parsing, exit
# codes, stdout/stderr) end to end without any hardware. Not part of
# $(TESTS): it is a shell script, not a minitest binary, so `test`
# runs it explicitly below.
test_rsssend_bin: ../examples/basic/rsssend.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsssend_bin ../examples/basic/rsssend.c $(LIB_SRCS)

# Builds ../examples/network/rsswitchd.c and rsswitchd_proto.c against
# the x86_64 board, so test_rsswitchd.c can spawn the real daemon and
# drive it over TCP on an ephemeral port. Not part of $(TESTS):
# test_rsswitchd (below) is the minitest binary that spawns and drives
# this one; `test` builds it explicitly as a dependency of both.
test_rsswitchd_bin: ../examples/network/rsswitchd.c \
		     ../examples/network/rsswitchd_proto.c \
		     ../examples/network/rsswitchd_proto.h $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd_bin \
		../examples/network/rsswitchd.c \
		../examples/network/rsswitchd_proto.c $(LIB_SRCS)

# PROTO-07: unit-tests the protocol parser with no socket or GPIO
# dependency of its own. It links $(LIB_SRCS) anyway: from plan 02-03
# on, the parser calls socket_addr_limits() for its per-device range
# check, and this rule never opens a board (see test_proto.c's
# test_err_names, which asserts x86_64_board_init_count() stays 0).
test_proto: test_proto.c minitest.h ../examples/network/rsswitchd_proto.c \
	    ../examples/network/rsswitchd_proto.h $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_proto test_proto.c \
		../examples/network/rsswitchd_proto.c $(LIB_SRCS)

# Built like test_rsswitchd_bin, but against ../librsswitch.c and
# failing_board.c instead of $(LIB_SRCS)'s x86_64 board, so
# test_rsswitchd's test_err_send can reach the daemon's ERR send
# reply path (D-12/D-14): a well-formed command still passes the
# daemon's own checks and reaches the library, and it is the
# library's board_init() call that then fails.
test_rsswitchd_failbin: ../examples/network/rsswitchd.c \
			 ../examples/network/rsswitchd_proto.c \
			 ../examples/network/rsswitchd_proto.h \
			 failing_board.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd_failbin \
		../examples/network/rsswitchd.c \
		../examples/network/rsswitchd_proto.c \
		../librsswitch.c failing_board.c

test_rsswitchd: test_rsswitchd.c minitest.h test_rsswitchd_bin \
		test_rsswitchd_failbin
	$(CC) $(CFLAGS) $(INCLUDES) -o test_rsswitchd test_rsswitchd.c

# Built with -DDEBUG so the DEBUG-only "dev: ..., group: ..., socket:
# ..., data: ..." line in socket_send() is exercised (LIB-02).
# Includes ../librsswitch.c directly (like test_codeword_api), so it
# links only the x86_64 board, not librsswitch.c a second time.
test_logging: test_logging.c $(TEST_DEPS)
	$(CC) $(CFLAGS) -DDEBUG $(INCLUDES) -o test_logging test_logging.c ../boards/x86_64/x86_64.c

# Intercepts, at link time, the libgpiod calls boards/gpiod/gpiod_board.c
# makes to open the chip, to get and request the line, to build the 2.x
# request and to drive the line (both API variants -- the options for
# the version not in use have no references in the link and are
# silently ignored), so test_gpiod.c's __wrap_* functions can drive
# board_init(), transmit() and board_exit() against dummy chip and
# line or request handles
# without ever opening a real chip (G-01-5). Belongs only to this test
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
		  ../boards/gpiod/gpiod_board.h ../boards/rpi/rpi.h \
		  ../boards/bbb/bbb.h ../boards/board.h

# test_gpiod_rpi and test_gpiod_bbb are built from the one test file
# test_gpiod.c, once per board macro. The test includes
# ../boards/gpiod/gpiod_board.c directly (like test_logging includes
# ../librsswitch.c), so it can drive board_init()/board_exit(),
# transmit() and parse_config() and their static state. It needs a
# board macro, because gpiod_board.h picks the default chip
# and line by it. The -DX86_64 from CFLAGS does no harm, because the
# test includes no librsswitch.c. Only ever names a chip that cannot
# exist for board_init(), and only ever touches dummy handles under
# GPIOD_TEST_WRAPS interception for transmit()/board_exit(), so it
# never opens a real GPIO line. Links no other source;
# GPIOD_CFLAGS/GPIOD_LIBS above select which libgpiod headers and
# library it builds and links against.
test_gpiod_rpi: $(GPIOD_TEST_DEPS)
	$(CC) $(CFLAGS) -DRASPBERRY_PI $(GPIOD_CFLAGS) $(if $(GPIOD_EXPECT_VERSION),-DEXPECT_GPIOD_VERSION=$(GPIOD_EXPECT_VERSION)) $(INCLUDES) -o test_gpiod_rpi test_gpiod.c $(GPIOD_TEST_WRAPS) $(GPIOD_LIBS)

test_gpiod_bbb: $(GPIOD_TEST_DEPS)
	$(CC) $(CFLAGS) -DBEAGLEBONE_BLACK $(GPIOD_CFLAGS) $(if $(GPIOD_EXPECT_VERSION),-DEXPECT_GPIOD_VERSION=$(GPIOD_EXPECT_VERSION)) $(INCLUDES) -o test_gpiod_bbb test_gpiod.c $(GPIOD_TEST_WRAPS) $(GPIOD_LIBS)

# Optional ThreadSanitizer target (D-10). `make tsan` builds
# test_concurrency with -fsanitize=thread -g -O1 into
# test_concurrency_tsan (the name matches the tests/test_* ignore
# pattern) and runs it. It is opt-in and never a prerequisite of
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
