#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../../librsswitch.h"
#include "rsswitchd_proto.h"

static void serve_connection(int connfd, const char *ip);
static int wait_readable(int fd, const struct timespec *deadline);
static int handle_command(int connfd, const char *ip,
			   const struct proto_cmd *cmd);
static void log_err(const char *ip, enum proto_err err, const char *text);
static int send_reply(int connfd, const char *ip, const char *reply);

#define DEFAULT_PORT 5000
#define DEFAULT_BIND "0.0.0.0"

/*
 * Default --timeout, in seconds: how long a connection may go without
 * a complete command before it is closed (D-19, DMN-05).
 */
#define DEFAULT_TIMEOUT 10U

/*
 * How long a connection may go without a complete command before it
 * is closed, and how long a reply write may block -- set once from
 * --timeout in main(), read (never written) by every serve_connection()
 * call afterwards.
 */
static unsigned int idle_timeout = DEFAULT_TIMEOUT;

static const struct option long_opts[] = {
	{"port", required_argument, NULL, 'p'},
	{"bind", required_argument, NULL, 'b'},
	{"gpio", required_argument, NULL, 'g'},
	{"timeout", required_argument, NULL, 't'},
	{"help", no_argument,       NULL, 'h'},
	{NULL, 0, NULL, 0}
};

/*
 * Prints one line per option with its default. --help writes this to
 * stdout and exits 0; a command-line error writes it to stderr, after
 * the error message, and exits 2.
 */
static void usage(FILE *out)
{
	fprintf(out,
		"Usage: rsswitchd [--port PORT] [--bind ADDR] [--gpio CONFIG]\n"
		"                 [--timeout SECONDS] [--help]\n"
		"  --port PORT    TCP port to listen on, 0-65535; 0 lets the\n"
		"                 kernel pick a free port (default 5000)\n"
		"  --bind ADDR    IPv4 address to listen on (default 0.0.0.0,\n"
		"                 meaning all addresses)\n"
		"  --gpio CONFIG  board-specific GPIO config (default: the\n"
		"                 board's built-in default)\n"
		"  --timeout SECS closes a connection when no complete\n"
		"                 command arrives for SECS seconds, 1-3600\n"
		"                 (default 10)\n"
		"  --help         show this text and exit\n");
}

/*
 * Parses arg as a base-10 unsigned long no larger than max, rejecting
 * everything strtoul() would otherwise silently accept: a leading
 * sign, whitespace, a hex prefix, trailing garbage, an empty string
 * and overflow (the same idiom as rsssend.c's parse_u32(), with an
 * explicit upper bound). Leading zeros are allowed here -- this is a
 * command line, not the wire protocol. On success writes the value to
 * *out and returns 0; on failure prints "rsswitchd: invalid NAME
 * 'arg'" to stderr and returns -1.
 */
static int parse_number(const char *name, const char *arg, unsigned long max,
			 unsigned long *out)
{
	unsigned long val;
	char *end;

	if (arg[0] == '\0' || strspn(arg, "0123456789") != strlen(arg))
		goto invalid;

	errno = 0;
	val = strtoul(arg, &end, 10);
	if (errno == ERANGE || val > max || *end != '\0')
		goto invalid;

	*out = val;
	return 0;

invalid:
	fprintf(stderr, "rsswitchd: invalid %s '%s'\n", name, arg);
	return -1;
}

/*
 * Logs "[-] what: strerror(errno)" to syslog and prints the same to
 * stderr with an "rsswitchd: " prefix -- an operator starting the
 * daemon by hand must see why it did not come up, not just find it
 * in the log -- then exits 1.
 */
static void fatal(const char *what)
{
	int saved_errno = errno;

	syslog(LOG_ERR, "[-] %s: %s", what, strerror(saved_errno));
	fprintf(stderr, "rsswitchd: %s: %s\n", what, strerror(saved_errno));
	exit(1);
}

