#ifndef BBB_H
#define BBB_H

#ifndef PIN
#define PIN "49"
#endif /* PIN */

#ifndef NTIMES
#define NTIMES 10
#endif /* NTIMES */


int32_t board_init(void);
int32_t board_exit(void);
void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* BBB_H */
