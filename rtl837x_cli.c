/*
 * RTL837x debug commands of the console: raw register, SerDes and PHY access,
 * flash IDs, GPIO and LED dumps, the random number generator, the chip reset
 * and (HEALTH=1) the "health" dump.
 *
 * cmd_parser.c hands every word it does not know to platform_cmd(); it is
 * bound at link time, so a platform without these registers provides its own.
 */

#include <8051.h>
#include <stdint.h>
#include <stdbool.h>
#include "cmd_parser.h"
#include "rtl837x_cli.h"
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_stp.h"
#include "rtl837x_leds.h"
#include "uip/uip.h"
#include "machine.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern volatile __xdata uint32_t ticks;
extern volatile __xdata uint8_t sfr_data[4];
extern __xdata struct flash_region_t flash_region;
extern __xdata uint16_t management_vlan;

__xdata uint8_t gpio_last_value[8] = { 0 };


/* The 1 or 2 bytes atoi_hex() left in hexvalue[] (hex_size of them) as one number */
static uint16_t hexvalue_u16(uint8_t hex_size)
{
	uint16_t v = hexvalue[0];
	if (hex_size == 2) {
		v <<= 8;
		v |= hexvalue[1];
	}
	return v;
}


static void parse_regget(void)
{
	if (cmd_words_len != 2) {
		goto err;
	}

	uint8_t hex_size = atoi_hex(cmd_words_b[1]);

	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}

	uint16_t reg = hexvalue_u16(hex_size);

	print_string("REGGET: ");
	print_short(reg);
	print_string(": VAL: ");

	reg_read_m(reg);
	print_sfr_data();
	write_char('\n');
	return;

err:
	cmd_error("regget <hexvalue>\n\tlike: regget 0BB0 or regget 0c");
	return;
}


static void parse_regset(void)
{
	if (cmd_words_len != 3) {
		goto err;
	}

	uint8_t hex_size = atoi_hex(cmd_words_b[1]);
	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}

	uint16_t reg = hexvalue_u16(hex_size);

	hex_size = atoi_hex(cmd_words_b[2]);
	if (hex_size == 0 || hex_size > 4) {
		goto err;
	}

	// zero sfr memory data
	sfr_set_zero();

	// copy data over sfr memory
	uint8_t offset = 4 - hex_size;
	while(hex_size) {
		hex_size -= 1;
		sfr_data[offset + hex_size] = hexvalue[hex_size];
	}
	print_string("REGSET: ");
	print_short(reg);

	reg_write_m(reg);

	print_string(": VAL: ");
	print_sfr_data();
	write_char('\n');
	return;

err:
	cmd_error("regset <hexvalue> <hexvalue>\n\tlike regset 0b abcd1234.");
}


static void parse_sdsget(void)
{
	__xdata uint8_t sds_id, page, reg, hex_size;

	if (cmd_words_len != 4) {
		goto err;
	}

	if (!atoi_byte(cmd_words_b[1])) {
		goto err;
	}
	sds_id = atoi_results_u8;

	hex_size = atoi_hex(cmd_words_b[2]);
	if (hex_size != 1) {
		goto err;
	}
	page = hexvalue[0];

	hex_size = atoi_hex(cmd_words_b[3]);
	if (hex_size != 1) {
		goto err;
	}
	reg = hexvalue[0];

	print_string("SDSGET: ");
	itoa(sds_id);
	print_string(":0x");
	print_byte(page);
	print_string(":0x");
	print_byte(reg);
	print_string(": VAL: ");

	sds_read(sds_id, page, reg);
	print_phy_data();
	write_char('\n');
	return;

err:
	cmd_error("sdsget <sds-id> <hex:page> <hex:reg>\n");
	return;
}


