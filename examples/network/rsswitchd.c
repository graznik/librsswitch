#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../../librsswitch.h"

int check_input(char *input);
int sock_ctrl(char *recbuf);

#define CODE_LEN 4

int main(void)
{
	int listenfd = 0, connfd = 0, n = 0;
	struct sockaddr_in serv_addr, cli_addr;
	socklen_t cli_len = sizeof(cli_addr);
	char cli_ipaddr[INET_ADDRSTRLEN];

	char recbuf[1025];

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&serv_addr, '0', sizeof(serv_addr));
	memset(recbuf, '0', sizeof(recbuf));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(5000);

	bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

	listen(listenfd, 10);

	openlog("rsswitchd", 0, LOG_USER);

	while(1) {
		connfd = accept(listenfd, (struct sockaddr*)&cli_addr, &cli_len);
		/* Log clients IP address. */
		inet_ntop(AF_INET, &cli_addr.sin_addr,
			  cli_ipaddr, sizeof(cli_addr));
		syslog(LOG_NOTICE, "Client from %s connected.", cli_ipaddr);

		while ( (n = read(connfd, recbuf, sizeof(recbuf)-1)) > 0) {
			recbuf[n] = 0;

			if (n < 0) {
				printf("\nRead error\n");
			}

			if (check_input(recbuf) == 0 ) {
				sock_ctrl(recbuf);

			}
			close(connfd);
			sleep(1);
		}
	}
}

int check_input(char *input)
{
	/* Check if string contains only digits */
	int i;
	for (i = 0; i < CODE_LEN; i++) {
		if (isdigit(input[i]) == 0) {
			syslog(LOG_ERR, "Invalid input.");
			return -1;
		}
	}

	return 0;
}

int sock_ctrl(char *recbuf)
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
