/* RTL8370B rate registers, as documented by the RTL8367C SDK rate/storm APIs.
 * Rates use 8-kbit/s tokens. Keep the chip's existing bucket/clock calibration. */
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl836x_rate.h"
#include "rtl837x_bandwidth.h"
#include "rtl837x_storm.h"
#include "rtl837x_trap.h"
#include "cmd_parser.h"
#include "machine.h"

#pragma codeseg BANK2
#pragma constseg BANK2

#define BW_REG(p, in) ((in) ? (RTL836X_REG_INGRESS_RATE0 + ((uint16_t)(p) << 5)) : (RTL836X_REG_EGRESS_RATE0 + ((uint16_t)(p) << 1)))
#define MISC_REG(p) (RTL836X_REG_PORT0_MISC_CFG + ((uint16_t)(p) << 5))
#define METER_REG(i) ((i) < 32 ? RTL836X_REG_METER_RATE0 + ((uint16_t)(i) << 1) : RTL836X_REG_METER_RATE32 + ((uint16_t)((i) - 32) << 1))
#define METER_IFG_REG(i) ((i) < 32 ? RTL836X_REG_METER_IFG0 + ((i) >> 4) : RTL836X_REG_METER_IFG32 + (((i) - 32) >> 4))
#define METER_MODE_REG(i) ((i) < 32 ? RTL836X_REG_METER_MODE0 + ((i) >> 4) : RTL836X_REG_METER_MODE32 + (((i) - 32) >> 4))
#define STORM_CTRL(t) (RTL836X_REG_STORM_CTRL0 + (t))
#define STORM_SELECT(p, t) (RTL836X_REG_STORM_SELECT0 + ((uint16_t)(t) << 3) + ((p) >> 1))
#define RATE_VALID(p, r) (MACHINE_PORT_VALID(p) && (r) >= RTL836X_RATE_MIN_KBPS && (r) <= RTL836X_RATE_MAX_KBPS && !((r) & 7))

static __code const char * __code const storm_names[STORM_TYPES] = { " bcast ", " mcast ", " ucast ", " umcast " };

static void bad_rate(void)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: native rate control needs a front port and 8..1000000 kbit/s in steps of 8; PPS is unsupported\n");
}

static uint32_t read_count(__xdata uint16_t reg)
{
	__xdata uint32_t count;
	reg_read(reg);
	count = SFR_DATA_U16;
	reg_read(reg + 1);
	return count | ((uint32_t)(SFR_DATA_U16 & 7) << 16);
}

static void write_count(__xdata uint16_t reg, __xdata uint32_t count)
{
	REG_SET(reg, count);
	reg_bits(reg + 1, 7, count >> 16);
}

uint32_t rtl836x_bw_get(uint8_t port, __xdata uint8_t ingress) __banked
{
	if (!MACHINE_PORT_VALID(port) || ingress > 1)
		return UINT32_MAX;
	return read_count(BW_REG(port, ingress));
}

void bandwidth_setup(void) __banked
{
	__xdata uint8_t p, t;
	for (p = MACHINE_MIN_PORT; p <= MACHINE_MAX_PORT; p++) {
		write_count(BW_REG(p, 1), RTL836X_RATE_OFF);
		write_count(BW_REG(p, 0), RTL836X_RATE_OFF);
		reg_bit_clear(MISC_REG(p), 11);
	}
	for (t = 0; t < STORM_TYPES; t++)
		reg_bits(STORM_CTRL(t), 0x04ff, 0); /* front ports and chip CPU port 10 */
	reg_bit_set(RTL836X_REG_INGRESS_BYPASS, 2); /* RMA, IGMP and Realtek control frames */
	reg_bit_clear(RTL836X_REG_EGRESS_IFG, 0); /* exclude preamble/IFG */
	/* SDK storm_bypass_set: retain STP, pause, LACP, LLDP and IGMP/MLD under a storm. */
	reg_bit_set(RTL836X_REG_RMA_CTRL00 + TRAP_RMA_STP, 6);
	reg_bit_set(RTL836X_REG_RMA_CTRL00 + 1, 6);
	reg_bit_set(RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP, 6);
	reg_bit_set(RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LLDP, 6);
	reg_bit_set(RTL836X_REG_RMA_CTRL_LLDP, 6);
	reg_bit_set(RTL836X_REG_IGMP_MLD_CFG0, 13);
}

