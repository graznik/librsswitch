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
	test_no_pause_between_commands();
	test_no_fd_leak();

	return MT_REPORT();
}
