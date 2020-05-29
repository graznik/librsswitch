#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>

#include "stm32f0discovery.h"

static void delay_setup(void)
{
	rcc_periph_clock_enable(RCC_TIM6);
	/* microsecond counter */
        timer_set_prescaler(TIM6, rcc_apb1_frequency / 1e6 - 1);
	timer_set_period(TIM6, 0xffff);
	timer_one_shot_mode(TIM6);
}

static void delay_us(uint16_t us)
{
	TIM_ARR(TIM6) = us;
	TIM_EGR(TIM6) = TIM_EGR_UG;
	TIM_CR1(TIM6) |= TIM_CR1_CEN;
	/* timer_enable_counter(TIM6); */
	while (TIM_CR1(TIM6) & TIM_CR1_CEN)
		;
}

int32_t board_init(void)
{
	/* Enable GPIOD clock. */
	rcc_periph_clock_enable(RCC_GPIOA);

	/* Set GPIO11 (in GPIO port A) to 'output push-pull'. */
	gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO11);

	delay_setup();

	return 0;
}

int32_t board_exit(void)
{
	return 0;
}

void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	gpio_set(GPIOA, PIN);
	delay_us(pulse_len * nhigh);
	gpio_clear(GPIOA, PIN);
	delay_us(pulse_len * nlow);
}