int main(int argc, char *argv[])
{
	int listenfd = 0, connfd = 0;
	const char *gpio_config = NULL;
	struct sockaddr_in serv_addr, cli_addr;
	socklen_t cli_len = sizeof(cli_addr);
	char cli_ipaddr[INET_ADDRSTRLEN];
	struct in_addr bind_addr;
	unsigned long port = DEFAULT_PORT;
	int opt;

	/*
	 * On the x86_64 build the library prints every sent codeword
	 * on stdout. When stdout is a pipe or a file, as it is under
	 * systemd or when a test captures it, the stream is fully
	 * buffered by default, so a codeword only appears once the
	 * buffer fills or at exit, and a daemon stopped by a signal
	 * loses it. Line buffering makes each codeword visible as
	 * soon as it is sent.
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Opened first, so a startup failure below is logged under
	 * the daemon's own name.
	 */
	openlog("rsswitchd", 0, LOG_USER);

	{
		struct sigaction sa;

		/*
		 * Now that every command gets a reply, a client that
		 * closes its connection before reading that reply
		 * makes the next write() to it raise SIGPIPE. The
		 * default disposition of SIGPIPE terminates the whole
		 * process, not just that connection. Ignoring it turns
		 * that write() into a normal, checked EPIPE failure
		 * instead -- send_reply() already logs and returns -1
		 * on any write error, ending only that connection.
		 */
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGPIPE, &sa, NULL) < 0)
			fatal("sigaction SIGPIPE");
	}

	if (inet_pton(AF_INET, DEFAULT_BIND, &bind_addr) != 1) {
		fprintf(stderr, "rsswitchd: internal error: bad default bind address\n");
		return 1;
	}

	while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		unsigned long val;

		switch (opt) {
		case 'p':
			if (parse_number("--port", optarg, 65535, &val) < 0) {
				usage(stderr);
				exit(2);
			}
			port = val;
			break;
		case 'b':
			if (inet_pton(AF_INET, optarg, &bind_addr) != 1) {
				fprintf(stderr, "rsswitchd: invalid --bind '%s'\n",
					optarg);
				usage(stderr);
				exit(2);
			}
			break;
		case 'g':
			gpio_config = optarg;
			break;
		case 't':
			if (parse_number("--timeout", optarg, 3600,
					  &val) < 0) {
				usage(stderr);
				exit(2);
			}
			if (val == 0) {
				fprintf(stderr,
					"rsswitchd: invalid --timeout '%s'\n",
					optarg);
				usage(stderr);
				exit(2);
			}
			idle_timeout = (unsigned int)val;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		default:
			usage(stderr);
			exit(2);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "rsswitchd: unexpected argument '%s'\n",
			argv[optind]);
		usage(stderr);
		exit(2);
	}

	socket_gpio_config(gpio_config);

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0)
		fatal("socket");

	{
		int optval = 1;

		/*
		 * Without this, a daemon stopped while a client was
		 * still connected leaves its side of that connection
		 * in TIME_WAIT/FIN_WAIT, and binding the same port
		 * again fails with EADDRINUSE until the kernel times
		 * that state out. SO_REUSEADDR lets a new listener
		 * bind the port while old connections on it are still
		 * being torn down; it does not allow two live
		 * listeners on one port.
		 */
		if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
			       &optval, sizeof(optval)) < 0)
			fatal("setsockopt SO_REUSEADDR");
	}

	memset(&serv_addr, 0, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr = bind_addr;
	serv_addr.sin_port = htons((uint16_t)port);

	if (bind(listenfd, (struct sockaddr *)&serv_addr,
		 sizeof(serv_addr)) < 0) {
		char bind_ipaddr[INET_ADDRSTRLEN];
		char what[64];

		if (inet_ntop(AF_INET, &bind_addr, bind_ipaddr,
			      sizeof(bind_ipaddr)) == NULL)
			snprintf(bind_ipaddr, sizeof(bind_ipaddr), "unknown");
		snprintf(what, sizeof(what), "bind %s:%lu", bind_ipaddr, port);
		fatal(what);
	}

	if (listen(listenfd, 10) < 0)
		fatal("listen");

	{
		struct sockaddr_in bound_addr;
		socklen_t bound_len = sizeof(bound_addr);
		char bound_ipaddr[INET_ADDRSTRLEN];

		if (getsockname(listenfd, (struct sockaddr *)&bound_addr,
				&bound_len) < 0)
			fatal("getsockname");
		if (inet_ntop(AF_INET, &bound_addr.sin_addr, bound_ipaddr,
			      sizeof(bound_ipaddr)) == NULL)
			snprintf(bound_ipaddr, sizeof(bound_ipaddr), "unknown");
		fprintf(stderr, "rsswitchd: listening on %s:%u\n",
			bound_ipaddr,
			(unsigned int)ntohs(bound_addr.sin_port));
	}

	while (1) {
		connfd = accept(listenfd, (struct sockaddr *)&cli_addr,
				&cli_len);
		if (connfd < 0) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "[-] accept: %s", strerror(errno));

			/*
			 * A resource error is likely persistent; pause
			 * so a tight accept()/fail loop cannot spin the
			 * CPU or flood syslog while it lasts. The
			 * daemon keeps running either way -- accept()
			 * failing must never end it.
			 */
			if (errno == EMFILE || errno == ENFILE ||
			    errno == ENOBUFS || errno == ENOMEM) {
				struct timespec pause = { 0, 100000000L };

				nanosleep(&pause, NULL);
			}
			continue;
		}

		/* Log clients IP address. */
		if (inet_ntop(AF_INET, &cli_addr.sin_addr, cli_ipaddr,
			      sizeof(cli_ipaddr)) == NULL)
			snprintf(cli_ipaddr, sizeof(cli_ipaddr), "unknown");
		syslog(LOG_NOTICE, "Client from %s connected.", cli_ipaddr);

		{
			struct timeval tv;

			/*
			 * Bounds every reply write by --timeout, the same
			 * as the idle-command deadline below: a client
			 * that never reads its replies cannot block this
			 * connfd's write() forever (DMN-05). A failure
			 * here is unusual enough that skipping the
			 * connection, rather than treating it as fatal,
			 * keeps the daemon serving everyone else.
			 */
			tv.tv_sec = (time_t)idle_timeout;
			tv.tv_usec = 0;
			if (setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO,
				       &tv, sizeof(tv)) < 0)
				syslog(LOG_ERR,
				       "[-] setsockopt SO_SNDTIMEO: %s",
				       strerror(errno));
			else
				serve_connection(connfd, cli_ipaddr);
		}

		close(connfd);
	}
}

