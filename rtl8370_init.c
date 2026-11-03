/*
 * Switch initialization for the RTL8370N, as the TP-Link boot code's loader
 * mode does it (Realtek SDK rtk_switch_init() for the RTL8370B).
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_init.h"

#pragma codeseg BANK2
#pragma constseg BANK2

/* After the pragmas, so the tables land in BANK2. Generated from the board's flash dump, see the Makefile */
#include "phy_patch.h"

/* SDK rtl8367c_setAsicRegBits: val is shifted to the mask's lowest bit */
static void reg_bits(uint16_t reg, uint16_t mask, uint16_t val)
{
	uint16_t m = mask;

	while (!(m & 1)) {
		m >>= 1;
		val <<= 1;
	}
	reg_read(reg);
	val = (SFR_DATA_U16 & ~mask) | (val & mask);
	REG_SET(reg, val);
}


/* In the order of the boot code (bank 2 b02_7ffb, 4fd2, b02_9130) and the console image */
void rtl8370_init(void) __banked
{
	uint16_t v;

	REG_SET(RTL836X_REG_MAGIC_ID, RTL836X_MAGIC_ID_VALUE);
	reg_read(RTL836X_REG_CHIP_NUMBER);
	v = SFR_DATA_U16;
	REG_SET(RTL836X_REG_MAGIC_ID, 0);
	print_string("Chip number: ");
	print_short(v);
	write_char('\n');
	if (v != 0x0652 && v != 0x6368) {	/* RTL8370B */
		print_string("INCORRECT MACHINE! Switch init skipped\n");
		return;
	}

	REG_SET(RTL836X_REG_M10_FORCE_SELECT, 0x1076);
	REG_SET(RTL836X_REG_RLDP_CTRL4, 0x07ff);

#if PHY_PATCH_LEN
	/* _rtk_switch_init_8370b: PHYID_CFG0 makes the patch go to all eight PHYs at once */
	reg_read(RTL836X_REG_PHYID_CFG0);
	v = SFR_DATA_U16;
	REG_SET(RTL836X_REG_PHYID_CFG0, (v | 0x3be0) & 0xffe0);
	for (uint8_t i = 0; i < PHY_PATCH_LEN; i++)
		phy_write(0, 0, phy_patch[i][0], phy_patch[i][1]);
	REG_SET(RTL836X_REG_PHYID_CFG0, v);
	for (uint8_t i = 0; i < 8; i++)
		phy_write(i, 0, 0xa4b4, long_link[i]);
#endif

	/* The common part of _rtk_switch_init, minus rate limits, LUT, RMA and mirror settings */
	reg_bits(RTL836X_REG_MAX_LENGTH_LIMINT_IPG, 0x6000, 3);
	reg_bits(RTL836X_REG_MAX_LEN_RX_TX, 0x0003, 3);
	reg_bits(RTL836X_REG_ACL_ACCESS_MODE, 0x0001, 1);
	REG_SET(RTL836X_REG_LINE_RATE_HSG_H, 0x0007);
	reg_bits(RTL836X_REG_PORT_SECURITY_CTRL, 0x00c0, 3);	/* unknown unicast DA: per port */

	reg_read(RTL836X_REG_MISCELLANEOUS_CONFIGURE0);
	v = SFR_DATA_U16 | 0x0050;
	REG_SET(RTL836X_REG_MISCELLANEOUS_CONFIGURE0, v);

	/* rtk_port_phyEnableAll_set(ENABLED): out of power down (BMCR bit 11), restart autonegotiation */
	reg_bits(RTL836X_REG_PHY_AD, PHY_AD_PDNPHY, 0);
	for (uint8_t i = 0; i < 8; i++)
		phy_modify(i, 0, RTL836X_OCP_MII, 0x0800, 0x0200);
}
