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
