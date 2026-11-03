/*
 * RTL836x stand-ins for the RTL837x feature modules that have no RTL836x
 * implementation yet: bandwidth, storm control, IGMP, cable test, GPIO/I2C
 * pins and the trap setup.
 */
#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"

#pragma codeseg BANK3
#pragma constseg BANK3