static void parse_sdsset(void)
{
	__xdata uint8_t sds_id, page, reg, hex_size;
	__xdata uint16_t val;

	if (cmd_words_len != 5) {
		goto err;
	}

	if (!atoi_byte(cmd_words_b[1])) {
		goto err;
	}
	sds_id = atoi_results_u8;

	hex_size = atoi_hex(cmd_words_b[2]);
	if (hex_size != 1) {
		goto err;
	}
	page = hexvalue[0];

	hex_size = atoi_hex(cmd_words_b[3]);
	if (hex_size != 1) {
		goto err;
	}
	reg = hexvalue[0];

	hex_size = atoi_hex(cmd_words_b[4]);
	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}
	val = hexvalue_u16(hex_size);

	print_string("SDSSET: ");
	itoa(sds_id);
	print_string(":0x");
	print_byte(page);
	print_string(":0x");
	print_byte(reg);
	print_string(": VAL: ");

	sds_write_v(sds_id, page, reg, val);

	print_short(val);
	write_char('\n');
	return;

err:
	cmd_error("sdsset <sds-id> <hex:page> <hex:reg> <hex:val>\n");
	return;
}


static void parse_phyget(void)
{
	__xdata uint8_t phy_id, dev_id, hex_size;
	__xdata uint16_t reg;

	if (cmd_words_len != 4) {
		goto err;
	}

	if (!atoi_byte(cmd_words_b[1])) {
		goto err;
	}
	phy_id = atoi_results_u8;


	if (!atoi_byte(cmd_words_b[2])) {
		goto err;
	}
	dev_id = atoi_results_u8;

	hex_size = atoi_hex(cmd_words_b[3]);
	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}
	reg = hexvalue_u16(hex_size);

	print_string("PHYGET: ");
	itoa(phy_id);
	print_string(":");
	itoa(dev_id);
	print_string(".");
	print_short(reg);
	print_string(": VAL: ");

	phy_read(phy_id, dev_id, reg);
	print_phy_data();
	write_char('\n');
	return;

err:
	cmd_error("phyget <phy-id> <dev-id> <hex:reg>\n");
	return;
}


static void parse_physet(void)
{
	__xdata uint8_t phy_id, dev_id, hex_size;
	__xdata uint16_t reg, val;

	if (cmd_words_len != 5) {
		goto err;
	}

	if (!atoi_byte(cmd_words_b[1])) {
		goto err;
	}
	phy_id = atoi_results_u8;


	if (!atoi_byte(cmd_words_b[2])) {
		goto err;
	}
	dev_id = atoi_results_u8;

	hex_size = atoi_hex(cmd_words_b[3]);
	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}
	reg = hexvalue_u16(hex_size);

	hex_size = atoi_hex(cmd_words_b[4]);
	if (hex_size == 0 || hex_size > 2) {
		goto err;
	}
	val = hexvalue_u16(hex_size);

	print_string("PHYSET: ");
	itoa(phy_id);
	print_string(":");
	itoa(dev_id);
	print_string(".");
	print_short(reg);
	print_string(": VAL: ");

	phy_write(phy_id, dev_id, reg, val);

	print_short(val);
	write_char('\n');
	return;

err:
	cmd_error("physet <phy-id> <dev-id> <hex:reg> <hex:val>\n");
	return;
}


static void parse_rnd(void)
{
	// In order to get a new random numner, this bit has to be set each time!
	reg_bit_set(RTL837X_RLDP_RLPP, RLDP_RND_EN);
	reg_read_m(RTL837X_RAND_NUM1);
	print_byte(sfr_data[2]);
	print_byte(sfr_data[3]);
	reg_read_m(RTL837X_RAND_NUM0);
	print_byte(sfr_data[0]);
	print_byte(sfr_data[1]);
	print_byte(sfr_data[2]);
	print_byte(sfr_data[3]);
	write_char('\n');
}


