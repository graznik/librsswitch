/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Drives the real x86_64 rsswitchd binary over TCP: spawns it on an
 * ephemeral port, waits for its stderr "listening on" line, connects
 * as a real client and reads the codewords it prints on stdout. The
 * contract this relies on -- "--port 0", the stderr listening line,
 * codewords on stdout -- is also the contract Phase 4's Python
 * end-to-end fixture uses, so keep the two in sync.
 *
 * Every helper below is static and reusable: daemon_spawn()/
 * daemon_wait_ready()/daemon_start() start a long-running daemon,
 * daemon_run() drives a command line that exits by itself (--help,
 * an invalid option), daemon_stdout_line()/daemon_stop() read output
 * from and stop a running daemon, and client_connect()/client_send()
 * are the TCP client side. An atexit() handler and a watchdog alarm
 * guarantee every spawned daemon is reaped, even if a check fails or
 * this test itself is killed.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "minitest.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX_LIVE_PIDS 64
#define OUT_BUF_LEN 256
#define LISTEN_LINE_LEN 128

/* Overridable with argv[1] of this test binary. */
static const char *daemon_path = "./test_rsswitchd_bin";

static pid_t live_pids[MAX_LIVE_PIDS];
static int live_count;

struct daemon {
	pid_t pid;
	int out_fd;			/* daemon's stdout, read end */
	int err_fd;			/* daemon's stderr, read end */
	int port;			/* parsed from the listening line */
	char listen_line[LISTEN_LINE_LEN];
	char out_buf[OUT_BUF_LEN];	/* stdout line accumulator */
	size_t out_len;
	int out_eof;
};

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void live_add(pid_t pid)
{
	if (live_count < (int)ARRAY_SIZE(live_pids))
		live_pids[live_count++] = pid;
}

static void live_remove(pid_t pid)
{
	int i;

	for (i = 0; i < live_count; i++) {
		if (live_pids[i] == pid) {
			live_pids[i] = live_pids[live_count - 1];
			live_count--;
			return;
		}
	}
}

/* Registered with atexit(): kills and reaps every daemon this test
 * spawned and never explicitly stopped, including on a failed check
 * (minitest's CHECK() never aborts, so this always runs at the end of
 * main()) or the alarm(120) watchdog firing.
 */
static void reap_all(void)
{
	int i;

	for (i = 0; i < live_count; i++) {
		kill(live_pids[i], SIGKILL);
		waitpid(live_pids[i], NULL, 0);
	}
	live_count = 0;
}

/*
 * Forks and execs daemon_path with argv = { daemon_path, args[0],
 * args[1], ..., NULL }. The child moves stdin to /dev/null and both
 * pipe write-ends onto fd 1/2, arms PR_SET_PDEATHSIG so it dies with
 * this test process, and -- if nofile > 0 -- lowers its own
 * RLIMIT_NOFILE soft limit just before exec so the daemon starts with
 * exactly that many descriptors available (used by the accept()
 * failure test in a later commit). Every fd this test creates is
 * close-on-exec, so the child starts with exactly fds 0, 1 and 2.
 */
static int daemon_spawn(struct daemon *d, const char *const args[],
			 long nofile)
{
	int out_pipe[2], err_pipe[2];
	const char *argv_buf[32];
	size_t n = 0;
	pid_t pid;

	argv_buf[n++] = daemon_path;
	if (args != NULL) {
		size_t i;

		for (i = 0; args[i] != NULL; i++) {
			if (n >= ARRAY_SIZE(argv_buf) - 1)
				return -1;
			argv_buf[n++] = args[i];
		}
	}
	argv_buf[n] = NULL;

	if (pipe2(out_pipe, O_CLOEXEC) < 0)
		return -1;
	if (pipe2(err_pipe, O_CLOEXEC) < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		return -1;
	}

	if (pid == 0) {
		int devnull;

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() == 1)
			_exit(1);

		devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, STDIN_FILENO);

		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(err_pipe[1], STDERR_FILENO);

		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		if (devnull >= 0)
			close(devnull);

		if (nofile > 0) {
			struct rlimit rl;

			if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
				rl.rlim_cur = (rlim_t)nofile;
				setrlimit(RLIMIT_NOFILE, &rl);
			}
		}

		execv(argv_buf[0], (char *const *)argv_buf);
		_exit(127);
	}

	close(out_pipe[1]);
	close(err_pipe[1]);

	d->pid = pid;
	d->out_fd = out_pipe[0];
	d->err_fd = err_pipe[0];
	d->port = -1;
	d->listen_line[0] = '\0';
	d->out_len = 0;
	d->out_eof = 0;

	live_add(pid);
	return 0;
}

/*
 * Polls the stderr pipe, bounded by timeout_ms, until a complete line
 * starting with "rsswitchd: listening on " arrives. Parses the port
 * after the line's last ':' and keeps the full line for tests that
 * check the reported address too.
 */
