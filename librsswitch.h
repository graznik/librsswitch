/*
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

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

void send_tris(char *, int);
void send_0(void);
void send_1(void);
void send_f(void);
void send_sync(void);
void transmit(int, int);
int  pt2260_init(Encoder *);
int  pt2262_init(Encoder *);
int  socket_ctrl(Encoder *, uint, uint, uint);
int socket_send(uint, uint, uint, uint);
