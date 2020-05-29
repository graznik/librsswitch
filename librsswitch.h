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

#ifndef LIBRSSWITCH_H
#define LIBRSSWITCH_H

#include <stdint.h>

int32_t socket_send(uint32_t dev, uint32_t group,
		    uint32_t socket, uint32_t data);

enum socket_type {
		  REV_008345,
		  POLLIN_2605
};

enum socket_group {
		  GROUP0,
		  GROUP1,
		  GROUP2,
		  GROUP3,
		  GROUP4,
		  GROUP5,
		  GROUP6,
		  GROUP7,
		  GROUP8,
		  GROUP9,
		  GROUP10,
		  GROUP11,
		  GROUP12,
		  GROUP13,
		  GROUP14,
		  GROUP15,
		  GROUP16,
		  GROUP17,
		  GROUP18,
		  GROUP19,
		  GROUP20,
		  GROUP21,
		  GROUP22,
		  GROUP23,
		  GROUP24,
		  GROUP25,
		  GROUP26,
		  GROUP27,
		  GROUP28,
		  GROUP29,
		  GROUP30,
		  GROUP31,
};

enum socket {
		  SOCKET0,
		  SOCKET1,
		  SOCKET2,
		  SOCKET3,
		  SOCKET4,
		  SOCKET5,
		  SOCKET6,
		  SOCKET7,
		  SOCKET8,
		  SOCKET9,
		  SOCKET10,
		  SOCKET11,
		  SOCKET12,
		  SOCKET13,
		  SOCKET14,
		  SOCKET15,
		  SOCKET16,
		  SOCKET17,
		  SOCKET18,
		  SOCKET19,
		  SOCKET20,
		  SOCKET21,
		  SOCKET22,
		  SOCKET23,
		  SOCKET24,
		  SOCKET25,
		  SOCKET26,
		  SOCKET27,
		  SOCKET28,
		  SOCKET29,
		  SOCKET30,
		  SOCKET31,
};

enum socket_state {
		  SOCK_OFF,
		  SOCK_ON
};

#endif /* LIBRSSWITCH_H */
