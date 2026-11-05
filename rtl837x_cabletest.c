#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_cabletest.h"
#include "rtl837x_phy.h"
#include "phy.h"
#include "machine.h"
#include "timer.h"

#pragma codeseg BANK3
#pragma constseg BANK3

#define RTCT_CTRL		0xa422
#define RTCT_CTRL_START		0x00f1
#define RTCT_CTRL_DONE		0x8000
#define RTCT_SRAM_ADDR		0xa436
#define RTCT_SRAM_DATA		0xa438
#if SOC_FAMILY == RTL836X
#define RTCT_SRAM_STATUS	0x802b
#define RTCT_SRAM_LEN		0x802d
#else
#define RTCT_SRAM_STATUS	0x8026
#define RTCT_SRAM_LEN		0x8028
#endif

#define RTCT_ST_BUSY		0x01
#define RTCT_ST_MISMATCH	0x06
#define RTCT_ST_OPEN		0x08
#define RTCT_ST_SHORT		0x10
#define RTCT_ST_OK		0x20
#define RTCT_ST_DONE		0x40
#define RTCT_ST_CROSS_SHORT	0x80

#define RTCT_LEN_ZERO		150
#define RTCT_LEN_PER_50CM	43

#define PHY_POWER		0xa610
#define PHY_POWER_OFF		0x0800

#define BMCR_PDOWN_1G_FULL	0x0940
#define BMCR_1G_FULL		0x0140
#define BMCR_ANRESTART		0x0200

#define CT_IDLE			0
#define CT_SILENCE		1
#define CT_RUN			2
#define CT_POLL_TICKS		(SYS_TICK_HZ / 20)
#define CT_POLLS		100

extern __code const struct machine machine;
extern __xdata struct machine_runtime machine_detected;

__xdata uint8_t ct_state = CT_IDLE;
__xdata uint8_t ct_port;
__xdata struct timer ct_timer;
__xdata uint8_t ct_polls;
__xdata uint8_t ct_valid;
__xdata uint8_t ct_have = 0;
__xdata uint8_t ct_pair;
#if SOC_FAMILY == RTL836X
__xdata uint16_t ct_ctrl;
#else
__xdata uint8_t ct_flags;
__xdata uint16_t ct_bmcr;
#endif
__xdata uint8_t ct_result[4];
__xdata uint16_t ct_len[4];

void rtl_phy_cable_test_start(uint8_t port) __banked
{
#if SOC_FAMILY == RTL836X
	phy_read(port, PHY_MMD31, RTCT_CTRL);
	ct_ctrl = SFR_DATA_U16 & ~RTCT_CTRL_DONE;
	phy_write(port, PHY_MMD31, RTCT_CTRL, ct_ctrl);
	phy_write(port, PHY_MMD31, RTCT_CTRL, ct_ctrl | 0x00f2);
	phy_write(port, PHY_MMD31, RTCT_CTRL, ct_ctrl | 0x00f3);
#else
	phy_write(port, PHY_MMD31, PHY_MMD31_FEDCR, BMCR_1G_FULL);
	phy_write(port, PHY_MMD31, RTCT_CTRL, RTCT_CTRL_START);
#endif
}

bool rtl_phy_cable_test_get_status(uint8_t port) __banked
{
	phy_read(port, PHY_MMD31, RTCT_CTRL);
	if (!(SFR_DATA_U16 & RTCT_CTRL_DONE))
		return false;

	for (ct_pair = 0; ct_pair < 4; ct_pair++) {
		// The flags are the low byte of the word, not the high one.
		phy_write(port, PHY_MMD31, RTCT_SRAM_ADDR, RTCT_SRAM_STATUS + (ct_pair << 2));
		phy_read(port, PHY_MMD31, RTCT_SRAM_DATA);
#if SOC_FAMILY == RTL836X
		/* The 0x6368 SDK compares the complete status word, not a bit mask. */
		ct_ctrl = SFR_DATA_U16; /* SDCC miscompiles switch on a 16-bit SFR. */
		switch (ct_ctrl) {
		case 0x0060: ct_result[ct_pair] = CABLE_RESULT_OK; break;	/* all pairs of a healthy 1G link */
		case 0x0048: ct_result[ct_pair] = CABLE_RESULT_OPEN; break;
		case 0x0050: ct_result[ct_pair] = CABLE_RESULT_SAME_SHORT; break;
		case 0x0042:
		case 0x0044: ct_result[ct_pair] = CABLE_RESULT_MISMATCH; break;
		default: ct_result[ct_pair] = CABLE_RESULT_UNSPEC; break;
		}
#else
		ct_flags = SFR_DATA_U16;
		if (!(ct_flags & RTCT_ST_DONE))
			return false;
		if (ct_flags & RTCT_ST_OK)
			ct_result[ct_pair] = CABLE_RESULT_OK;
		else if (ct_flags & RTCT_ST_OPEN)
			ct_result[ct_pair] = CABLE_RESULT_OPEN;
		else if (ct_flags & RTCT_ST_SHORT)
			ct_result[ct_pair] = CABLE_RESULT_SAME_SHORT;
		else if (ct_flags & RTCT_ST_BUSY)
			ct_result[ct_pair] = CABLE_RESULT_UNSPEC;
		else if (ct_flags & RTCT_ST_CROSS_SHORT)
			ct_result[ct_pair] = CABLE_RESULT_CROSS_SHORT;
		else if (ct_flags & RTCT_ST_MISMATCH)
			ct_result[ct_pair] = CABLE_RESULT_MISMATCH;
		else
			ct_result[ct_pair] = CABLE_RESULT_UNSPEC;
#endif

		phy_write(port, PHY_MMD31, RTCT_SRAM_ADDR, RTCT_SRAM_LEN + (ct_pair << 2));
		phy_read(port, PHY_MMD31, RTCT_SRAM_DATA);
		ct_len[ct_pair] = SFR_DATA_U16;
#if SOC_FAMILY == RTL836X
		/* SDK raw/2 is in 2.5cm units; the CLI stores decimetres. */
		ct_len[ct_pair] /= 8;
#else
		ct_len[ct_pair] = ct_len[ct_pair] > RTCT_LEN_ZERO ? ct_len[ct_pair] - RTCT_LEN_ZERO : 0;
		ct_len[ct_pair] = ct_len[ct_pair] * 5 / RTCT_LEN_PER_50CM;
#endif
	}
	return true;
}

