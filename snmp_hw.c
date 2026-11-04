#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#if SOC_FAMILY == RTL836X
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"
#endif
#include "machine.h"
#include "snmp_hw.h"

/* BANK4, beside its callers snmp.c and metrics.c. */
#pragma codeseg BANK4
#pragma constseg BANK4

extern __code const struct machine machine;

#if SOC_FAMILY == RTL836X
/* The RTL836x has no STAT_GET engine to recover from: the native MIB latch,
 * PORTn_STATUS and the Clause 22 PHY registers are read directly. */
void snmp_hw_init(void) __banked
{
}

#define NONE 0xff
/* RTL837x selector to the RTL836x counter with the same meaning: [0] is the
 * 64-bit counter or the high word, [1] the low word. Native counters that sum
 * RX and TX (19..27) have no RTL837x equivalent and are not mapped. */
static __code const uint8_t native[21][2] = {
	{0, NONE}, {28, NONE}, {7, NONE}, {9, NONE}, {10, NONE},
	{38, NONE}, {39, NONE}, {40, NONE},
	{36, 37},	/* out discards, 802.1d Tp port in discards */
	{29, 30},	/* single, multi collision frames */
	{31, 32},	/* deferred transmissions, late collisions */
	{34, 2},	/* excessive collisions, symbol errors */
	{4, 3},		/* control in unknown opcodes, in pause frames */
	{35, 8},	/* out pause frames, drop events */
	{NONE, NONE},	/* TX broadcast, TX multicast */
	{NONE, 1},	/* RX CRC align errors: dot3StatsFCSErrors */
	{NONE, NONE}, {NONE, NONE},
	{NONE, 5},	/* RX fragments */
	{NONE, 6},	/* RX jabbers */
	{33, NONE},	/* TX collisions */
};

bool snmp_hw_stat(uint8_t port, __xdata uint8_t selector, __xdata bool high, __xdata bool low,
		  __xdata struct snmp_counter *__xdata value) __banked
{
	__xdata uint8_t id;

	if (!MACHINE_PORT_VALID(port) || selector >= sizeof(native) / sizeof(native[0]) || !(high || low))
		return false;
	if (high && low)
		id = selector <= 7 ? native[selector][0] : NONE;
	else
		id = native[selector][high ? 0 : 1];
	/* rtl836x_mib_read() fails before it touches the value, and zero-extends 32-bit counters */
	return id != NONE && rtl836x_mib_read(port, id, value->bytes);
}

bool snmp_hw_counter(uint8_t port, __xdata uint8_t selector,
		     __xdata struct snmp_counter *__xdata value) __banked
{
	return selector <= 7 && snmp_hw_stat(port, selector, true, true, value);
}

/* Selector 48: high is the RX error sum, else the TX error sum (rtl836x_mib_summary 4/5),
 * reduced to the 32-bit word the RTL837x reports */
bool snmp_hw_counter32(uint8_t port, __xdata uint8_t selector, __xdata bool high,
		       __xdata struct snmp_counter *__xdata value) __banked
{
	__xdata struct snmp_counter sum;
	__xdata uint8_t i;

	if (selector != 48 || !MACHINE_PORT_VALID(port) || !rtl836x_mib_summary(port, high ? 4 : 5, sum.bytes))
		return false;
	for (i = 0; i < 8; i++)
		value->bytes[i] = i < 4 ? 0 : sum.bytes[i];
	return true;
}

bool snmp_hw_admin(uint8_t port, __xdata uint8_t *__xdata state) __banked
{
	if (!MACHINE_PORT_VALID(port))
		return false;
	phy_read(port, 0, 0);	/* MII BMCR bit 11: power down */
	*state = (SFR_DATA_U16 & 0x0800) ? 2 : 1;
	return true;
}

/* Both callers test only the requested port's bit, so only that bit is read */
bool snmp_hw_link(uint8_t port, __xdata uint32_t *__xdata value) __banked
{
	if (!MACHINE_PORT_VALID(port))
		return false;
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	*value = (SFR_DATA_U16 & PORT_STATUS_LINK) ? (uint16_t)1 << port : 0;
	return true;
}

