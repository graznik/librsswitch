#include <stdlib.h>
#include <bcm2835.h>
#ifdef DEBUG
#include <syslog.h>
#endif

#include "rpi.h"

int32_t board_init(void)
{
	/* Initialize the IO pin */
	if (!bcm2835_init()) {
		syslog(LOG_ERR, "Cannot init bcm2835\n");
		return -1;
	}

	/* Set the pin to be an output */
	bcm2835_gpio_fsel(PIN, BCM2835_GPIO_FSEL_OUTP);

	return 0;
}

int32_t board_exit(void)
{
	return 0;
}

void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	bcm2835_gpio_write(PIN, HIGH);
	delayMicroseconds(pulse_len * nhigh);
	bcm2835_gpio_write(PIN, LOW);
	delayMicroseconds(pulse_len * nlow);
}
