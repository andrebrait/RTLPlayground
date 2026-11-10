#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_trap.h"
#if SOC_FAMILY == RTL836X
#include "rtl836x_regs.h"
extern __xdata bool lldp_enabled;
#endif
#include "machine.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __xdata uint8_t sfr_data[4];

void trap_init(void) __banked
{
#if SOC_FAMILY == RTL836X
	/* Reset retained protocol actions before saved configuration replay. */
	trap_rma_set(TRAP_RMA_STP, TRAP_ACT_FORWARD);
	trap_rma_set(TRAP_RMA_LACP, TRAP_ACT_FORWARD);
	trap_rma_set(TRAP_RMA_LLDP, lldp_enabled ? TRAP_ACT_TRAP : TRAP_ACT_FORWARD);
#else
	REG_SET(RTL837X_EXT_CPU_CTRL, CPU_PORT);
	/* Use the common destination selector and normalized action on this port. */
	trap_rma_set(TRAP_RMA_LLDP, MACHINE_DEFAULT_LLDP ? TRAP_ACT_TRAP : TRAP_ACT_FORWARD);
#endif
}

void trap_rma_set(uint8_t rma, __xdata uint8_t act) __banked
{
	__xdata uint16_t reg;

	if (act > TRAP_ACT_DROP)
		return;
	if (rma != TRAP_RMA_STP && rma != TRAP_RMA_LACP && rma != TRAP_RMA_LLDP)
		return;
#if SOC_FAMILY == RTL836X
	/* rtl8367c_setAsicRma: operation in bits 8:7; LLDP's override can supersede RMA 0E. */
	reg = RTL836X_REG_RMA_CTRL00 + rma;
	reg_bits(reg, RMA_OPERATION_MASK, act);
	if (rma == TRAP_RMA_LLDP) {
		reg_bits(RTL836X_REG_RMA_CTRL_LLDP, RMA_OPERATION_MASK, act);
		reg_bits(RTL836X_REG_RMA_LLDP_EN, 1, 1);
	}
#else
	if (rma == TRAP_RMA_STP)
		reg = RTL837X_RMA0_CONF;
	else if (rma == TRAP_RMA_LACP)
		reg = RTL837X_RMA2_CONF;
	else
		reg = RTL837X_RMA0E_CONF;
	reg_read_m(reg);
	sfr_data[3] = (sfr_data[3] & ~RTL837X_RMA_ACT_MASK) | (act << 4);
	reg_write_m(reg);
#endif
}
