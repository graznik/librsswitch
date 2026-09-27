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

int main(int argc, char *argv[])
{
	int listenfd = 0, connfd = 0, n = 0;
	const char *gpio_config = NULL;
	struct sockaddr_in serv_addr, cli_addr;
	socklen_t cli_len = sizeof(cli_addr);
	char cli_ipaddr[INET_ADDRSTRLEN];
	struct in_addr bind_addr;
	unsigned long port = DEFAULT_PORT;
	int opt;

	char recbuf[1025];

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
	memset(&serv_addr, '0', sizeof(serv_addr));
	memset(recbuf, '0', sizeof(recbuf));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr = bind_addr;
	serv_addr.sin_port = htons((uint16_t)port);

	bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

	listen(listenfd, 10);

	openlog("rsswitchd", 0, LOG_USER);

	while (1) {
		connfd = accept(listenfd, (struct sockaddr *)&cli_addr,
				&cli_len);
		/* Log clients IP address. */
		inet_ntop(AF_INET, &cli_addr.sin_addr,
			  cli_ipaddr, sizeof(cli_ipaddr));
		syslog(LOG_NOTICE, "Client from %s connected.", cli_ipaddr);

		while ((n = read(connfd, recbuf, sizeof(recbuf)-1)) > 0) {
			recbuf[n] = 0;

			if (n < 0)
				printf("\nRead error\n");

			if (check_input(recbuf, n) == 0)
				sock_ctrl(recbuf);

			close(connfd);
			sleep(1);
		}
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
