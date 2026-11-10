#ifndef _POE_H_
#define _POE_H_

#include <stdint.h>
#include <stdbool.h>

/* GPIO-backed administration only: these functions do not measure delivered power.
 * Port numbers are physical, one-based; invalid/non-PoE ports are refused. */
bool poe_port_set(uint8_t port, __xdata bool enabled) __banked;
/* Bit 0 is physical port 1. A set bit permits the controller to negotiate PoE. */
uint8_t poe_admin_mask(void) __banked;

#endif