bool snmp_hw_speed(uint8_t port, __xdata uint16_t *__xdata mbps) __banked
{
	static __code const uint16_t speeds[4] = {10, 100, 1000, 0};

	if (!MACHINE_PORT_VALID(port))
		return false;
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	*mbps = (SFR_DATA_U16 & PORT_STATUS_LINK) ? speeds[SFR_DATA_U16 & PORT_STATUS_SPEED_MASK] : 0;
	return true;
}
#else
/* Set when a statistics request timed out: its selector may still be pending.
 * Cleared only after the SFR engine and STAT_GET are observed idle. */
static __xdata uint8_t fault;

void snmp_hw_init(void) __banked
{
	/* Establish the recovery state explicitly, independent of the CRT. */
	fault = 0;
}

/* The host edge models SFR command completion, including a stuck engine. */
#ifdef RTLP_HOST_TEST
extern void snmp_hw_command(uint16_t reg, uint8_t command);
#endif
static bool access(uint16_t reg, uint8_t command)
{
	/* An external MDIO transaction takes longer than an ASIC register access. */
	__xdata uint16_t budget = command == SFR_EXEC_READ_SMI ? 4096 : 64;
	/* Never replace a command still in flight. */
	if (fault || SFR_EXEC_STATUS)
		return false;
	SFR_REG_ADDR_U16 = reg;
	SFR_EXEC_GO = command;
#ifdef RTLP_HOST_TEST
	snmp_hw_command(reg, command);
#endif
	do {
		if (!SFR_EXEC_STATUS)
			return true;
	} while (--budget);
	return false;
}

static void copy_word(__xdata uint8_t *__xdata value)
{
	value[0] = SFR_DATA_24;
	value[1] = SFR_DATA_16;
	value[2] = SFR_DATA_8;
	value[3] = SFR_DATA_0;
}

static bool user_port(uint8_t port)
{
	return MACHINE_PORT_VALID(port);
}

/* A statistics request may start only when no command is in flight and, after
 * a timeout, STAT_GET proves its selector finished. Read-only probe, no reset. */
static bool stat_ready(void)
{
	if (SFR_EXEC_STATUS) {
		fault = 1;
		return false;
	}
	if (fault) {
		fault = 0;
		/* STAT_GET bit 0 is the selector's busy bit. */
		fault = !access(RTL837X_STAT_GET, SFR_EXEC_READ_REG) || (SFR_DATA_0 & 1);
	}
	return !fault;
}

/* Fill tmp[4..7] (and tmp[0..3] when both halves are read) from one snapshot. */
static bool stat_words(uint8_t port, __xdata uint8_t selector, __xdata bool high,
		       __xdata bool low, __xdata uint8_t *__xdata tmp)
{
	uint8_t budget = 32;
	SFR_DATA_24 = 0;
	SFR_DATA_16 = 0;
	SFR_DATA_8 = selector >> 3;
	SFR_DATA_0 = (selector << 5) | (port << 1) | 1;
	if (!access(RTL837X_STAT_GET, SFR_EXEC_WRITE_REG))
		return false;
	do {
		if (!access(RTL837X_STAT_GET, SFR_EXEC_READ_REG))
			return false;
		if (!(SFR_DATA_0 & 1)) {
			if (high) {
				if (!access(RTL837X_STAT_V_HIGH, SFR_EXEC_READ_REG))
					return false;
				copy_word(low ? tmp : tmp + 4);
			}
			if (low) {
				if (!access(RTL837X_STAT_V_LOW, SFR_EXEC_READ_REG))
					return false;
				copy_word(tmp + 4);
			}
			return true;
		}
	} while (--budget);
	return false;
}

/* The caller's counter changes only after the whole snapshot was read. */
static bool stat_get(uint8_t port, __xdata uint8_t selector, __xdata bool high,
		     __xdata bool low, __xdata struct snmp_counter *__xdata value)
{
	__xdata uint8_t tmp[8];
	uint8_t i;
	if (!user_port(port) || !stat_ready())
		return false;
	for (i = 0; i < 4; i++)
		tmp[i] = 0;
	if (!stat_words(port, selector, high, low, tmp)) {
		fault = 1;
		return false;
	}
	for (i = 0; i < 8; i++)
		value->bytes[i] = tmp[i];
	return true;
}

