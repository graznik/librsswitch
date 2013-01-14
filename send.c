#include "librsswitch.h"

int main(int argc, char *argv[])
{
	/* Group A, Switch 2, On */
	socket_send(0, 0, 0, 1);

	return 0;
}
