#ifndef RPI_H
#define RPI_H

#ifndef PIN
#define PIN RPI_GPIO_P1_07 /* GPIO 4 */
#endif /* PIN */

#ifndef NTIMES
#define NTIMES 2
#endif /* NTIMES */


int32_t board_init(void);
int32_t board_exit(void);
void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* RPI_H */
