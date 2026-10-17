#ifndef BBB_H
#define BBB_H

#include "../board.h"

/*
 * Pin P9_23, config "chip:line". On kernel 6.x, such as on Debian 13,
 * it is line 17 of gpiochip0. On other kernels its chip can differ, so
 * check it with gpioinfo and pass "chip:line" instead.
 */
#define BBB_DEFAULT_CHIPNAME "gpiochip0"
#define BBB_DEFAULT_LINE_OFFSET 17

#endif /* BBB_H */
