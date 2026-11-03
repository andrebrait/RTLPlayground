/*
 * The port interface of rtl837x_port.c for the RTL836x. Port status and MIB
 * counters are implemented; VLAN, L2, LAG and isolation stay at hardware defaults,
 * which forward between all ports and to the CPU port.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"
#include "rtl837x_port.h"
#include "machine.h"

#pragma codeseg BANK1
#pragma constseg BANK1


void port_stats_print(void) __banked
{
	__xdata uint8_t value[8];
	print_string("\nPort\tState\tLink\tTX U/M/B\tTX excess\tRX U/M/B\tRX FCS\n");
	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		print_phys_port(i); write_char('\t');
		phy_read(i, 0, 0);
		print_string(SFR_DATA_8 & 0x08 ? "Off\t" : "On\t");	/* BMCR power down */
		reg_read(RTL836X_REG_PORT0_STATUS + i);
		if (!(SFR_DATA_0 & PORT_STATUS_LINK))
			print_string("Down\t");
		else if ((SFR_DATA_0 & PORT_STATUS_SPEED_MASK) == 2)
			print_string("1000M\t");
		else if (SFR_DATA_0 & PORT_STATUS_SPEED_MASK)
			print_string("100M\t");
		else
			print_string("10M\t");
		for (uint8_t metric = 0; metric < 4; metric++) {
			if (rtl836x_mib_summary(i, metric, value)) {
				print_string("0x");
				for (uint8_t b = 0; b < 8; b++)
					print_byte(value[b]);
			} else {
				print_string("n/a");
			}
			write_char(metric == 3 ? '\n' : '\t');
		}
	}
}


void vlan_setup(void) __banked
{
}


void port_l2_setup(void) __banked
{
}


void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked
{
	(void)mac;
	(void)vlan;
	(void)remove_entry;
}


/* No L2 table and no LAGs yet. Only "stp on" or a LACP group reach the setters, and the RTL836x CLI refuses both */
void port_l2_forget_port(uint8_t port) __banked
{
	(void)port;
}


uint16_t port_lag_members_get(uint8_t lag) __banked
{
	(void)lag;
	return 0;
}


void port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked
{
	(void)lag;
	(void)members;
}
