/* RTL8370B multicast register facts: RTL8367C SDK IGMP and LUT APIs. */
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_igmp.h"
#include "rtl836x_igmp.h"
#include "cmd_parser.h"
#include "machine.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __xdata uint8_t igmpEnabled;

static __xdata uint16_t igmp_key[6], igmp_members, igmp_flags;

static void igmp_protocols(__xdata uint16_t operations)
{
	__xdata uint8_t p;
	for (p = MACHINE_MIN_PORT; p <= MACHINE_MAX_PORT; p++)
		reg_bits(RTL836X_REG_IGMP_PORT0_CTRL + p, RTL836X_IGMP_PORT_FIELDS,
			 RTL836X_IGMP_ALLOW_ALL | operations);
	reg_bits(RTL836X_REG_IGMP_PORT8_CTRL + RTL836X_CPU_PORT - 8, RTL836X_IGMP_PORT_FIELDS,
		 RTL836X_IGMP_ALLOW_ALL | RTL836X_IGMP_MLD_FLOOD);
}

/* shortcut: 256 polls, use a timed wait if documented operations exceed this bound. */
static bool igmp_lut_idle(void)
{
	__xdata uint8_t polls = 0;
	do {
		reg_read(RTL836X_REG_TABLE_LUT_ADDR);
		if (!(SFR_DATA_U16 & TABLE_LUT_ADDR_BUSY))
			return true;
	} while (++polls);
	return false;
}

/* -1 unavailable, 0 absent, 1 present; lookup by the complete SIP/DIP key. */
static int8_t igmp_lut_read(void)
{
	__xdata uint8_t i;
	if (!igmp_lut_idle())
		return -1;
	for (i = 0; i < 6; i++)
		REG_SET(RTL836X_REG_TABLE_WRITE_DATA0 + i, igmp_key[i]);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, TABLE_ACCESS_LUT_KEY);
	if (!igmp_lut_idle())
		return -1;
	igmp_members = igmp_flags = 0;
	if (!(SFR_DATA_U16 & TABLE_LUT_HIT))
		return 0;
	reg_read(RTL836X_REG_TABLE_READ_DATA0 + 3);
	igmp_members = (SFR_DATA_U16 & 0xc000) >> 6;
	reg_read(RTL836X_REG_TABLE_READ_DATA0 + 4);
	igmp_members |= SFR_DATA_U16 & 0x00ff;
	reg_read(RTL836X_REG_TABLE_READ_DATA0 + 5);
	igmp_flags = SFR_DATA_U16;
	igmp_members |= (SFR_DATA_U16 & 0x0080) << 3;
	return 1;
}

void igmp_setup(void) __banked
{
	igmpEnabled = 0;
	reg_bit_clear(RTL836X_REG_LUT_CFG, RTL836X_LUT_IPMC_HASH_BIT);
	reg_bit_clear(RTL836X_REG_IGMP_MLD_CFG0, 0);
	igmp_protocols(RTL836X_IGMP_MLD_FLOOD);
	REG_SET(RTL836X_REG_UNKNOWN_IPV4_CTRL0, 0);
	reg_bits(RTL836X_REG_UNKNOWN_IPV4_CTRL1, 0x0030, 0);
	igmp_router_port_set(0);
}

void igmp_enable(void) __banked
{
	__xdata uint8_t i;
	/* Empty classification exceptions make lookup use DIP with SIP zero. */
	for (i = 0; i < 4; i++)
		REG_SET(RTL836X_REG_IPMC_GROUP_VALID0 + i, 0);
	reg_bit_clear(RTL836X_REG_LUT_CFG2, 0);
	reg_bits(RTL836X_REG_LUT_CFG, 0x0018, 3);
	igmp_protocols(RTL836X_IGMP_TRAP_IPV4);
	reg_bit_set(RTL836X_REG_IGMP_MLD_CFG0, 0);
	REG_SET(RTL836X_REG_UNKNOWN_IPV4_CTRL0, 0x5555);
	igmpEnabled = 1;
}

void igmp_router_port_set(uint16_t pmask) __banked
{
	if (pmask & ~(0x00ff | PMASK_CPU)) {
		err_status = ERR_INVALID_ARGUMENT;
		print_string("Invalid IGMP router port mask\n");
		return;
	}
	reg_bits(RTL836X_REG_IGMP_ROUTER_PORT, 0x07ff,
		 (pmask & 0x00ff) | ((pmask & PMASK_CPU) << 1));
}

void igmp_show(void) __banked
{
	__xdata uint8_t p;
	print_string(igmpEnabled ? "IGMP on\n" : "IGMP off\n");
	for (p = MACHINE_MIN_PORT; p <= MACHINE_MAX_PORT; p++) {
		print_string("port "); print_phys_port(p); print_string(": ");
		reg_read(RTL836X_REG_IGMP_PORT0_CTRL + p);
		print_short(SFR_DATA_U16); write_char('\n');
	}
}

bool rtl836x_igmp_update(__xdata uint8_t *group, __xdata uint8_t port, __xdata bool join) __banked
{
	__xdata uint8_t i;
	__xdata int8_t found;
	__xdata uint16_t desired;
	if (!MACHINE_PORT_VALID(port) || (group[0] & 0xf0) != 0xe0)
		return false;
	igmp_key[0] = igmp_key[1] = igmp_key[4] = igmp_key[5] = 0;
	igmp_key[2] = (uint16_t)group[2] << 8 | group[3];
	igmp_key[3] = 0x1000 | ((uint16_t)(group[0] & 15) << 8) | group[1];
	found = igmp_lut_read();
	if (found < 0)
		return false;
	if (!found && !join)
		return true;
	desired = join ? igmp_members | ((uint16_t)1 << port) : igmp_members & ~((uint16_t)1 << port);
	if (desired && desired == igmp_members && (igmp_flags & 0x0021) == 0x0020)
		return true;
	igmp_key[3] |= (desired & 0x0300) << 6;
	igmp_key[4] = desired & 0x00ff;
	igmp_key[5] = desired ? 0x0020 | ((desired & 0x0400) >> 3) : 0;
	if (!igmp_lut_idle())
		return false;
	for (i = 0; i < 6; i++)
		REG_SET(RTL836X_REG_TABLE_WRITE_DATA0 + i, igmp_key[i]);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, TABLE_ACCESS_LUT_WRITE);
	if (!igmp_lut_idle())
		return false;
	/* A completed write can still miss a full LUT; verify the requested membership. */
	found = igmp_lut_read();
	return desired ? found == 1 && igmp_members == desired && (igmp_flags & 0x0021) == 0x0020 : found == 0;
}
