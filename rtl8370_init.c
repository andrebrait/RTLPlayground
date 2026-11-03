/*
 * Switch initialization for the RTL8370N, as the TP-Link boot code's loader
 * mode does it (Realtek SDK rtk_switch_init() for the RTL8370B).
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_init.h"

#pragma codeseg BANK2
#pragma constseg BANK2