static void cabletest_print(void)
{
	print_string("Cable test port ");
	print_phys_port(ct_port);
	if (!ct_valid) {
		print_string(": timed out\n");
		return;
	}
	write_char('\n');
	for (ct_pair = 0; ct_pair < 4; ct_pair++) {
		print_string("Pair ");
		write_char('A' + ct_pair);
		switch (ct_result[ct_pair]) {
		case CABLE_RESULT_OK:
			print_string(": ok\n");
			continue;
		case CABLE_RESULT_OPEN:
			print_string(": open at ");
			break;
		case CABLE_RESULT_SAME_SHORT:
			print_string(": short at ");
			break;
		case CABLE_RESULT_CROSS_SHORT:
			print_string(": short to another pair at ");
			break;
		case CABLE_RESULT_MISMATCH:
			print_string(": impedance mismatch at ");
			break;
		default:
			print_string(": unknown\n");
			continue;
		}
		itoa_short(ct_len[ct_pair] / 10);
		write_char('.');
		write_char('0' + ct_len[ct_pair] % 10);
		print_string(" m\n");
	}
}

void cabletest_run(void) __banked
{
	if (ct_state) {
		print_string("Cable test running on port ");
		print_phys_port(ct_port);
		write_char('\n');
		return;
	}
#if SOC_FAMILY == RTL836X
	if (phy_settings.port > 7) {
#else
	if (!machine_detected.isRTL8373 && (phy_settings.port < 4 || phy_settings.port > 7)) {
#endif
		print_string("No cable test on this PHY\n");
		return;
	}

	// A test started on a port that is off never ends and blocks the PHY until reset.
#if SOC_FAMILY == RTL836X
	phy_read(phy_settings.port, PHY_MMD31, PHY_MMD31_FEDCR);
#else
	phy_read(phy_settings.port, PHY_MMD31, PHY_POWER);
#endif
	if (SFR_DATA_U16 & PHY_POWER_OFF) {
		print_string("Port is off\n");
		return;
	}

	ct_port = phy_settings.port;
#if SOC_FAMILY == RTL836X
	/* Echo-response mode needs no BMCR override or later admin-state restore. */
	rtl_phy_cable_test_start(ct_port);
	ct_polls = CT_POLLS;
	timer_set(&ct_timer, CT_POLL_TICKS);
	ct_state = CT_RUN;
#else
	phy_read(ct_port, PHY_MMD31, PHY_MMD31_FEDCR);
	ct_bmcr = SFR_DATA_U16;

	// Forcing the speed leaves the link up; only a power-down silences the partner.
	phy_write(ct_port, PHY_MMD31, PHY_MMD31_FEDCR, BMCR_PDOWN_1G_FULL);
	timer_set(&ct_timer, SYS_TICK_HZ);
	ct_state = CT_SILENCE;
#endif
	ct_have = 0;
	print_string("Cable test started\n");
}

void cabletest_show(void) __banked
{
	if (ct_state)
		print_string("Cable test running\n");
	else if (ct_port != phy_settings.port || !ct_have)
		print_string("No cable test result\n");
	else
		cabletest_print();
}

void cabletest_tick(void) __banked
{
#if SOC_FAMILY == RTL837X
	if (ct_state == CT_SILENCE) {
		if (!timer_expired(&ct_timer))
			return;
		phy_read(ct_port, PHY_MMD31, PHY_POWER);
		if (SFR_DATA_U16 & PHY_POWER_OFF) {
			phy_write(ct_port, PHY_MMD31, PHY_MMD31_FEDCR, ct_bmcr);
			ct_state = CT_IDLE;
			print_string("Cable test aborted, port is off\n");
			return;
		}
		rtl_phy_cable_test_start(ct_port);
		ct_polls = CT_POLLS;
		timer_set(&ct_timer, CT_POLL_TICKS);
		ct_state = CT_RUN;
		return;
	}
#endif
	if (ct_state != CT_RUN || !timer_expired(&ct_timer))
		return;
	timer_restart(&ct_timer);

	ct_valid = rtl_phy_cable_test_get_status(ct_port);
	if (!ct_valid && --ct_polls)
		return;

#if SOC_FAMILY == RTL836X
	phy_read(ct_port, PHY_MMD31, RTCT_CTRL);
	phy_write(ct_port, PHY_MMD31, RTCT_CTRL, (SFR_DATA_U16 & ~0x8001) | 0x00f0);
#else
	phy_write(ct_port, PHY_MMD31, RTCT_CTRL, 0);
	phy_write(ct_port, PHY_MMD31, PHY_MMD31_FEDCR, ct_bmcr | BMCR_ANRESTART);
#endif
	ct_have = 1;
	ct_state = CT_IDLE;
	cabletest_print();
}
