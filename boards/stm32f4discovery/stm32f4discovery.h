#ifndef STM32F4DISCOVERY_H
#define STM32F4DISCOVERY_H

#include <stdint.h>

#ifndef PIN
#define PIN GPIO11
#endif /* PIN */

#ifndef NTIMES
#define NTIMES 4
#endif /* NTIMES */


int32_t board_init(void);
int32_t board_exit(void);
void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* RPI_H */
