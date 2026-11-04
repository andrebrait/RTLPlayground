/*
 * Register and PHY access for the RTL836x (RTL8370N): 16-bit registers,
 * written through SFR a4:a5 and read back from SFR a6:a7.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#ifndef RTLP_HOST_TEST	/* the host test supplies the SFRs */
#include "rtl837x_sfr.h"
#endif

extern __xdata uint8_t sfr_data[4];

/* The controller can report idle before it has latched a new command, so the
 * vendor loader waits this long before every status poll */
#define SETTLE_READ()	__asm__("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop")
#define SETTLE_WRITE()	__asm__("nop\n\tnop\n\tnop")

/* The value is left in SFR a6 (high byte) and a7 (low byte), i.e. SFR_DATA_U16 */
void reg_read(uint16_t reg_addr)
{
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_EXEC_GO = SFR_EXEC_READ_REG;
	do {
		SETTLE_READ();
	} while (SFR_EXEC_STATUS != 0);
}


void reg_read_m(uint16_t reg_addr)
{
	reg_read(reg_addr);
	sfr_data[0] = 0;
	sfr_data[1] = 0;
	sfr_data[2] = SFR_DATA_8;
	sfr_data[3] = SFR_DATA_0;
}


/* The value must be in SFR a4 (high byte) and a5 (low byte), see REG_SET */
void reg_write(uint16_t reg_addr)
{
	SFR_REG_ADDR_U16 = reg_addr;
	SFR_EXEC_GO = SFR_EXEC_WRITE_REG;
	do {
		SETTLE_WRITE();
	} while (SFR_EXEC_STATUS != 0);
}


/* Writes sfr_data[2] (high byte) and sfr_data[3]; [0] and [1] do not exist on 16-bit registers */
void reg_write_m(uint16_t reg_addr)
{
	SFR_DATA_24 = sfr_data[2];
	SFR_DATA_16 = sfr_data[3];
	SFR_DATA_8 = 0;
	SFR_DATA_0 = 0;
	reg_write(reg_addr);
}


/*
 * PHY register access goes through the OCP window: OCP address bits 15:10 in
 * GPHY_OCP_MSB_0, the rest select the register at 0x2000. A reg below 32 is
 * the MII register at OCP 0xa400 + 2 reg. dev_id is ignored: the internal
 * GPHYs have one OCP address space.
 */
static uint16_t phy_addr(uint8_t phy_id, uint16_t reg)
{
	uint16_t v;

	if (reg < 32)
		reg = RTL836X_OCP_MII + 2 * reg;
	reg_read(RTL836X_REG_GPHY_OCP_MSB_0);
	v = (SFR_DATA_U16 & ~0x003f) | (reg >> 10);
	REG_SET(RTL836X_REG_GPHY_OCP_MSB_0, v);
	return RTL836X_PHY_WINDOW | ((reg >> 6) & 0x0f) << 8 | (uint16_t)phy_id << 5 | ((reg >> 1) & 0x1f);
}


/* The result is in SFR a6:a7 (SFR_DATA_U16), as on the RTL837x */
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
	(void)dev_id;
	reg_read(phy_addr(phy_id, reg));
}


void phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	(void)dev_id;
	reg = phy_addr(phy_id, reg);
	REG_SET(reg, v);
}


void phy_write_mask(uint16_t phy_mask, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	for (uint8_t phy_id = 0; phy_mask; phy_id++, phy_mask >>= 1) {
		if (phy_mask & 1)
			phy_write(phy_id, dev_id, reg, v);
	}
}


void phy_modify(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t mask, uint16_t set)
{
	phy_read(phy_id, dev_id, reg);
	phy_write(phy_id, dev_id, reg, (SFR_DATA_U16 & ~mask) | set);
}
