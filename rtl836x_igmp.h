#ifndef _RTL836X_IGMP_H_
#define _RTL836X_IGMP_H_

#include <stdint.h>
#include <stdbool.h>

/* Network-order group; front chip port. False means the requested state was not verified. */
bool rtl836x_igmp_update(__xdata uint8_t *group, __xdata uint8_t port, __xdata bool join) __banked;

#endif
