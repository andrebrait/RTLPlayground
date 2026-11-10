/* RTL8370N internal GPHY controls using the Clause 22 register window.
 * Register layout and negotiation sequence checked against the RTL8367C SDK. */
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_phy.h"
#include "rtl836x_regs.h"
#include "cmd_parser.h"
#include "machine.h"
#include "rtl837x_port.h"
#include "phy.h"
#include "rtl837x_pause.h"

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

/* Advertise symmetric pause or none; the speed setters keep these bits */
void port_pause_set(__xdata uint8_t port, __xdata bool on) __banked
{
	uint16_t want = on ? PHY_ANEG_PAUSE : 0;
	phy_read(port, 0, 4);
	if ((SFR_DATA_U16 & (PHY_ANEG_PAUSE | PHY_ANEG_ASYM_PAUSE)) == want)
		return;
	phy_write(port, 0, 4, (SFR_DATA_U16 & ~(PHY_ANEG_PAUSE | PHY_ANEG_ASYM_PAUSE)) | want);
	phy_read(port, 0, 0);
	phy_write(port, 0, 0, SFR_DATA_U16 | 0x0200);	/* restart, but never power up */
}

/* PAUSE_* bits: MII ANAR and ANLPAR, and the MAC's negotiated pause in PORTn_STATUS */
uint8_t port_pause_state(uint8_t port) __banked
{
	uint8_t s;
	phy_read(port, 0, 4);
	s = (SFR_DATA_U16 >> 10) & 0x03;
	phy_read(port, 0, 5);
	s |= ((SFR_DATA_U16 >> 10) & 0x03) << 2;
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	if (SFR_DATA_U16 & PORT_STATUS_LINK) {
		if (SFR_DATA_U16 & PORT_STATUS_TX_PAUSE)
			s |= PAUSE_NEG_TX;
		if (SFR_DATA_U16 & PORT_STATUS_RX_PAUSE)
			s |= PAUSE_NEG_RX;
	}
	return s;
}

static void set_eee(__xdata uint8_t port, __xdata uint8_t modes, __xdata bool restart)
{
	phy_read(port, 0, RTL836X_OCP_EEE_ADV);
	uint16_t adv = SFR_DATA_U16;
	reg_read(RTL836X_REG_PORT0_EEE_CFG + ((uint16_t)port << 5));
	uint16_t config = SFR_DATA_U16 & ~(RTL836X_EEE_100M | RTL836X_EEE_1G);
	if (modes & 2)
		config |= RTL836X_EEE_100M;
	if (modes & 4)
		config |= RTL836X_EEE_1G;
	/* The Ports page sends eee with every change of a port: leave the link alone if nothing changes */
	if ((adv & 0x0006) == modes && config == SFR_DATA_U16)
		return;
	phy_write(port, 0, RTL836X_OCP_EEE_ADV, (adv & ~0x0006) | modes);
	REG_SET(RTL836X_REG_PORT0_EEE_CFG + ((uint16_t)port << 5), config);
	if (restart) {
		phy_read(port, 0, 0);
		phy_write(port, 0, 0, SFR_DATA_U16 | 0x0200);
	}
}

void port_eee_enable(__xdata uint8_t port, __xdata uint8_t speed) __banked
{
	uint8_t limit = speed & ~EEE_NORESET;
	if (!MACHINE_PORT_VALID(port) || (limit != EEE_100 && limit != EEE_1000)) {
		invalid_mode();
		return;
	}
	set_eee(port, limit == EEE_100 ? 2 : 6, !(speed & EEE_NORESET));
}

void port_eee_disable(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port)) {
		invalid_mode();
		return;
	}
	set_eee(port, 0, true);
}

void port_eee_status(uint8_t port) __banked
{
	print_string("Port "); print_phys_port(port); print_string(" EEE advertised:");
	phy_read(port, 0, RTL836X_OCP_EEE_ADV);
	uint16_t advertised = SFR_DATA_U16;
	if (advertised & 2) print_string(" 100M");
	if (advertised & 4) print_string(" 1G");
	if (!(advertised & 6)) print_string(" off");
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	print_string((SFR_DATA_U16 & PORT_STATUS_LINK) && (SFR_DATA_U16 & PORT_STATUS_EEE_CAP) ?
		"; negotiated: yes\n" : "; negotiated: no\n");
}

void port_eee_enable_all(__xdata uint8_t speed) __banked
{
	for (uint8_t port = MACHINE_MIN_PORT; port <= MACHINE_MAX_PORT; port++)
		port_eee_enable(port, speed);
}

void port_eee_disable_all(void) __banked
{
	for (uint8_t port = MACHINE_MIN_PORT; port <= MACHINE_MAX_PORT; port++)
		port_eee_disable(port);
}

void port_eee_status_all(void) __banked
{
	for (uint8_t port = MACHINE_MIN_PORT; port <= MACHINE_MAX_PORT; port++)
		port_eee_status(port);
}