void bandwidth_ingress_set(uint8_t port, __xdata uint32_t bw) __banked
{
	if (!RATE_VALID(port, bw)) { bad_rate(); return; }
	write_count(BW_REG(port, 1), bw >> 3);
	reg_bit_clear(MISC_REG(port), 10); /* exclude preamble/IFG */
	reg_bit_set(MISC_REG(port), 11);
}

void bandwidth_egress_set(uint8_t port, __xdata uint32_t bw) __banked
{
	if (!RATE_VALID(port, bw)) { bad_rate(); return; }
	write_count(BW_REG(port, 0), bw >> 3);
}

void bandwidth_ingress_disable(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port)) { bad_rate(); return; }
	write_count(BW_REG(port, 1), RTL836X_RATE_OFF);
}

void bandwidth_egress_disable(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port)) { bad_rate(); return; }
	write_count(BW_REG(port, 0), RTL836X_RATE_OFF);
}

void bandwidth_ingress_drop(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port)) { bad_rate(); return; }
	reg_bit_clear(MISC_REG(port), 11);
}

void bandwidth_ingress_fc(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port)) { bad_rate(); return; }
	reg_bit_set(MISC_REG(port), 11);
}

void bandwidth_status(uint8_t port) __banked
{
	__xdata uint8_t egress;
	__xdata uint32_t count;
	if (!MACHINE_PORT_VALID(port)) { bad_rate(); return; }
	for (egress = 0; egress < 2; egress++) {
		print_string(egress ? "egress: " : "ingress: ");
		count = rtl836x_bw_get(port, 1 - egress);
		if (count == RTL836X_RATE_OFF)
			print_string("disabled");
		else {
			print_long(count << 3); print_string(" kbps");
		}
		if (!egress)
			print_string(reg_bit_test(MISC_REG(port), 11) ? " fc" : " drop");
		write_char('\n');
	}
}

/* Stack-backed addressing avoids SDCC spilling the validation operand into IRAM. */
static void apply_storm(uint8_t port, uint8_t type, uint32_t count) __reentrant
{
	uint8_t idx = STORM_METER(port, type);
	uint16_t reg = METER_REG(idx);
	write_count(reg, count);
	reg_bit_clear(METER_IFG_REG(idx), idx & 15);
	reg_bit_clear(METER_MODE_REG(idx), idx & 15);
	reg_bits(STORM_SELECT(port, type), port & 1 ? 0x3f00 : 0x003f, idx);
	reg_bit_set(STORM_CTRL(type), port);
}

void storm_set(uint8_t port, __xdata uint8_t type, __xdata uint32_t rate, __xdata uint8_t pps) __banked
{
	if (type >= STORM_TYPES || pps || !RATE_VALID(port, rate)) { bad_rate(); return; }
	apply_storm(port, type, rate >> 3);
}

void storm_off(uint8_t port, __xdata uint8_t type) __banked
{
	if (!MACHINE_PORT_VALID(port) || type >= STORM_TYPES) { bad_rate(); return; }
	reg_bit_clear(STORM_CTRL(type), port);
}

uint32_t rtl836x_storm_get(uint8_t port, __xdata uint8_t type) __banked
{
	__xdata uint8_t idx;
	if (!MACHINE_PORT_VALID(port) || type >= STORM_TYPES)
		return UINT32_MAX;
	reg_read(STORM_SELECT(port, type));
	idx = (SFR_DATA_U16 >> (port & 1 ? 8 : 0)) & 0x3f;
	return read_count(METER_REG(idx)) << 3;
}

void storm_show(void) __banked
{
	__xdata uint8_t p, t;
	for (p = MACHINE_MIN_PORT; p <= MACHINE_MAX_PORT; p++) {
		print_string("port "); print_phys_port(p);
		for (t = 0; t < STORM_TYPES; t++) {
			print_string(storm_names[t]);
			if (!reg_bit_test(STORM_CTRL(t), p)) print_string("off");
			else { print_long(rtl836x_storm_get(p, t)); print_string(" kbps"); }
		}
		write_char('\n');
	}
}
