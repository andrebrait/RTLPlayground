#ifndef _RTL836X_MIB_H_
#define _RTL836X_MIB_H_

#include <stdint.h>
#include <stdbool.h>

#define RTL836X_MIB_COUNT 58

/* Native per-port order; octets at 0, 19 and 28 are 64-bit, others 32-bit.
 * Values are eight big-endian bytes. False means unavailable, not zero. */
bool rtl836x_mib_read(uint8_t port, __xdata uint8_t counter, __xdata uint8_t * __xdata value) __banked;
/* 0: TX U/M/B sum; 1: TX excessive collisions; 2: RX U/M/B sum; 3: RX FCS errors. */
bool rtl836x_mib_summary(__xdata uint8_t port, __xdata uint8_t metric, __xdata uint8_t * __xdata value) __banked;

#endif
