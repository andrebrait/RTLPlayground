#ifndef _RTL837X_STP_H_
#define _RTL837X_STP_H_

#include <stdint.h>
#include "rtl837x_common.h"
void stp_in(void) __banked;
void stp_setup(void) __banked;
void stp_timers(void) __banked;
void stp_off(void) __banked;
void stp_parse(void) __banked __reentrant;
void stp_defaults(void) __banked;

#endif
