/*
 * Small hardware accessors of the HOME bank. They live apart from rtlplayground.c
 * (which cannot be built on the host: serial ISR, timers) so the host tests compile
 * the real definitions and mock only the register and GPIO reads below them.
 */
#include <8051.h>
#include <stdint.h>
#include <stdbool.h>

#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_flash.h"
#include "rtl837x_common.h"
#include "machine.h"
#include "platform.h"

extern __xdata uint8_t sfr_data[4];
extern __xdata struct flash_region_t flash_region;

/*
 * Copies the 4 sfr data bytes to dst
 */
void sfr_data_copy(__xdata uint8_t *dst)
{
	uint8_t idx = 4;
	while (idx) {
		idx -= 1;
		dst[idx] = sfr_data[idx];
	}
}


int16_t chip_temp_get(uint8_t sensor) __banked
{
	reg_read(sensor ? RTL837X_TM_RESULT_POWERON : RTL837X_TM_RESULT);
	return SFR_DATA_U16;
}


bool sfp_present_live(uint8_t slot)
{
	return !gpio_pin_test(machine.sfp_port[slot].pin_detect);
}


// Where the web server continues streaming after the packet it is building (httpd.c)
extern __xdata uint16_t cont_len;
extern __xdata uint32_t cont_addr;

uint16_t config_read(uint16_t offset, __xdata uint8_t * __xdata buf, __xdata uint16_t len) __banked
{
	__xdata uint16_t room;

	if (offset >= CONFIG_LEN)
		return 0;
	room = CONFIG_LEN - offset;
	if (len > room)
		len = room;
	flash_region.addr = CONFIG_START + offset;
	flash_region.len = len;
	flash_read_bulk(buf);
	return len;
}


void config_stream_rest(uint16_t offset, __xdata uint16_t len) __banked
{
	cont_addr = CONFIG_START + offset;
	cont_len = len;
}