// Print GPIO status
static void print_gpio_status(void)
{
	for (uint8_t idx = 0; idx < 2; idx++) {
		reg_read(RTL837X_REG_GPIO_00_31_INPUT + (idx * 4));
		print_string("GPIO ");
		write_char(idx + '0');
		write_char(':');
		write_char(' ');

		print_byte(SFR_DATA_24);
		print_byte(SFR_DATA_16);
		print_byte(SFR_DATA_8);
		print_byte(SFR_DATA_0);

		write_char(' ');
		print_byte( gpio_last_value[(idx *4)] ^ SFR_DATA_24);
		gpio_last_value[(idx *4)] = SFR_DATA_24;
		print_byte( gpio_last_value[(idx *4) + 1] ^ SFR_DATA_16);
		gpio_last_value[(idx *4) + 1] = SFR_DATA_16;
		print_byte( gpio_last_value[(idx *4) + 2] ^ SFR_DATA_8);
		gpio_last_value[(idx *4) + 2] = SFR_DATA_8;
		print_byte( gpio_last_value[(idx *4) + 3] ^ SFR_DATA_0);
		gpio_last_value[(idx *4) + 3] = SFR_DATA_0;
		write_char('\n');
	}
}


#ifdef HEALTH
/*
 * Health instrumentation. The counters live in xdata and are fed from the
 * main loop through the banked entry points below; the "health" command
 * prints everything in one go. All figures are raw hex, diffing two dumps
 * gives the rates. Phase times are in system ticks (SYS_TICK_HZ = 200,
 * 5 ms each); a phase counts as slow when it spans 2 or more ticks.
 */
extern __xdata uint16_t len_left;	/* httpd: bytes still to send of the current file */
extern __xdata uint8_t entry;		/* httpd: index of the file being sent */

static __xdata uint32_t health_loops;
__xdata uint16_t health_rx_frames;

static __xdata uint16_t ph_last_t;
static __xdata uint8_t  ph_max[HEALTH_PHASES];
static __xdata uint16_t ph_slow[HEALTH_PHASES];

/* The ISR increments ticks between the two byte reads of a 16-bit load, so
 * read until two loads agree. */
static uint16_t ticks_lo(void)
{
	uint16_t a, b;
	a = (uint16_t)ticks;
	do {
		b = a;
		a = (uint16_t)ticks;
	} while (a != b);
	return a;
}


void health_loop_start(void) __banked
{
	health_loops++;
	ph_last_t = ticks_lo();
}


void health_phase(uint8_t id) __banked
{
	__xdata uint16_t now = ticks_lo();
	__xdata uint16_t d = now - ph_last_t;
	ph_last_t = now;
	if (d > 0xff)
		d = 0xff;
	if ((uint8_t)d > ph_max[id])
		ph_max[id] = d;
	if (d >= 2)
		ph_slow[id]++;
}


/* Fill the stack area above the current frame with a pattern; the dump scans
 * for how much of it survived. The 8051 stack grows upward, so everything
 * above SP is free at this point. Interrupts are held off so an ISR does not
 * push into the area mid-paint. */
void health_stack_paint(void) __banked
{
	__xdata uint8_t from = SP + 4;
	__xdata uint8_t ea = EA;
	EA = 0;
	for (uint8_t i = 0xff; i >= from; i--)
		*(__idata uint8_t *)i = 0xa5;
	EA = ea;
}


static void ph_line(__code const char *name, uint8_t id)
{
	print_string(name);
	print_string(" max ");
	print_byte(ph_max[id]);
	print_string(" slow ");
	print_short(ph_slow[id]);
	write_char('\n');
}


