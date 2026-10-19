#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_trap.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __xdata uint8_t sfr_data[4];

_Static_assert(RMA_LLDP == RTL837X_RMA0E_CONF, "platform.h token out of step with the register header");
_Static_assert(RMA_ACT_FORWARD == RTL837X_RMA_ACT_FORWARD, "platform.h token out of step with the register header");
_Static_assert(RMA_ACT_TRAP == RTL837X_RMA_ACT_TRAP, "platform.h token out of step with the register header");

void trap_init(void) __banked
{
	REG_SET(RTL837X_EXT_CPU_CTRL, CPU_PORT);
}

void trap_rma_set(uint16_t reg, uint8_t act) __banked
{
	reg_read_m(reg);
	sfr_data[3] = (sfr_data[3] & ~RTL837X_RMA_ACT_MASK) | act;
	reg_write_m(reg);
}
