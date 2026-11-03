/*
 * Register and PHY access for the RTL836x (RTL8370N): 16-bit registers,
 * written through SFR a4:a5 and read back from SFR a6:a7.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
