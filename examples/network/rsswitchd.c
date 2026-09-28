#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../../librsswitch.h"

static int check_input(char *input, int len);
static int sock_ctrl(char *recbuf);
static void serve_connection(int connfd, const char *ip);

#define CODE_LEN 4
#define DEFAULT_PORT 5000
#define DEFAULT_BIND "0.0.0.0"

static const struct option long_opts[] = {
	{"port", required_argument, NULL, 'p'},
	{"bind", required_argument, NULL, 'b'},
	{"gpio", required_argument, NULL, 'g'},
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
		"Usage: rsswitchd [--port PORT] [--bind ADDR] [--gpio CONFIG] [--help]\n"
		"  --port PORT    TCP port to listen on, 0-65535; 0 lets the\n"
		"                 kernel pick a free port (default 5000)\n"
		"  --bind ADDR    IPv4 address to listen on (default 0.0.0.0,\n"
		"                 meaning all addresses)\n"
		"  --gpio CONFIG  board-specific GPIO config (default: the\n"
		"                 board's built-in default)\n"
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

		serve_connection(connfd, cli_ipaddr);
		close(connfd);
	}
}

/*
 * Reads and handles every command sent on connfd, until read()
 * returns 0 (the client closed its end) or a real error. A signal
 * interrupting the read is retried; any other error is logged with
 * the client's address and ends this connection. The caller closes
 * connfd exactly once, after this function returns.
 */
static void serve_connection(int connfd, const char *ip)
{
	char recbuf[1025];

	memset(recbuf, 0, sizeof(recbuf));

	for (;;) {
		ssize_t n = read(connfd, recbuf, sizeof(recbuf) - 1);

		if (n == 0)
			return;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "[-] read from %s: %s", ip,
			       strerror(errno));
			return;
		}

		recbuf[n] = 0;

		if (check_input(recbuf, (int)n) == 0)
			sock_ctrl(recbuf);
	}
}

static int check_input(char *input, int len)
{
	/* Check if string contains only digits */
	int i;

	if (len < CODE_LEN) {
		syslog(LOG_ERR, "Input too short.");
		return -1;
	}

	for (i = 0; i < CODE_LEN; i++) {
		if (isdigit(input[i]) == 0) {
			syslog(LOG_ERR, "Invalid input.");
			return -1;
		}
	}

	return 0;
}

static int sock_ctrl(char *recbuf)
{
	syslog(LOG_NOTICE, "RSS Control");
#ifdef DEBUG
	syslog(LOG_NOTICE, "dev: %d, group: %d, socket: %d, data: %d",
	       recbuf[0] - 48, recbuf[1] - 48, recbuf[2] - 48, recbuf[3] - 48);
#endif
	/*librrswitch needs integer values */
	socket_send(recbuf[0] - 48,
		    recbuf[1] - 48,
		    recbuf[2] - 48,
		    recbuf[3] - 48);

	return 0;
}
