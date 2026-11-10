/*
 * Port LEDs of the RTL836x: parallel LED outputs driven by the LED engine.
 * The board's LED wiring is set up in machine_custom_init().
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_leds.h"

#pragma codeseg BANK2
#pragma constseg BANK2


/* Parallel LED outputs in hardware mode, as the TP-Link firmware runs them */
void leds_setup(void) __banked
{
	REG_SET(RTL836X_REG_PARA_LED_IO_EN1, 0xffff);
	REG_SET(RTL836X_REG_PARA_LED_IO_EN2, 0xffff);
	REG_SET(RTL836X_REG_LED_SYS_CONFIG, 0x1472);
}


void leds_dump(void) __banked
{
	print_string("not supported\n");
}
