#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <bcm2835.h>
#ifdef DEBUG
#include <syslog.h>
#endif

#define PIN RPI_GPIO_P1_07 /* GPIO 4 */

typedef struct{
	char **groups;
	uint ngroups; 
	char **sockets;
	uint nsockets; 
	char **data;
	uint ndata; 
	uint pulse_len;
} Encoder;

int socket_send(uint, uint, uint, uint);
