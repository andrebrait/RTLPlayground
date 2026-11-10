#include "machine_defs.h"

/* Front port N = chip port N-1. The identity block at 0x1fc000 and the TP-Link
 * settings at 0x1ff000 are only read: every flash writer stays below 0x100000. */
__code const struct machine machine = {
	MACHINE_COMMON_SETTING,
	.n_sfp = 0,
	.log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 0},
	.phys_to_log_port = {0, 1, 2, 3, 4, 5, 6, 7, 0xff},	/* [8] must be invalid: MACHINE_PORT_VALID(0) is true */
	.reset_pin = GPIO_NA,	/* the default 0 is a real GPIO */
	.mac_flash_offset = 0x1fc000,
};
