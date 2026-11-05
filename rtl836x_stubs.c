/*
 * RTL836x stand-ins for unported cable-test and GPIO/I2C modules.
 * Their CLI commands remain disabled; only common runtime hooks are here.
 */
#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_cabletest.h"
#include "rtl837x_pins.h"

#pragma codeseg BANK3
#pragma constseg BANK3

void cabletest_tick(void) __banked
{
}


/* The board has no reset button and no SFP cages (reset_pin GPIO_NA, n_sfp 0) */
void gpio_input_setup(uint8_t pin) __banked
{
	(void)pin;
}


void gpio_output_setup(uint8_t pin, __xdata uint8_t initial_val) __banked
{
	(void)pin;
	(void)initial_val;
}


bool sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len) __banked __reentrant
{
	(void)slot;
	(void)reg;
	(void)len;
	return false;
}