/*
 * Waits for fd to become readable, bounded by deadline (a
 * CLOCK_MONOTONIC timestamp), or indefinitely if deadline is NULL.
 * Recomputes the remaining time and retries on EINTR, so a signal
 * this daemon does not yet catch cannot shorten the wait. The
 * signal-mask argument to pselect() is NULL here -- plan 02-04 passes
 * a real one, to atomically unblock a shutdown signal while waiting.
 *
 * Returns 1 once fd is readable, 0 once deadline has passed with
 * nothing to read, or -1 with errno set on a real pselect() error.
 */
static int wait_readable(int fd, const struct timespec *deadline)
{
	for (;;) {
		struct timespec timeout;
		fd_set rfds;
		int ret;

		if (deadline != NULL) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			timeout.tv_sec = deadline->tv_sec - now.tv_sec;
			timeout.tv_nsec = deadline->tv_nsec - now.tv_nsec;
			if (timeout.tv_nsec < 0) {
				timeout.tv_nsec += 1000000000L;
				timeout.tv_sec--;
			}
			if (timeout.tv_sec < 0) {
				timeout.tv_sec = 0;
				timeout.tv_nsec = 0;
			}
		}

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		ret = pselect(fd + 1, &rfds, NULL, NULL,
			      deadline != NULL ? &timeout : NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		return ret > 0 ? 1 : 0;
	}
}

/*
 * Reads and handles every command sent on connfd, until read()
 * returns 0 (the client closed its end), a real error, or no complete
 * command arrives for idle_timeout seconds. A signal interrupting the
 * read is retried; any other error is logged with the client's
 * address and ends this connection. Every complete command is fed
 * through the protocol parser as soon as it is available, so several
 * commands in one read(), or one command split across several
 * read()s, are both handled correctly (PROTO-01, PROTO-05).
 *
 * The idle deadline is set on entry and re-armed only when proto_next()
 * completes a result -- a command, an ERR or an empty line -- never by
 * partial bytes still buffered in pc: re-arming on every read() would
 * let a client that trickles one byte at a time hold the only
 * connection slot forever, exactly the stall DMN-05 exists to close.
 * A command still incomplete when the deadline passes, or when the
 * connection ends, is dropped without being sent or logged -- a
 * legacy client that never closes its side hits this in normal
 * operation (D-25). The caller closes connfd exactly once, after this
 * function returns.
 */
