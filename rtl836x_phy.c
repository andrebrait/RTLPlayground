/* RTL8370N internal GPHY controls using the Clause 22 register window.
 * Register layout and negotiation sequence checked against the RTL8367C SDK. */
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_phy.h"
#include "rtl836x_regs.h"
#include "cmd_parser.h"
#include "machine.h"
#include "phy.h"

#pragma codeseg BANK2
#pragma constseg BANK2

__xdata struct phy_settings phy_settings;

static void invalid_mode(void)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: unsupported PHY speed/duplex combination\n");
}

/* Restrict advertised modes rather than forcing a duplex mismatch with the peer. */
static void set_abilities(__xdata uint16_t advertised, __xdata uint16_t gigabit, __xdata bool enable)
{
	phy_read(phy_settings.port, 0, 9);
	phy_write(phy_settings.port, 0, 9, (SFR_DATA_U16 & ~0x0300) | gigabit);
	phy_read(phy_settings.port, 0, 4);
	phy_write(phy_settings.port, 0, 4, (SFR_DATA_U16 & ~0x01e0) | advertised);
	phy_read(phy_settings.port, 0, 0);
	uint16_t control = (SFR_DATA_U16 & ~0x3140) | 0x1200;
	if (enable)
		control &= ~0x0800;
	if (gigabit)
		control |= 0x0140;
	else if (advertised & 0x0180) {
		control |= 0x2000;
		if (advertised & 0x0100)
			control |= 0x0100;
	} else if (advertised & 0x0040)
		control |= 0x0100;
	phy_write(phy_settings.port, 0, 0, control);
}

void phy_set_speed(void) __banked
{
	if (!MACHINE_PORT_VALID(phy_settings.port)) {
		invalid_mode();
		return;
	}
	if (phy_settings.speed == PHY_OFF) {
		phy_read(phy_settings.port, 0, 0);
		phy_write(phy_settings.port, 0, 0, SFR_DATA_U16 | 0x0800);
		return;
	}
	__xdata uint16_t advertised = 0;
	__xdata uint16_t gigabit = 0;
	switch (phy_settings.speed) {
	case PHY_SPEED_AUTO:
		advertised = 0x01e0;
		gigabit = 0x0200;
		break;
	case PHY_SPEED_1G:
		gigabit = 0x0200;
		break;
	case PHY_SPEED_10M:
	case PHY_SPEED_100M:
		if (phy_settings.duplex > PHY_DUPLEX_BOTH) {
			invalid_mode();
			return;
		}
		if (phy_settings.duplex != PHY_DUPLEX_FULL)
			advertised |= 0x0020;
		if (phy_settings.duplex != PHY_DUPLEX_HALF)
			advertised |= 0x0040;
		if (phy_settings.speed == PHY_SPEED_100M)
			advertised <<= 2;
		break;
	default:
		invalid_mode();
		return;
	}
	set_abilities(advertised, gigabit, true);
}

void phy_set_duplex(void) __banked
{
	if (!MACHINE_PORT_VALID(phy_settings.port) || phy_settings.duplex > PHY_DUPLEX_FULL) {
		invalid_mode();
		return;
	}
	phy_read(phy_settings.port, 0, 4);
	__xdata uint16_t advertised = SFR_DATA_U16;
	__xdata uint16_t modes = 0;
	if (advertised & 0x0060)
		modes |= phy_settings.duplex == PHY_DUPLEX_FULL ? 0x0040 : 0x0020;
	if (advertised & 0x0180)
		modes |= phy_settings.duplex == PHY_DUPLEX_FULL ? 0x0100 : 0x0080;
	phy_read(phy_settings.port, 0, 9);
	__xdata uint16_t gigabit = phy_settings.duplex == PHY_DUPLEX_FULL ? SFR_DATA_U16 & 0x0200 : 0;
	if (!(modes | gigabit)) {
		invalid_mode();
		return;
	}
	set_abilities(modes, gigabit, false);
}

void phy_show(uint8_t port) __banked
{
	phy_read(port, 0, 0);
	uint16_t control = SFR_DATA_U16;
	print_string(control & 0x0800 ? "\nState: Off" : "\nState: On");
	print_string(control & 0x1000 ? "\nAuto-negotiation: On" : "\nAuto-negotiation: Off");
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	uint16_t status = SFR_DATA_U16;
	print_string("\nLink: ");
	if (!(status & PORT_STATUS_LINK))
		print_string("Down");
	else {
		switch (status & PORT_STATUS_SPEED_MASK) {
		case 0: print_string("10M"); break;
		case 1: print_string("100M"); break;
		case 2: print_string("1000M"); break;
		default: print_string("Unknown"); break;
		}
		print_string(status & 0x0004 ? " full duplex" : " half duplex");
	}
	write_char('\n');
}
