/*
 * RTL836x stand-ins for the RTL837x feature modules that have no RTL836x
 * implementation yet: bandwidth, IGMP, cable test, GPIO/I2C pins, the trap
 * setup and the PHY settings. Only what common code calls at runtime is here;
 * the CLI commands of these features are compiled out on the RTL836x.
 */
#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_bandwidth.h"
#include "rtl837x_cabletest.h"
#include "rtl837x_igmp.h"
#include "rtl837x_pins.h"
#include "rtl837x_trap.h"
#include "rtl837x_phy.h"

#pragma codeseg BANK3
#pragma constseg BANK3

/* parse_port() keeps the port it works on here */
__xdata struct phy_settings phy_settings;


void bandwidth_setup(void) __banked
{
}


void igmp_setup(void) __banked
{
}


uint8_t igmp_packet_handler(void) __banked
{
	return 0;
}


void cabletest_tick(void) __banked
{
}


void trap_init(void) __banked
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