static void serve_connection(int connfd, const char *ip)
{
	struct proto_conn pc;
	char recbuf[1024];
	struct timespec deadline;

	proto_init(&pc);

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += idle_timeout;

	for (;;) {
		ssize_t n;
		const char *p = recbuf;
		size_t left;
		struct proto_cmd cmd;
		int ready;

		ready = wait_readable(connfd, &deadline);
		if (ready < 0) {
			syslog(LOG_ERR, "[-] pselect for %s: %s", ip,
			       strerror(errno));
			return;
		}
		if (ready == 0)
			return;

		n = read(connfd, recbuf, sizeof(recbuf));

		if (n == 0)
			return;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "[-] read from %s: %s", ip,
			       strerror(errno));
			return;
		}

		left = (size_t)n;
		while (proto_next(&pc, &p, &left, &cmd)) {
			clock_gettime(CLOCK_MONOTONIC, &deadline);
			deadline.tv_sec += idle_timeout;

			if (handle_command(connfd, ip, &cmd) < 0)
				return;
		}
	}
}

/*
 * Turns one parsed command boundary into the corresponding library
 * call, syslog line and reply. PROTO_EMPTY produces neither (D-09).
 * Returns 0 to keep serving this connection, -1 if the reply could
 * not be written (the caller then stops serving it).
 */
static int handle_command(int connfd, const char *ip,
			   const struct proto_cmd *cmd)
{
	switch (cmd->kind) {
	case PROTO_EMPTY:
		return 0;
	case PROTO_ERROR: {
		char reply[96];

		log_err(ip, cmd->err, cmd->text);
		snprintf(reply, sizeof(reply), "ERR %s %s\n",
			 proto_err_name(cmd->err), cmd->text);
		return send_reply(connfd, ip, reply);
	}
	case PROTO_SWITCH:
		if (socket_send(cmd->dev, cmd->group, cmd->socket,
				 cmd->data) == 0) {
			syslog(LOG_NOTICE,
			       "%s: sent dev %" PRIu32 " group %" PRIu32
			       " socket %" PRIu32 " data %" PRIu32,
			       ip, cmd->dev, cmd->group, cmd->socket,
			       cmd->data);
			return send_reply(connfd, ip, "OK\n");
		}
		log_err(ip, PROTO_ERR_SEND, "transmission failed");
		return send_reply(connfd, ip, "ERR send transmission failed\n");
	case PROTO_RAW:
		if (socket_send_codeword(cmd->tristate, cmd->pulse_len) == 0) {
			syslog(LOG_NOTICE,
			       "%s: sent raw %s pulse_len %" PRIu32,
			       ip, cmd->tristate, cmd->pulse_len);
			return send_reply(connfd, ip, "OK\n");
		}
		log_err(ip, PROTO_ERR_SEND, "transmission failed");
		return send_reply(connfd, ip, "ERR send transmission failed\n");
	}

	return 0;
}

/*
 * The single place every ERR result is logged (D-16): one line with
 * the client's address, the fixed class word and the already-built,
 * client-byte-free text.
 */
static void log_err(const char *ip, enum proto_err err, const char *text)
{
	syslog(LOG_ERR, "[-] %s: ERR %s %s", ip, proto_err_name(err), text);
}

/*
 * Writes reply in full to connfd, retrying on EINTR and continuing
 * after a short write. Returns 0 on success, -1 if the write failed
 * for any other reason (logged with the client's address).
 */
static int send_reply(int connfd, const char *ip, const char *reply)
{
	size_t len = strlen(reply);
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = write(connfd, reply + sent, len - sent);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "[-] write to %s: %s", ip,
			       strerror(errno));
			return -1;
		}
		sent += (size_t)n;
	}

	return 0;
}