static int daemon_wait_ready(struct daemon *d, int timeout_ms)
{
	static const char prefix[] = "rsswitchd: listening on ";
	char buf[LISTEN_LINE_LEN];
	size_t len = 0;
	long long deadline = now_ms() + timeout_ms;

	for (;;) {
		long long remaining = deadline - now_ms();
		struct pollfd pfd;
		int nready;
		ssize_t n;
		char *nl;

		if (remaining <= 0)
			return -1;
		if (len >= sizeof(buf) - 1)
			return -1;

		pfd.fd = d->err_fd;
		pfd.events = POLLIN;
		nready = poll(&pfd, 1, (int)remaining);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nready == 0)
			return -1;

		n = read(d->err_fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0)
			return -1;
		len += (size_t)n;
		buf[len] = '\0';

		nl = strchr(buf, '\n');
		if (nl == NULL)
			continue;
		*nl = '\0';

		if (strncmp(buf, prefix, strlen(prefix)) != 0)
			return -1;

		{
			const char *colon = strrchr(buf, ':');

			if (colon == NULL)
				return -1;
			d->port = atoi(colon + 1);
		}
		snprintf(d->listen_line, sizeof(d->listen_line), "%s", buf);
		return 0;
	}
}

/* Spawns with --bind 127.0.0.1 --port 0 plus extra, and waits up to
 * 5s for the listening line.
 */
static int daemon_start(struct daemon *d, const char *const extra[])
{
	const char *args[16];
	size_t n = 0;

	args[n++] = "--bind";
	args[n++] = "127.0.0.1";
	args[n++] = "--port";
	args[n++] = "0";
	if (extra != NULL) {
		size_t i;

		for (i = 0; extra[i] != NULL; i++) {
			if (n >= ARRAY_SIZE(args) - 1)
				return -1;
			args[n++] = extra[i];
		}
	}
	args[n] = NULL;

	if (daemon_spawn(d, args, 0) < 0)
		return -1;

	return daemon_wait_ready(d, 5000);
}

/*
 * For a command line that exits by itself (--help, an invalid
 * option). Collects stdout, stderr and the wait status; SIGKILLs the
 * child if it has not exited within timeout_ms.
 */