static void health_show(void)
{
	__xdata uint8_t i;

	print_string("up ");
	reg_read_m(RTL837X_REG_SEC_COUNTER);
	print_byte(sfr_data[0]); print_byte(sfr_data[1]);
	print_byte(sfr_data[2]); print_byte(sfr_data[3]);
	print_string(" ticks ");
	print_long(ticks);
	print_string(" loops ");
	print_long(health_loops);
	print_string(" rx ");
	print_short(health_rx_frames);
	write_char('\n');

	/* Phase timing: max duration seen (ticks of 5 ms) and slow-pass count */
	ph_line("link", HEALTH_PH_LINK);
	ph_line("sfp ", HEALTH_PH_SFP);
	ph_line("rx  ", HEALTH_PH_RX);
	ph_line("tx  ", HEALTH_PH_TX);
	ph_line("stp ", HEALTH_PH_STP);
	ph_line("cmd ", HEALTH_PH_CMD);

	/* Stack: current depth plus how much of the painted area is untouched */
	print_string("sp ");
	print_byte(SP);
	print_string(" untouched ");
	i = 0;
	while (*(__idata uint8_t *)(0xff - i) == 0xa5 && i < 0x86)
		i++;
	print_byte(i);
	write_char('\n');

	/* The TCP connection slot. One on this configuration; a build with more
	 * has to say which slot each line is, so make that a visible decision. */
#if UIP_CONNS != 1
#error "health: the tcp dump prints a single slot, extend it for UIP_CONNS > 1"
#endif
	i = 0;
	{
		print_string("tcp ");
		print_string("st ");
		print_byte(uip_conns[i].tcpstateflags);
		print_string(" tmr ");
		print_byte(uip_conns[i].timer);
		print_string(" rtx ");
		print_byte(uip_conns[i].nrtx);
		print_string(" rto ");
		print_byte(uip_conns[i].rto);
		print_string(" len ");
		print_short(uip_conns[i].len);
		print_string(" mss ");
		print_short(uip_conns[i].mss);
		write_char('\n');
		print_string("  lport ");
		print_short(HTONS(uip_conns[i].lport));
		print_string(" rport ");
		print_short(HTONS(uip_conns[i].rport));
		print_string(" rip ");
		print_short(HTONS(uip_conns[i].ripaddr[0]));
		write_char(' ');
		print_short(HTONS(uip_conns[i].ripaddr[1]));
		write_char('\n');
	}

	/* httpd transfer state and the protocol flags */
	print_string("httpd left ");
	print_short(len_left);
	print_string(" entry ");
	print_byte(entry);
	print_string(" stp ");
	print_byte(stp_enabled);
	print_string(" mvlan ");
	print_short(management_vlan);
	write_char('\n');
}
#endif


/* Returns true when the line in cmd_buffer was one of this platform's commands. */
bool platform_cmd(void) __banked
{
	if (cmd_compare(0, "reset")) {
		print_string("\nRESET\n\n");
		reset_chip();
	} else if (cmd_compare(0, "flash") && cmd_words_len == 2) {
		uint8_t c = cmd_buffer[cmd_words_b[1]];
		if (c == 's') {
			print_string("\nSECURITY REGISTERS\n");
			// The following will only show something else than 0xff if it was programmed for a managed switch
			print_string("Region 1: ");
			flash_region.addr = 0x0001000;
			flash_region.len = 40;
			flash_read_security();
			print_string("\nRegion 2: ");
			flash_region.addr = 0x0002000;
			flash_region.len = 40;
			flash_read_security();
			print_string("\nRegion 3: ");
			flash_region.addr = 0x0003000;
			flash_region.len = 40;
			flash_read_security();
		} else if (c == 'j') {
			print_string("\nJEDEC ID\n");
			flash_read_jedecid();
		} else if (c == 'u') {
			print_string("\nUNIQUE ID (note: only 4 bytes are likely correct here!)\n");
			flash_read_uid();
		}
	} else if (cmd_compare(0, "sds")) {
		print_reg(RTL837X_REG_SDS_MODES);
		write_char('\n');
	} else if (cmd_compare(0, "gpio")) {
		print_gpio_status();
	} else if (cmd_compare(0, "leds")) {
		leds_dump();
	} else if (cmd_compare(0, "regget")) {
		parse_regget();
	} else if (cmd_compare(0, "regset")) {
		parse_regset();
	} else if (cmd_compare(0, "sdsget")) {
		parse_sdsget();
	} else if (cmd_compare(0, "sdsset")) {
		parse_sdsset();
	} else if (cmd_compare(0, "phyget")) {
		parse_phyget();
	} else if (cmd_compare(0, "physet")) {
		parse_physet();
	} else if (cmd_compare(0, "rnd")) {
		parse_rnd();
#ifdef HEALTH
	} else if (cmd_compare(0, "health")) {
		health_show();
#endif
	} else {
		return false;
	}
	return true;
}
