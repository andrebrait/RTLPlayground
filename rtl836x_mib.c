/* Independent reader for the RTL8367C/RTL8370N four-word MIB latch.
 * Register facts: 0x7c words/port, selector is word address / 4, low word
 * first in 0x1000..3. No counter reset or counting-mode writes. */
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"

#pragma codeseg BANK1
#pragma constseg BANK1

static bool mib_select(uint16_t selector)
{
	uint8_t attempts = 100;
	do {
		REG_SET(RTL836X_REG_MIB_ADDRESS, selector);
		reg_read(RTL836X_REG_MIB_ADDRESS);
		if (SFR_DATA_U16 == selector)
			break;
	} while (--attempts);
	if (!attempts)
		return false;
	attempts = 100;
	do {
		reg_read(RTL836X_REG_MIB_CTRL);
		if (SFR_DATA_0 & RTL836X_MIB_RESET)
			return false;
		if (!(SFR_DATA_0 & RTL836X_MIB_BUSY))
			return true;
	} while (--attempts);
	return false;
}

bool rtl836x_mib_read(uint8_t port, __xdata uint8_t counter, __xdata uint8_t * __xdata value) __banked
{
	if (port >= 8 || counter >= RTL836X_MIB_COUNT) {
		return false;
	}
	__xdata uint16_t offset = (uint16_t)port * 0x7c + (uint16_t)counter * 2;
	if (counter > 0) offset += 2;
	if (counter > 19) offset += 2;
	if (counter > 28) offset += 2;
	__xdata uint8_t words = counter == 0 || counter == 19 || counter == 28 ? 4 : 2;
	uint16_t selector = offset >> 2;
	reg_read(RTL836X_REG_MIB_ADDRESS);
	/* Rewriting an unchanged selector does not refresh the latch. */
	if (SFR_DATA_U16 == selector && !mib_select(selector + 1))
		return false;
	if (!mib_select(selector))
		return false;
	for (uint8_t i = 0; i < 8; i++)
		value[i] = 0;
	for (uint8_t i = 0; i < words; i++) {
		reg_read(RTL836X_REG_MIB_COUNTER + (offset & 3) + i);
		value[7 - 2 * i] = SFR_DATA_0;
		value[6 - 2 * i] = SFR_DATA_8;
	}
	return true;
}

bool rtl836x_mib_summary(__xdata uint8_t port, __xdata uint8_t metric, __xdata uint8_t * __xdata value) __banked
{
	/* 4 and 5 are RX and TX errors, summed as the Linux rtl8365mb driver does:
	 * FCS + fragments + jabbers, and out discards + late collisions */
	static __code const uint8_t counters[6][3] = {
		{38, 39, 40}, {34, 0, 0}, {7, 9, 10}, {1, 0, 0}, {1, 5, 6}, {36, 32, 0}
	};
	__xdata uint8_t next[8];
	if (metric >= 6 || !rtl836x_mib_read(port, counters[metric][0], value))
		return false;
	for (__xdata uint8_t i = 1; i < 3 && counters[metric][i]; i++) {
		if (!rtl836x_mib_read(port, counters[metric][i], next))
			return false;
		uint16_t carry = 0;
		for (__xdata uint8_t b = 8; b; ) {
			b--;
			carry += (uint16_t)value[b] + next[b];
			value[b] = carry;
			carry >>= 8;
		}
	}
	return true;
}