static int daemon_run(const char *const args[], int *status,
		       char *out, size_t outlen, char *err, size_t errlen,
		       int timeout_ms)
{
	struct daemon d;
	long long deadline;
	size_t out_used = 0, err_used = 0;
	int out_eof = 0, err_eof = 0;
	int reaped = 0;

	if (out != NULL && outlen > 0)
		out[0] = '\0';
	if (err != NULL && errlen > 0)
		err[0] = '\0';

	if (daemon_spawn(&d, args, 0) < 0)
		return -1;

	deadline = now_ms() + timeout_ms;

	while (!out_eof || !err_eof) {
		struct pollfd pfd[2];
		nfds_t nfds = 0;
		int out_slot = -1, err_slot = -1;
		long long remaining = deadline - now_ms();
		int nready;

		if (remaining < 0)
			remaining = 0;

		if (!out_eof) {
			pfd[nfds].fd = d.out_fd;
			pfd[nfds].events = POLLIN;
			out_slot = (int)nfds;
			nfds++;
		}
		if (!err_eof) {
			pfd[nfds].fd = d.err_fd;
			pfd[nfds].events = POLLIN;
			err_slot = (int)nfds;
			nfds++;
		}

		nready = poll(pfd, nfds, (int)remaining);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (nready == 0) {
			if (now_ms() >= deadline)
				break;
			continue;
		}

		if (out_slot >= 0 &&
		    (pfd[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
			char scratch[256];
			ssize_t n;

			if (out != NULL && out_used + 1 < outlen)
				n = read(d.out_fd, out + out_used,
					 outlen - 1 - out_used);
			else
				n = read(d.out_fd, scratch, sizeof(scratch));
			if (n <= 0)
				out_eof = 1;
			else if (out != NULL && out_used + 1 < outlen)
				out_used += (size_t)n;
		}

		if (err_slot >= 0 &&
		    (pfd[err_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
			char scratch[256];
			ssize_t n;

			if (err != NULL && err_used + 1 < errlen)
				n = read(d.err_fd, err + err_used,
					 errlen - 1 - err_used);
			else
				n = read(d.err_fd, scratch, sizeof(scratch));
			if (n <= 0)
				err_eof = 1;
			else if (err != NULL && err_used + 1 < errlen)
				err_used += (size_t)n;
		}
	}

	if (out != NULL && outlen > 0)
		out[out_used < outlen ? out_used : outlen - 1] = '\0';
	if (err != NULL && errlen > 0)
		err[err_used < errlen ? err_used : errlen - 1] = '\0';

	close(d.out_fd);
	close(d.err_fd);

	if (!out_eof || !err_eof)
		kill(d.pid, SIGKILL);

	{
		long long wait_deadline = now_ms() + 2000;
		int st = 0;

		for (;;) {
			pid_t w = waitpid(d.pid, &st, WNOHANG);

			if (w == d.pid) {
				reaped = 1;
				break;
			}
			if (now_ms() >= wait_deadline) {
				kill(d.pid, SIGKILL);
				waitpid(d.pid, &st, 0);
				reaped = 1;
				break;
			}
			usleep(5000);
		}
		if (status != NULL)
			*status = st;
	}

	live_remove(d.pid);
	return reaped ? 0 : -1;
}

/* Returns the next stdout line (without its newline) in buf, or 0 on
 * timeout or EOF.
 */
static int daemon_stdout_line(struct daemon *d, char *buf, size_t len,
			       int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;

	for (;;) {
		char *nl = memchr(d->out_buf, '\n', d->out_len);
		long long remaining;
		struct pollfd pfd;
		int nready;
		ssize_t n;

		if (nl != NULL) {
			size_t linelen = (size_t)(nl - d->out_buf);
			size_t copylen = linelen < len - 1 ? linelen : len - 1;

			memcpy(buf, d->out_buf, copylen);
			buf[copylen] = '\0';
			memmove(d->out_buf, nl + 1, d->out_len - linelen - 1);
			d->out_len -= linelen + 1;
			return 1;
		}

		if (d->out_eof)
			return 0;

		remaining = deadline - now_ms();
		if (remaining <= 0)
			return 0;

		pfd.fd = d->out_fd;
		pfd.events = POLLIN;
		nready = poll(&pfd, 1, (int)remaining);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (nready == 0)
			return 0;

		if (d->out_len >= sizeof(d->out_buf) - 1) {
			/* No newline in a full buffer: the daemon only
			 * ever prints short codeword lines, so this is
			 * unexpected. Drop it rather than spin forever.
			 */
			d->out_len = 0;
		}

		n = read(d->out_fd, d->out_buf + d->out_len,
			 sizeof(d->out_buf) - 1 - d->out_len);
		if (n <= 0) {
			d->out_eof = 1;
			continue;
		}
		d->out_len += (size_t)n;
	}
}

/* Sends sig, waits up to 2s with WNOHANG polling, SIGKILLs and reaps
 * after that.
 */
static void daemon_stop(struct daemon *d, int sig, int *status)
{
	long long deadline = now_ms() + 2000;
	int st = 0;

	kill(d->pid, sig);

	for (;;) {
		pid_t w = waitpid(d->pid, &st, WNOHANG);

		if (w == d->pid)
			break;
		if (now_ms() >= deadline) {
			kill(d->pid, SIGKILL);
			waitpid(d->pid, &st, 0);
			break;
		}
		usleep(5000);
	}

	if (status != NULL)
		*status = st;

	close(d->out_fd);
	close(d->err_fd);
	live_remove(d->pid);
}

/* Counts the entries of /proc/<pid>/fd, i.e. the number of open file
 * descriptors of the given process. Returns -1 if /proc/<pid>/fd
 * cannot be read (the process already exited).
 */
static int daemon_fd_count(pid_t pid)
{
	char path[64];
	struct dirent *ent;
	DIR *dir;
	int count = 0;

	snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
	dir = opendir(path);
	if (dir == NULL)
		return -1;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		count++;
	}
	closedir(dir);
	return count;
}

/* Reads utime+stime (fields 14 and 15 of /proc/<pid>/stat, in clock
 * ticks) after the closing parenthesis of comm, so a process name
 * containing spaces or parentheses cannot shift the field count.
 * Returns -1 if the file cannot be read or parsed.
 */
static long daemon_cpu_ticks(pid_t pid)
{
	char path[64];
	char *line = NULL;
	size_t linecap = 0;
	char state;
	int ppid, pgrp, session, tty_nr, tpgid;
	unsigned int flags;
	unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
	char *paren;
	ssize_t n;
	long ret = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	n = getline(&line, &linecap, f);
	fclose(f);
	if (n < 0) {
		free(line);
		return -1;
	}

	/*
	 * scanf's assignment-suppression ('*') cannot be combined with
	 * a length modifier here (rejected by this compiler's
	 * gnu_scanf format check), so every field is captured into a
	 * named variable and the ones this function does not need are
	 * simply left unused after the call.
	 */
	paren = strrchr(line, ')');
	if (paren != NULL &&
	    sscanf(paren + 1,
		   " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu",
		   &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
		   &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime) == 13)
		ret = (long)(utime + stime);

	free(line);
	return ret;
}

/* Makes a SOCK_CLOEXEC TCP socket with TCP_NODELAY and retries
 * ECONNREFUSED every 20ms for up to 2s. Returns the fd, or -1 on
 * another error or at the deadline.
 */
static int client_connect(const char *addr, int port)
{
	long long deadline = now_ms() + 2000;

	for (;;) {
		struct sockaddr_in sa;
		int one = 1;
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (fd < 0)
			return -1;

		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((uint16_t)port);
		if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
			close(fd);
			return -1;
		}

		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
			return fd;

		if (errno == ECONNREFUSED && now_ms() < deadline) {
			close(fd);
			usleep(20000);
			continue;
		}

		close(fd);
		return -1;
	}
}

static int client_send(int fd, const char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = write(fd, buf + sent, len - sent);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

/*
 * Reads one \n-terminated reply line (without its newline) from fd,
 * one byte at a time. Returns 1 for a complete line (buf is then
 * NUL-terminated), 0 at EOF before any newline, -1 on timeout with no
 * complete line yet or if buf is too small.
 */
static int client_line(int fd, char *buf, size_t len, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	size_t used = 0;

	if (len == 0)
		return -1;

	for (;;) {
		struct pollfd pfd;
		long long remaining = deadline - now_ms();
		int nready;
		ssize_t n;
		char c;

		if (remaining <= 0)
			return -1;

		pfd.fd = fd;
		pfd.events = POLLIN;
		nready = poll(&pfd, 1, (int)remaining);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nready == 0)
			return -1;

		n = read(fd, &c, 1);
		if (n == 0)
			return 0;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (c == '\n') {
			if (used >= len)
				return -1;
			buf[used] = '\0';
			return 1;
		}

		if (used + 1 >= len)
			return -1;
		buf[used++] = c;
	}
}

/* Waits up to timeout_ms for a byte (or EOF) on fd; returns 1 if none
 * arrived, 0 otherwise.
 */
static int client_quiet(int fd, int timeout_ms)
{
	struct pollfd pfd;
	int nready;

	pfd.fd = fd;
	pfd.events = POLLIN;
	nready = poll(&pfd, 1, timeout_ms);
	if (nready < 0)
		return 1;
	return nready == 0;
}

static void test_help(void)
{
	static const char *const args[] = {"--help", NULL};
	char out[1024], err[256];
	int status = 0;

	CHECK_EQ(daemon_run(args, &status, out, sizeof(out), err,
			    sizeof(err), 2000), 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(strstr(out, "--port") != NULL);
	CHECK(strstr(out, "--bind") != NULL);
	CHECK(strstr(out, "--gpio") != NULL);
	CHECK(strstr(out, "5000") != NULL);
	CHECK(strstr(out, "0.0.0.0") != NULL);
	CHECK_EQ(err[0], '\0');
}

static void test_invalid_options(void)
{
	static const char *const cases[][3] = {
		{"--port", "abc", NULL},
		{"--port", "-1", NULL},
		{"--port", "65536", NULL},
		{"--port", "", NULL},
		{"--bind", "999.1.1.1", NULL},
		{"--bind", "", NULL},
		{"--nosuch", NULL, NULL},
		{"--port", NULL, NULL},
		{"extra", NULL, NULL},
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const char *args[4];
		char out[256], err[256];
		int status = 0;
		size_t j, n = 0;

		for (j = 0; j < 3 && cases[i][j] != NULL; j++)
			args[n++] = cases[i][j];
		args[n] = NULL;

		CHECK_EQ(daemon_run(args, &status, out, sizeof(out), err,
				    sizeof(err), 2000), 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
		CHECK_EQ(out[0], '\0');
		CHECK(err[0] != '\0');
		CHECK(strstr(err, "listening on") == NULL);
	}
}

static void test_option_forms(void)
{
	struct daemon d;
	int st;

	{
		static const char *const args[] = {
			"--bind", "127.0.0.1", "--port=0", NULL
		};

		CHECK_EQ(daemon_spawn(&d, args, 0), 0);
		CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
		CHECK(d.port > 0);
		daemon_stop(&d, SIGTERM, &st);
	}

	{
		static const char *const args[] = {
			"--bind", "127.0.0.1",
			"--port", "5000", "--port", "0", NULL
		};

		CHECK_EQ(daemon_spawn(&d, args, 0), 0);
		CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
		CHECK(d.port != 5000);
		daemon_stop(&d, SIGTERM, &st);
	}

	{
		static const char *const args[] = {
			"--port", "0", "--bind", "127.0.0.1", NULL
		};

		CHECK_EQ(daemon_spawn(&d, args, 0), 0);
		CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
		CHECK(strstr(d.listen_line, "127.0.0.1:") != NULL);
		daemon_stop(&d, SIGTERM, &st);
	}

	{
		static const char *const args[] = {
			"--bind", "127.0.0.1", "--port", "0", NULL
		};

		CHECK_EQ(daemon_spawn(&d, args, 0), 0);
		CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
		CHECK(strstr(d.listen_line, "127.0.0.1:") != NULL);
		daemon_stop(&d, SIGTERM, &st);
	}

	{
		static const char *const args[] = {
			"--bind", "127.0.0.1", "--port", "0", "--help", NULL
		};
		char out[1024], err[256];
		int status = 0;

		CHECK_EQ(daemon_run(args, &status, out, sizeof(out), err,
				    sizeof(err), 2000), 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	{
		static const char *const args[] = {
			"--bind", "127.0.0.1", "--port", "0",
			"--gpio", "anything", NULL
		};

		CHECK_EQ(daemon_spawn(&d, args, 0), 0);
		CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
		daemon_stop(&d, SIGTERM, &st);
	}
}

static void test_bind_only_there(void)
{
	struct daemon d;
	int st;
	int fd;

	CHECK_EQ(daemon_start(&d, NULL), 0);
	CHECK(strstr(d.listen_line, "127.0.0.1:") != NULL);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);

	{
		struct sockaddr_in sa;
		int fd2 = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

		CHECK(fd2 >= 0);
		if (fd2 >= 0) {
			int ret, saved_errno;

			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons((uint16_t)d.port);
			inet_pton(AF_INET, "127.0.0.2", &sa.sin_addr);

			ret = connect(fd2, (struct sockaddr *)&sa, sizeof(sa));
			saved_errno = errno;
			CHECK(ret < 0);
			CHECK_EQ(saved_errno, ECONNREFUSED);
			close(fd2);
		}
	}

	daemon_stop(&d, SIGTERM, &st);
}

static void test_default_bind_address(void)
{
	struct daemon d;
	int st;
	static const char *const args[] = {"--port", "0", NULL};

	CHECK_EQ(daemon_spawn(&d, args, 0), 0);
	CHECK_EQ(daemon_wait_ready(&d, 5000), 0);
	CHECK(strstr(d.listen_line, "listening on 0.0.0.0:") != NULL);
	daemon_stop(&d, SIGTERM, &st);
}

static void test_legacy_codeword(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A connection carries two commands with a gap between them; both
 * must be executed, in order (DMN-01).
 */
static void test_two_reads_one_connection(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);

		usleep(200000);

		CHECK_EQ(client_send(fd, "0321", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF1FF100010"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A connection that never sends anything must not stop the next
 * client from being served.
 */
static void test_empty_connection(void)
{
	struct daemon d;
	int st;
	int fd1, fd2;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd1 = client_connect("127.0.0.1", d.port);
	CHECK(fd1 >= 0);
	if (fd1 >= 0)
		close(fd1);

	fd2 = client_connect("127.0.0.1", d.port);
	CHECK(fd2 >= 0);
	if (fd2 >= 0) {
		CHECK_EQ(client_send(fd2, "0300", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		close(fd2);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A complete legacy command followed at once by close() still gets
 * executed; an incomplete one followed by close() gets nothing.
 */
static void test_eof_after_command(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		close(fd);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
	}

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "030", 3), 0);
		close(fd);
		CHECK_EQ(daemon_stdout_line(&d, line, sizeof(line), 300), 0);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A client reset (RST) while the daemon waits in read() must not stop
 * the daemon or the next client from being served.
 */
static void test_client_reset(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		struct linger lg = { .l_onoff = 1, .l_linger = 0 };

		setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		close(fd);
	}

	/* Give the daemon a moment to observe the reset. */
	usleep(100000);
	CHECK_EQ(waitpid(d.pid, NULL, WNOHANG), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* The daemon serves one connection at a time: a second client's
 * command is not executed while the first connection is still open
 * and silent, and runs once the first connection closes.
 */
static void test_one_connection_at_a_time(void)
{
	struct daemon d;
	int st;
	int fda, fdb;
	char line[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fda = client_connect("127.0.0.1", d.port);
	CHECK(fda >= 0);

	fdb = client_connect("127.0.0.1", d.port);
	CHECK(fdb >= 0);
	if (fdb >= 0)
		CHECK_EQ(client_send(fdb, "0300", 4), 0);

	CHECK_EQ(daemon_stdout_line(&d, line, sizeof(line), 300), 0);

	if (fda >= 0)
		close(fda);

	CHECK(daemon_stdout_line(&d, line, sizeof(line), 1000));
	CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);

	if (fdb >= 0)
		close(fdb);

	daemon_stop(&d, SIGTERM, &st);
}

/* A bind() conflict must exit 1 loudly, never silently listen on a
 * random port (DMN-02).
 */
static void test_bind_failure_exits(void)
{
	struct sockaddr_in sa;
	socklen_t salen = sizeof(sa);
	int listener;
	int port;
	char portstr[16];
	char prefix[64];
	const char *args[5];
	char out[256], err[512];
	int status = 0;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(listener >= 0);
	if (listener < 0)
		return;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(0);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

	CHECK_EQ(bind(listener, (struct sockaddr *)&sa, sizeof(sa)), 0);
	CHECK_EQ(listen(listener, 1), 0);
	CHECK_EQ(getsockname(listener, (struct sockaddr *)&sa, &salen), 0);
	port = ntohs(sa.sin_port);

	snprintf(portstr, sizeof(portstr), "%d", port);
	args[0] = "--bind";
	args[1] = "127.0.0.1";
	args[2] = "--port";
	args[3] = portstr;
	args[4] = NULL;

	CHECK_EQ(daemon_run(args, &status, out, sizeof(out), err, sizeof(err),
			    2000), 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	snprintf(prefix, sizeof(prefix), "rsswitchd: bind 127.0.0.1:%d", port);
	CHECK(strstr(err, prefix) != NULL);
	CHECK(strstr(err, "listening on") == NULL);

	close(listener);
}

/* An accept() failure (EMFILE, forced with a low RLIMIT_NOFILE) must
 * not stop or spin the daemon (DMN-02).
 */
static void test_accept_failure_keeps_running(void)
{
	struct daemon d;
	int st;
	int fd;
	long ticks0, ticks1;
	long clk_tck = sysconf(_SC_CLK_TCK);
	static const char *const args[] = {
		"--bind", "127.0.0.1", "--port", "0", NULL
	};

	CHECK_EQ(daemon_spawn(&d, args, 4), 0);
	CHECK_EQ(daemon_wait_ready(&d, 5000), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);

	ticks0 = daemon_cpu_ticks(d.pid);
	CHECK(ticks0 >= 0);

	usleep(1000000);

	CHECK_EQ(waitpid(d.pid, NULL, WNOHANG), 0);

	{
		char line[64];

		CHECK_EQ(daemon_stdout_line(&d, line, sizeof(line), 100), 0);
	}

	ticks1 = daemon_cpu_ticks(d.pid);
	CHECK(ticks1 >= 0);
	if (ticks0 >= 0 && ticks1 >= 0 && clk_tck > 0)
		CHECK((double)(ticks1 - ticks0) / (double)clk_tck < 0.5);

	if (fd >= 0)
		close(fd);

	daemon_stop(&d, SIGTERM, &st);
}

/* Two commands on one connection, with no artificial gap between
 * them (DMN-08).
 */
static void test_no_pause_between_commands(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];
	long long t0;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));

		t0 = now_ms();
		CHECK_EQ(client_send(fd, "0321", 4), 0);
		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK(now_ms() - t0 < 500);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A daemon stopped while a client connection was still open must not
 * block an immediate restart on the same port (SO_REUSEADDR, DMN-03).
 */
static void test_restart_same_port(void)
{
	struct daemon a, b;
	int st;
	int fd;
	int port;
	char line[64];
	char portstr[16];
	const char *args_b[5];

	CHECK_EQ(daemon_start(&a, NULL), 0);
	port = a.port;

	fd = client_connect("127.0.0.1", a.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK(daemon_stdout_line(&a, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
	}

	/* A is signalled while the connection is still open, so its
	 * side is closed first and leaves TIME_WAIT/FIN_WAIT on port.
	 */
	daemon_stop(&a, SIGTERM, &st);

	if (fd >= 0)
		close(fd);

	snprintf(portstr, sizeof(portstr), "%d", port);
	args_b[0] = "--bind";
	args_b[1] = "127.0.0.1";
	args_b[2] = "--port";
	args_b[3] = portstr;
	args_b[4] = NULL;

	CHECK_EQ(daemon_spawn(&b, args_b, 0), 0);
	CHECK_EQ(daemon_wait_ready(&b, 2000), 0);

	daemon_stop(&b, SIGTERM, &st);
}

/* 20 connections that each send a command or nothing, then close,
 * must not leak a file descriptor per connection (DMN-01/T-02-06).
 */
static void test_no_fd_leak(void)
{
	struct daemon d;
	int st;
	int baseline, after;
	int seen = 0;
	int i;
	long long deadline;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	/*
	 * A warm-up connection forces the daemon's first syslog()
	 * call (per accepted connection), which glibc's syslog(3)
	 * only opens its socket to the syslog daemon on -- a one-time
	 * fd for the life of the process, not a per-connection leak.
	 * Measure the baseline after this warm-up so it is not
	 * mistaken for one.
	 */
	{
		int wfd = client_connect("127.0.0.1", d.port);

		CHECK(wfd >= 0);
		if (wfd >= 0)
			close(wfd);
	}
	usleep(200000);

	baseline = daemon_fd_count(d.pid);
	CHECK(baseline >= 0);

	for (i = 0; i < 20; i++) {
		int fd = client_connect("127.0.0.1", d.port);

		CHECK(fd >= 0);
		if (fd < 0)
			continue;

		if (i % 2 == 0)
			CHECK_EQ(client_send(fd, "0300", 4), 0);

		close(fd);
	}

	while (seen < 10) {
		char line[64];

		if (!daemon_stdout_line(&d, line, sizeof(line), 3000))
			break;
		seen++;
	}
	CHECK_EQ(seen, 10);

	/*
	 * Until the next commit removes it, the daemon still sleeps
	 * 1s after every processed command before it can close that
	 * connection and accept the next one, so draining 10 commands
	 * can take close to 10s. Poll generously rather than assume a
	 * fast drain; once the sleep is gone this converges almost
	 * immediately.
	 */
	after = daemon_fd_count(d.pid);
	deadline = now_ms() + 15000;
	while (after != baseline && now_ms() < deadline) {
		usleep(20000);
		after = daemon_fd_count(d.pid);
	}
	CHECK_EQ(after, baseline);

	daemon_stop(&d, SIGTERM, &st);
}

/* Sends cmd_bytes, expects codeword on stdout and "OK" as the reply. */
static void expect_ok(struct daemon *d, int fd, const char *cmd_bytes,
		       size_t cmd_len, const char *codeword)
{
	char line[64], reply[64];

	CHECK_EQ(client_send(fd, cmd_bytes, cmd_len), 0);
	CHECK(daemon_stdout_line(d, line, sizeof(line), 2000));
	CHECK_EQ(strcmp(line, codeword), 0);
	CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
	CHECK_EQ(strcmp(reply, "OK"), 0);
}

/* v2 commands reach the library, including Pollin group/socket >= 10
 * (PROTO-01), and each gets an OK reply (PROTO-04).
 */
static void test_v2_codewords(void)
{
	struct daemon d;
	int st;
	int fd;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		expect_ok(&d, fd, "1 17 23 1\n", 10, "0FFF0000F00F");
		expect_ok(&d, fd, "1 10 10 1\n", 10, "F0F0FF0F0F0F");
		expect_ok(&d, fd, "1 31 31 1\n", 10, "00000000000F");
		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* Legacy and v2 produce the same codeword and the same OK reply for
 * the same address (PROTO-02).
 */
static void test_legacy_equals_v2(void)
{
	struct daemon d;
	int st;
	int fd;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		expect_ok(&d, fd, "0300", 4, "FFF11FF00001");
		expect_ok(&d, fd, "0 3 0 0\n", 8, "FFF11FF00001");
		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* One write carrying a legacy command directly followed by a v2 line
 * and a CRLF-terminated v2 line produces three codewords and three
 * OK replies, in order (PROTO-05).
 */
static void test_back_to_back(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64], reply[64];
	static const char msg[] = "03001 17 23 1\n0 3 0 0\r\n";

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, msg, sizeof(msg) - 1), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "0FFF0000F00F"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* The same bytes as test_back_to_back, sent one byte per write, 2ms
 * apart, produce the same three codewords and OK replies (PROTO-05
 * fragmentation).
 */
static void test_byte_by_byte(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64], reply[64];
	static const char msg[] = "03001 17 23 1\n0 3 0 0\r\n";
	size_t i;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		for (i = 0; i < sizeof(msg) - 1; i++) {
			CHECK_EQ(client_send(fd, &msg[i], 1), 0);
			usleep(2000);
		}

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "0FFF0000F00F"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A malformed v2 line gets ERR syntax and transmits nothing; a
 * legacy command right after it still executes and gets OK
 * (PROTO-03).
 */
static void test_bad_line_then_good(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64], reply[64];
	static const char msg[] = "1 017 23 1\n0300";

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, msg, sizeof(msg) - 1), 0);

		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR syntax malformed command"), 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* An overlong line is discarded, not transmitted, gets ERR toolong,
 * and the connection still works afterwards (D-17).
 */
static void test_toolong_then_good(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64], reply[64];
	char msg[205];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	memset(msg, 'x', 200);
	msg[200] = '\n';
	memcpy(msg + 201, "0300", 4);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, msg, sizeof(msg)), 0);

		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR toolong line longer than 128 bytes"),
			 0);

		CHECK(daemon_stdout_line(&d, line, sizeof(line), 2000));
		CHECK_EQ(strcmp(line, "FFF11FF00001"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* "1 17 23 1" with no trailing newline, followed by close(), sends
 * nothing and gets no reply -- a partial command at EOF is dropped
 * (PROTO-04).
 */
static void test_partial_at_eof(void)
{
	struct daemon d;
	int st;
	int fd;
	char line[64];
	static const char msg[] = "1 17 23 1";

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, msg, sizeof(msg) - 1), 0);
		close(fd);
		CHECK_EQ(daemon_stdout_line(&d, line, sizeof(line), 300), 0);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* The worked examples from 02-CONTEXT.md's <specifics>, this time
 * checking replies rather than just codewords.
 */
static void test_worked_replies(void)
{
	struct daemon d;
	int st;
	int fd;
	char reply[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		/* 0300 -> one OK. */
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		/* 0300\r\n -> exactly one OK (the CRLF is swallowed,
		 * not a second, empty command).
		 */
		CHECK_EQ(client_send(fd, "0300\r\n", 6), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);
		CHECK(client_quiet(fd, 300));

		/* 03000301 -> OK OK. */
		CHECK_EQ(client_send(fd, "03000301", 8), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		/* 1 017 23 1\n -> ERR syntax malformed command. */
		CHECK_EQ(client_send(fd, "1 017 23 1\n", 11), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR syntax malformed command"), 0);

		/* An overlong line -> ERR toolong, then 0300 -> OK. */
		{
			char big[205];

			memset(big, 'x', 200);
			big[200] = '\n';
			memcpy(big + 201, "0300", 4);

			CHECK_EQ(client_send(fd, big, sizeof(big)), 0);
			CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000),
				 1);
			CHECK_EQ(strcmp(reply,
					"ERR toolong line longer than 128 bytes"),
				 0);
			CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000),
				 1);
			CHECK_EQ(strcmp(reply, "OK"), 0);
		}

		/* An empty line -> no reply within 300ms. */
		CHECK_EQ(client_send(fd, "\n", 1), 0);
		CHECK(client_quiet(fd, 300));

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A pipeline of 7 commands (including a legacy command whose \n is
 * swallowed and one empty line) sent in one write gets exactly 7
 * replies, in order, then nothing more (PROTO-04, D-10, D-11, D-14,
 * D-18).
 */
static void test_one_reply_per_command(void)
{
	struct daemon d;
	int st;
	int fd;
	char reply[64];
	static const char msg[] =
		"0300\n1 17 23 1\nx\n\r\n1 4294967296 0 1\n0 3 0 0\r\n"
		"1 017 23 1\n1 10 10 1\n";
	static const char *const expect[] = {
		"OK",
		"OK",
		"ERR syntax malformed command",
		"ERR range group too large",
		"OK",
		"ERR syntax malformed command",
		"OK",
	};
	size_t i;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, msg, sizeof(msg) - 1), 0);

		for (i = 0; i < ARRAY_SIZE(expect); i++) {
			CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000),
				 1);
			CHECK_EQ(strcmp(reply, expect[i]), 0);
		}

		CHECK(client_quiet(fd, 300));

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* Legacy commands get replies just like v2 and keep the connection
 * open (D-03).
 */
static void test_legacy_keeps_connection(void)
{
	struct daemon d;
	int st;
	int fd;
	char reply[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		CHECK_EQ(client_send(fd, "0301", 4), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* A library failure after the daemon's own checks passed is reported
 * as ERR send, with no codeword printed, and the connection stays
 * open (D-12, D-14).
 */
static void test_err_send(void)
{
	const char *saved_path = daemon_path;
	struct daemon d;
	int st;
	int fd;
	char reply[64];

	daemon_path = "./test_rsswitchd_failbin";
	CHECK_EQ(daemon_start(&d, NULL), 0);
	daemon_path = saved_path;

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "1 17 23 1\n", 10), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR send transmission failed"), 0);
		CHECK(daemon_stdout_line(&d, reply, sizeof(reply), 300) == 0);

		/* Same reply again -- the connection stays open. */
		CHECK_EQ(client_send(fd, "1 17 23 1\n", 10), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR send transmission failed"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* An out-of-range address gets a precise ERR range reply before the
 * library is ever called, for both v2 and legacy, the connection
 * stays usable afterwards, and the same range check fires even
 * against a daemon whose board always fails -- proving the daemon
 * validated first, since a board failure would give ERR send instead
 * (D-05, D-11, D-12, D-13).
 */
static void test_range_replies(void)
{
	struct daemon d;
	int st;
	int fd;
	char reply[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "1 40 0 1\n", 9), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR range group 40 not in 0-31"), 0);
		CHECK(client_quiet(fd, 300));

		CHECK_EQ(client_send(fd, "9999", 4), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR range dev 9 unknown"), 0);
		CHECK(client_quiet(fd, 300));

		expect_ok(&d, fd, "1 31 31 1\n", 10, "00000000000F");

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);

	{
		const char *saved_path = daemon_path;

		daemon_path = "./test_rsswitchd_failbin";
		CHECK_EQ(daemon_start(&d, NULL), 0);
		daemon_path = saved_path;

		fd = client_connect("127.0.0.1", d.port);
		CHECK(fd >= 0);
		if (fd >= 0) {
			CHECK_EQ(client_send(fd, "1 40 0 1\n", 9), 0);
			CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000),
				 1);
			CHECK_EQ(strcmp(reply,
					"ERR range group 40 not in 0-31"), 0);
			close(fd);
		}

		daemon_stop(&d, SIGTERM, &st);
	}
}

/* Neither a reply nor a syslog line ever contains raw client bytes
 * (D-15) -- checked here at the reply, escape sequences and garbage
 * alike collapse to the same fixed text.
 */
static void test_no_echo(void)
{
	struct daemon d;
	int st;
	int fd;
	char reply[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "\x1b[31m\n", 6), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR syntax malformed command"), 0);

		CHECK_EQ(client_send(fd, "zzzz\n", 5), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "ERR syntax malformed command"), 0);

		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

/* The daemon serves one connection at a time: B's command is not
 * answered while A's connection is open and silent, and gets its
 * reply once A closes (DMN-06).
 */
static void test_serialized_replies(void)
{
	struct daemon d;
	int st;
	int fda, fdb;
	char reply[64];

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fda = client_connect("127.0.0.1", d.port);
	CHECK(fda >= 0);
	if (fda >= 0) {
		CHECK_EQ(client_send(fda, "1 17 23 1\n", 10), 0);
		CHECK_EQ(client_line(fda, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);
	}

	fdb = client_connect("127.0.0.1", d.port);
	CHECK(fdb >= 0);
	if (fdb >= 0)
		CHECK_EQ(client_send(fdb, "0300", 4), 0);

	CHECK(client_quiet(fdb, 300));

	if (fda >= 0)
		close(fda);

	CHECK_EQ(client_line(fdb, reply, sizeof(reply), 1000), 1);
	CHECK_EQ(strcmp(reply, "OK"), 0);

	if (fdb >= 0)
		close(fdb);

	daemon_stop(&d, SIGTERM, &st);
}

/* A client that writes many lines and closes before reading any
 * reply must not kill the daemon with SIGPIPE (DMN-04).
 */
static void test_early_disconnect(void)
{
	struct daemon d;
	int st;
	int fd;
	int i;
	char reply[64];
	pid_t w;
	int status = 0;

	CHECK_EQ(daemon_start(&d, NULL), 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		for (i = 0; i < 64; i++)
			CHECK_EQ(client_send(fd, "x\n", 2), 0);
		close(fd);
	}

	usleep(300000);

	w = waitpid(d.pid, &status, WNOHANG);
	CHECK_EQ(w, 0);

	fd = client_connect("127.0.0.1", d.port);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK_EQ(client_send(fd, "0300", 4), 0);
		CHECK_EQ(client_line(fd, reply, sizeof(reply), 2000), 1);
		CHECK_EQ(strcmp(reply, "OK"), 0);
		close(fd);
	}

	daemon_stop(&d, SIGTERM, &st);
}

int main(int argc, char *argv[])
{
	if (argc > 1)
		daemon_path = argv[1];

	alarm(120);
	atexit(reap_all);

	test_help();
	test_invalid_options();
	test_option_forms();
	test_bind_only_there();
	test_default_bind_address();
	test_legacy_codeword();
	test_two_reads_one_connection();
	test_empty_connection();
	test_eof_after_command();
	test_client_reset();
	test_one_connection_at_a_time();
	test_bind_failure_exits();
	test_accept_failure_keeps_running();
	test_no_pause_between_commands();
	test_restart_same_port();
	test_no_fd_leak();
	test_v2_codewords();
	test_legacy_equals_v2();
	test_back_to_back();
	test_byte_by_byte();
	test_bad_line_then_good();
	test_toolong_then_good();
	test_partial_at_eof();
	test_worked_replies();
	test_one_reply_per_command();
	test_legacy_keeps_connection();
	test_err_send();
	test_range_replies();
	test_no_echo();
	test_serialized_replies();
	test_early_disconnect();

	return MT_REPORT();
}