bool snmp_hw_counter(uint8_t port, __xdata uint8_t selector,
		     __xdata struct snmp_counter *__xdata value) __banked
{
	/* Only the eight native 64-bit interface counters are defined here. */
	return selector <= 7 && stat_get(port, selector, true, true, value);
}

bool snmp_hw_counter32(uint8_t port, __xdata uint8_t selector, __xdata bool high,
		       __xdata struct snmp_counter *__xdata value) __banked
{
	/* Selector 48 is the RX/TX error pair; a 32-bit word is zero-extended. */
	return selector == 48 && stat_get(port, selector, high, !high, value);
}

bool snmp_hw_stat(uint8_t port, __xdata uint8_t selector, __xdata bool high, __xdata bool low,
		  __xdata struct snmp_counter *__xdata value) __banked
{
	return selector < 52 && (high || low) && stat_get(port, selector, high, low, value);
}

/* Copper admin state is PHY MMD31.a610 bit 11 (power down); SFP has no switch. */
bool snmp_hw_admin(uint8_t port, __xdata uint8_t *__xdata state) __banked
{
	if (!user_port(port))
		return false;
	if (machine.is_sfp[port]) {
		*state = 1;
		return true;
	}
	/* SMI setup overwrites SFR data, so check idle before touching anything. */
	if (!stat_ready())
		return false;
	SFR_SMI_REG_U16 = 0xa610;
	SFR_SMI_PHY = port;
	SFR_SMI_DEV = (uint8_t)((31U << 3) | 2U);
	if (!access(0xa610, SFR_EXEC_READ_SMI))
		return false;
	*state = (SFR_DATA_8 & 0x08) ? 2 : 1;
	return true;
}

bool snmp_hw_link(uint8_t port, __xdata uint32_t *__xdata value) __banked
{
	if (!user_port(port) || !stat_ready() ||
	    !access(RTL837X_REG_LINKS_STS, SFR_EXEC_READ_REG))
		return false;
	/* GUI: sfr_data[(port / 8) + 1], i.e. A5 for MAC0..7, A6 for MAC8.
	 * A4/A7 are not the user-port bitmap; do not expose their bits as link. */
	*value = SFR_DATA_16 | ((uint16_t)SFR_DATA_8 << 8);
	return true;
}

/* Return zero when no negotiated speed is available; never infer a capability. */
bool snmp_hw_speed(uint8_t port, __xdata uint16_t *__xdata mbps) __banked
{
	__xdata uint32_t links;
	__xdata uint8_t code;
	if (!snmp_hw_link(port, &links))
		return false;
	*mbps = 0;
	/* snmp_hw_link() has already validated the MAC port range 0..8. */
	if (!((uint16_t)links & ((uint16_t)1 << port))) /* 16 bits cover MACs 0..8, 32-bit temporaries would use scarce DATA */
		return true;
	if (!access(port < 8 ? RTL837X_REG_LINKS : RTL837X_REG_LINKS_89, SFR_EXEC_READ_REG))
		return false;
	switch ((port & 7) >> 1) {
	case 0:
		code = SFR_DATA_0;
		break;
	case 1:
		code = SFR_DATA_8;
		break;
	case 2:
		code = SFR_DATA_16;
		break;
	default:
		code = SFR_DATA_24;
		break;
	}
	code = (port & 1) ? code >> 4 : code & 15;
	switch (code) {
	case 0:
		*mbps = 10;
		break;
	case 1:
		*mbps = 100;
		break;
	case 2:
		*mbps = 1000;
		break;
	case 4:
		*mbps = 10000;
		break;
	case 5:
		*mbps = 2500;
		break;
	case 6:
		*mbps = 5000;
		break;
	default:
		break;
	}
	return true;
}
#endif
