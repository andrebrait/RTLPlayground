#ifndef _RTL836X_RATE_H_
#define _RTL836X_RATE_H_

#include <stdint.h>

#define RTL836X_RATE_MIN_KBPS 8UL
#define RTL836X_RATE_MAX_KBPS 1000000UL
#define RTL836X_RATE_OFF 0x1ffffUL

/* Native counts are 8 kbit/s; OFF is unlimited. ingress is 0 or 1. */
uint32_t rtl836x_bw_get(uint8_t port, __xdata uint8_t ingress) __banked;
/* Read the selected meter, in kbit/s. Both getters return UINT32_MAX for invalid ports/types. */
uint32_t rtl836x_storm_get(uint8_t port, __xdata uint8_t type) __banked;

#endif
