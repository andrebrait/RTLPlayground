#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_storm.h"
#include "machine.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];

/* RTL837X_STORM_MIDX words, row type * 2 + half: half 0 holds ports 0-4, half 1 ports 5-9,
 * 6 bits per port, each set to STORM_METER(port, type) */
static const __code uint8_t storm_midx[8][4] = {
	{ 0x28, 0x92, 0x07, 0x18 }, { 0x3c, 0xe3, 0x4c, 0x2c },
	{ 0x29, 0x96, 0x17, 0x59 }, { 0x3d, 0xe7, 0x5c, 0x6d },
	{ 0x2a, 0x9a, 0x27, 0x9a }, { 0x3e, 0xeb, 0x6c, 0xae },
	{ 0x2b, 0x9e, 0x37, 0xdb }, { 0x3f, 0xef, 0x7c, 0xef },
};

static __code const char * __code const storm_names[STORM_TYPES] = { " bcast ", " mcast ", " ucast ", " umcast " };


void storm_set(uint8_t port, __xdata uint8_t type, __xdata uint32_t rate, __xdata uint8_t pps) __banked
{
	__xdata uint8_t idx = STORM_METER(port, type);
	__xdata uint8_t row = (type << 1) | (port >= 5 ? 1 : 0);
	__xdata uint8_t * __xdata r = (uint8_t *)&rate;

	sfr_data[0] = 0;
	sfr_data[1] = r[2];
	sfr_data[2] = r[1];
	sfr_data[3] = r[0];
	reg_write_m(RTL837X_METER_RATE + (idx << 2));

	if (!pps) {
		sfr_data[1] = 0;
		sfr_data[2] = 0x20;
		sfr_data[3] = 0;
	}
	reg_write_m(RTL837X_METER_BURST + (idx << 2));

	if (pps)
		reg_bit_set(RTL837X_METER_MODE + ((idx >> 5) << 2), idx & 0x1f);
	else
		reg_bit_clear(RTL837X_METER_MODE + ((idx >> 5) << 2), idx & 0x1f);

	sfr_data[0] = storm_midx[row][0];
	sfr_data[1] = storm_midx[row][1];
	sfr_data[2] = storm_midx[row][2];
	sfr_data[3] = storm_midx[row][3];
	reg_write_m(RTL837X_STORM_MIDX + (row << 2));

	reg_bit_set(RTL837X_STORM_CTRL + (type << 2), port);
}


void storm_off(uint8_t port, __xdata uint8_t type) __banked
{
	reg_bit_clear(RTL837X_STORM_CTRL + (type << 2), port);
}


void storm_get(uint8_t port) __banked
{
	uint8_t t, idx, b;

	for (t = 0; t < STORM_TYPES; t++) {
		idx = STORM_METER(port, t);
		b = reg_bit_test(RTL837X_STORM_CTRL + (t << 2), port);
		port_buf[t] = b;
		b = reg_bit_test(RTL837X_METER_MODE + ((idx >> 5) << 2), idx & 0x1f);
		port_buf[4 + t] = b;
		reg_read_m(RTL837X_METER_RATE + (idx << 2));
		sfr_data_copy(port_buf + 8 + (t << 2));
	}
}


void storm_show(void) __banked
{
	uint8_t i, t;

	for (i = machine.min_port; i <= machine.max_port; i++) {
		print_string("port ");
		write_char('0' + machine.log_to_phys_port[i]);
		storm_get(i);
		for (t = 0; t < STORM_TYPES; t++) {
			print_string(storm_names[t]);
			if (!port_buf[t]) {
				print_string("off");
				continue;
			}
			print_byte(port_buf[9 + (t << 2)]); print_byte(port_buf[10 + (t << 2)]); print_byte(port_buf[11 + (t << 2)]);
			if (port_buf[4 + t])
				print_string(" pps");
			else
				print_string(" kbps");
		}
		write_char('\n');
	}
}
