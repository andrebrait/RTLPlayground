#ifndef SFP_H
#define SFP_H

#include "platform.h"

#define SFP_WAKE_TICKS	100

extern __xdata uint8_t sfp_quirks[2];

bool sfp_read_field(__xdata char *dst, uint8_t sfp, uint8_t start, uint8_t length) __banked __reentrant;
bool sfp_get_info(uint8_t sfp) __banked;
void sfp_apply_quirks(uint8_t sfp) __banked __reentrant;
void setup_sfp_gpio(void) __banked;

#endif
