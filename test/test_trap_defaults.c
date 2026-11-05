/* trap_init() sets up LLDP trapping the way "lldp on|off" does, from MACHINE_DEFAULT_LLDP.
 * Built twice, with MACHINE_DEFAULT_LLDP 0 and 1; rtl837x_trap.c is included unmodified. */
#include <stdio.h>
#include "../rtl837x_trap.c"

volatile uint8_t SFR_DATA_24, SFR_DATA_16, SFR_DATA_8, SFR_DATA_0;
__xdata uint8_t sfr_data[4];
static uint32_t rma0e = 0xffffffff;	/* every bit set: only the action field may change */

void reg_write(uint16_t reg) { (void)reg; }
void reg_read_m(uint16_t reg)
{
	uint32_t v = reg == RTL837X_RMA0E_CONF ? rma0e : 0;
	sfr_data[0] = v >> 24; sfr_data[1] = v >> 16; sfr_data[2] = v >> 8; sfr_data[3] = v;
}
void reg_write_m(uint16_t reg)
{
	if (reg == RTL837X_RMA0E_CONF)
		rma0e = ((uint32_t)sfr_data[0] << 24) | ((uint32_t)sfr_data[1] << 16) | ((uint32_t)sfr_data[2] << 8) | sfr_data[3];
}

int main(void)
{
	uint8_t want = MACHINE_DEFAULT_LLDP ? 0x10 : 0x00; /* RTL837x action field */
	trap_init();
	if ((rma0e & RTL837X_RMA_ACT_MASK) != want || (rma0e | RTL837X_RMA_ACT_MASK) != 0xffffffff) {
		printf("FAIL: MACHINE_DEFAULT_LLDP=%d leaves RMA0E at 0x%08x\n", MACHINE_DEFAULT_LLDP, (unsigned)rma0e);
		return 1;
	}
	printf("PASS: MACHINE_DEFAULT_LLDP=%d sets the LLDP trap action as \"lldp %s\" does\n",
	       MACHINE_DEFAULT_LLDP, MACHINE_DEFAULT_LLDP ? "on" : "off");
	return 0;
}
