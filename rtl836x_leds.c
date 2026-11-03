/*
 * Port LEDs of the RTL836x: parallel LED outputs driven by the LED engine.
 * The board's LED wiring is set up in machine_custom_init().
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_leds.h"

#pragma codeseg BANK2
#pragma constseg BANK2
