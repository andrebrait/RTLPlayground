/*
 * test_page_json_836x.c - the RTL836x side of httpd/page_impl.c: /status.json
 * takes link and speed from PORTn_STATUS, /information.json shows no
 * temperature, and the pages of features that are not ported yet still
 * answer with valid JSON. page_impl.c is compiled unmodified.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Define the SFRs once here, as hw_mock.c does for the RTL837x tests */
#undef __sfr
#undef __sfr16
#undef __sfr32
#undef __sbit
#define __sfr   volatile uint8_t
#define __sfr16 volatile uint16_t
#define __sfr32 volatile uint32_t
#define __sbit  bool
#include "rtl837x_sfr.h"
#undef __sfr
#undef __sfr16
#undef __sfr32
#undef __sbit
#define __sfr   extern volatile uint8_t
#define __sfr16 extern volatile uint16_t
#define __sfr32 extern volatile uint32_t
#define __sbit  extern bool

#include "rtl837x_common.h"
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"
#include "rtl837x_port.h"
#include "page_impl.h"
#include "support.h"

extern uint8_t  outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;

uint8_t sfr_data[4];
static uint16_t regfile[0x4000];
static uint16_t phy_regs[8][32];
static uint16_t eee_adv[8];
/* Include the alternate selector just beyond the last physical port. */
static uint16_t mib_words[8 * 0x7c + 4];
static uint16_t mib_window[4];
static unsigned selector_writes, control_reads;
static unsigned busy_reads;
static bool selector_stuck;

void reg_write(uint16_t addr)
{
	CHECK(addr == RTL836X_REG_MIB_ADDRESS, "MIB reads write only the selector");
	selector_writes++;
	if (selector_stuck)
		return;
	uint16_t selector = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
	if (regfile[addr] != selector) {
		for (unsigned i = 0; i < 4; i++)
			mib_window[i] = mib_words[selector * 4 + i];
	}
	regfile[addr] = selector;
}

static void mib_set(unsigned port, unsigned index, uint64_t value)
{
	unsigned offset = port * 0x7c + index * 2 + (index > 0 ? 2 : 0) +
		(index > 19 ? 2 : 0) + (index > 28 ? 2 : 0);
	unsigned words = index == 0 || index == 19 || index == 28 ? 4 : 2;
	for (unsigned i = 0; i < words; i++, value >>= 16)
		mib_words[offset + i] = value;
}

void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
	(void)dev_id;
	if (reg == 0xa5d0) {
		SFR_DATA_U16 = eee_adv[phy_id];
		SFR_DATA_8 = SFR_DATA_U16 >> 8;
		SFR_DATA_0 = SFR_DATA_U16;
		return;
	}
	if (reg == 0xa412)
		reg = 9;
	if (phy_id >= 8 || reg >= 32) {
		CHECK(false, "only MII registers used on the RTL836x");
		return;
	}
	SFR_DATA_U16 = phy_regs[phy_id][reg];
	SFR_DATA_8 = phy_regs[phy_id][reg] >> 8;
	SFR_DATA_0 = phy_regs[phy_id][reg];
}

/* The RTL8370N returns the 16-bit value in a6:a7 */
void reg_read(uint16_t addr)
{
	if (addr >= RTL836X_REG_MIB_COUNTER && addr < RTL836X_REG_MIB_COUNTER + 4) {
		SFR_DATA_U16 = mib_window[addr - RTL836X_REG_MIB_COUNTER];
	} else {
		SFR_DATA_U16 = regfile[addr];
	}
	if (addr == RTL836X_REG_MIB_CTRL) {
		control_reads++;
		if (busy_reads) {
			busy_reads--;
			SFR_DATA_U16 |= RTL836X_MIB_BUSY;
		}
	}
	SFR_DATA_8 = SFR_DATA_U16 >> 8;
	SFR_DATA_0 = SFR_DATA_U16;
}

void reg_read_m(uint16_t addr)
{
	reg_read(addr);
	sfr_data[0] = sfr_data[1] = 0;
	sfr_data[2] = SFR_DATA_8;
	sfr_data[3] = SFR_DATA_0;
}

static const char *body(void)
{
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	const char *p = strstr((const char *)outbuf, "\r\n\r\n");
	return p ? p + 4 : (const char *)outbuf;
}

static int has(const char *needle) { return strstr(body(), needle) != 0; }

static int port_has(uint8_t port, const char *needle)
{
	char key[20];
	snprintf(key, sizeof(key), "\"logPort\":%u,", port);
	const char *p = strstr(body(), key);
	if (!p)
		return 0;
	const char *value = strstr(p, needle);
	const char *end = strchr(p, '}');
	return value && end && value < end;
}

/* Brackets and braces balance outside strings, quotes pair up. */
static int balanced(void)
{
	int depth = 0, instr = 0;
	for (const char *p = body(); *p; p++) {
		if (*p == '"') { instr = !instr; continue; }
		if (instr) continue;
		if (*p == '{' || *p == '[') depth++;
		if (*p == '}' || *p == ']') depth--;
		if (depth < 0) return 0;
	}
	return depth == 0 && !instr && *body();
}

static uint64_t value64(const uint8_t *v)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < 8; i++)
		n = n << 8 | v[i];
	return n;
}

static void t_mib(void)
{
	uint8_t v[8];
	printf("[test] native MIB latch, widths, carry, failures and boundary ports\n");
	mib_set(0, 0, UINT64_C(0x123456789abcdef0));
	CHECK(rtl836x_mib_read(0, 0, v) && value64(v) == UINT64_C(0x123456789abcdef0), "64-bit octets retain all words");
	mib_set(7, 57, 0x89abcdef);
	CHECK(rtl836x_mib_read(7, 57, v) && value64(v) == 0x89abcdef, "last native counter on port 7");
	CHECK(!rtl836x_mib_read(8, 0, v) && !rtl836x_mib_read(0, 58, v), "reject CPU/nonexistent port and global counter");
	mib_set(0, 1, 0x11223344);
	mib_set(0, 2, 0xaabbccdd);
	CHECK(rtl836x_mib_read(0, 1, v) && value64(v) == 0x11223344, "first half of shared block");
	CHECK(rtl836x_mib_read(0, 2, v) && value64(v) == 0xaabbccdd, "second half of shared block");
	mib_set(0, 2, 0x12345678);
	unsigned writes = selector_writes;
	CHECK(rtl836x_mib_read(0, 2, v) && value64(v) == 0x12345678 && selector_writes == writes + 2, "same selector relatches via alternate block");
	mib_set(0, 38, 0xffffffff);
	mib_set(0, 39, 0xffffffff);
	mib_set(0, 40, 0xffffffff);
	CHECK(rtl836x_mib_summary(0, 0, v) && value64(v) == UINT64_C(0x2fffffffd), "TX packet sum carries above 32 bits");
	mib_set(0, 7, 0xffffffff);
	mib_set(0, 9, 2);
	mib_set(0, 10, 3);
	CHECK(rtl836x_mib_summary(0, 2, v) && value64(v) == UINT64_C(0x100000004), "RX packet sum carries above 32 bits");
	mib_set(0, 34, 17);
	CHECK(rtl836x_mib_summary(0, 1, v) && value64(v) == 17, "TX bad is excessive-collision frames only");
	CHECK(rtl836x_mib_summary(0, 3, v) && value64(v) == 0x11223344, "RX bad is FCS-error frames only");
	busy_reads = 3;
	control_reads = 0;
	CHECK(rtl836x_mib_read(0, 0, v) && control_reads >= 4, "busy latch eventually becomes available");
	mib_set(7, 19, UINT64_C(0x100000005));
	mib_set(7, 28, UINT64_C(0xfedcba9876543210));
	CHECK(rtl836x_mib_read(7, 19, v) && value64(v) == UINT64_C(0x100000005), "middle 64-bit latch alignment");
	CHECK(rtl836x_mib_read(7, 28, v) && value64(v) == UINT64_C(0xfedcba9876543210), "TX octet latch alignment");
	regfile[RTL836X_REG_MIB_CTRL] = RTL836X_MIB_BUSY;
	control_reads = 0;
	CHECK(!rtl836x_mib_read(0, 0, v) && control_reads <= 100, "busy timeout is bounded");
	regfile[RTL836X_REG_MIB_CTRL] = RTL836X_MIB_RESET;
	CHECK(!rtl836x_mib_read(0, 0, v), "reset is unavailable");
	CHECK(!rtl836x_mib_summary(0, 0, v), "unavailable aggregate is not a valid partial sum");
	regfile[RTL836X_REG_MIB_CTRL] = 0;
	selector_stuck = true;
	writes = selector_writes;
	CHECK(!rtl836x_mib_read(7, 0, v) && selector_writes - writes <= 100, "selector timeout is bounded");
	selector_stuck = false;
}

static void t_status(void)
{
	printf("[test] /status.json: link and speed from PORTn_STATUS\n");
	regfile[RTL836X_REG_PORT0_STATUS + 0] = PORT_STATUS_LINK | 2;	/* 1000M */
	regfile[RTL836X_REG_PORT0_STATUS + 1] = PORT_STATUS_LINK | 1 | 0x40;	/* 100M, TX pause */
	regfile[RTL836X_REG_PORT0_STATUS + 2] = PORT_STATUS_LINK;	/* 10M */
	regfile[RTL836X_REG_PORT0_STATUS + 3] = 2;			/* down */
	phy_regs[0][0] = 0x0800; /* powered down */
	phy_regs[1][4] = 0x0de0; /* pause, asymmetric pause, 100F/H and 10F/H */
	phy_regs[1][9] = 0x0200; /* 1000F */
	send_status();
	CHECK(port_has(0, "\"link\":3,") && port_has(0, "\"enabled\":0,"), "1000M is LINKS[3], BMCR power down is disabled");
	CHECK(port_has(1, "\"link\":2,") && port_has(1, "\"enabled\":1,"), "100M is LINKS[2], powered up is enabled");
	CHECK(port_has(1, "\"adv\":\"011111\""), "MII ANAR and GBCR advertise the supported speeds");
	CHECK(port_has(1, "\"flowcontrol\":3") && port_has(1, "\"pause_tx\":1") &&
	      port_has(1, "\"pause_rx\":0"), "advertised pause direction differs from negotiated TX/RX pause");
	CHECK(port_has(2, "\"link\":1,"), "10M is LINKS[1]");
	CHECK(port_has(3, "\"link\":0,"), "link bit clear is down, even with speed bits set");
	CHECK(has("\"logPort\":7,") && !has("\"logPort\":8,"), "8 ports");
	CHECK(port_has(0, "\"txG\":\"0x2fffffffd\"") && port_has(0, "\"rxG\":\"0x100000004\""), "real totals preserve aggregate carry");
	CHECK(port_has(0, "\"txB\":\"0x11\"") && port_has(0, "\"rxB\":\"0x11223344\""), "explicit error semantics");
	CHECK(port_has(0, "\"macs\":null"), "unsupported occupancy is unavailable");
	CHECK(slen <= TCP_OUTBUF_SIZE, "fits the buffer");
	CHECK(balanced(), "valid JSON");
	regfile[RTL836X_REG_MIB_CTRL] = RTL836X_MIB_RESET;
	send_status();
	CHECK(port_has(0, "\"txG\":null") && port_has(7, "\"rxB\":null"), "unavailable counters never become zero");
	out_reset();
	port_stats_print();
	CHECK(strstr(out_buf, "TX excess") && strstr(out_buf, "RX FCS") && strstr(out_buf, "n/a"), "CLI labels error semantics and unavailable reads");
	regfile[RTL836X_REG_MIB_CTRL] = 0;
	out_reset();
	port_stats_print();
	CHECK(strstr(out_buf, "00000002fffffffd"), "CLI retains aggregate carry");
}

static void t_info(void)
{
	printf("[test] /information.json without a temperature\n");
	send_basic_info();
	CHECK(has("\"chip_temp\":\"n/a\""), "temperature n/a");
	CHECK(balanced(), "valid JSON");
	CHECK(has("\"mib_family\":\"rtl836x\""), "native label discriminator");
	CHECK(has("\"poe_ports\":4"), "board reports four GPIO-controlled PoE ports");
}

static void t_counters(void)
{
	CHECK(!send_counters(1) && balanced(), "native counters are a JSON array");
	CHECK(strstr(body(), "[\"0x123456789abcdef0\",\"0x11223344\",\"0x12345678\"") == body(), "native order and 64/32-bit hex shape");
	unsigned entries = 1;
	for (const char *p = body(); *p; p++)
		if (*p == ',') entries++;
	CHECK(entries == RTL836X_MIB_COUNT, "58 native entries, no fabricated queue counters");
	CHECK(!send_counters(8) && has("\"0x89abcdef\"]"), "physical port 8 maps to last logical port");
	CHECK(has("\"0x100000005\"") && has("\"0xfedcba9876543210\""), "detail retains middle and TX 64-bit octets");
	CHECK(send_counters(0) && send_counters(9), "invalid physical ports rejected");
	regfile[RTL836X_REG_MIB_CTRL] = RTL836X_MIB_RESET;
	CHECK(!send_counters(1) && strncmp(body(), "[null,null,", 11) == 0 && balanced(), "detail unavailable entries are null");
	regfile[RTL836X_REG_MIB_CTRL] = 0;
}

static void t_eee(void)
{
	eee_adv[0] = 2;
	eee_adv[7] = 6;
	regfile[RTL836X_REG_PORT0_STATUS] = 0x0415;
	regfile[RTL836X_REG_PORT0_STATUS + 7] = 0x0016;
	send_eee();
	CHECK(has("\"portNum\":1,\"isSFP\":0,\"eee\":\"001\",\"eee_lp\":null,\"active\":1"),
	      "100M EEE advertisement and negotiated LPI come from native registers");
	CHECK(has("\"portNum\":8,\"isSFP\":0,\"eee\":\"011\",\"eee_lp\":null,\"active\":0"),
	      "gigabit EEE advertisement is distinct from active negotiation and unknown peer data");
	CHECK(balanced(), "native EEE JSON is valid");
}

static void t_poe(void)
{
	regfile[RTL836X_REG_GPIO_67C_O_X0] = 0x0386; /* only front port 2 is disabled */
	send_status();
	CHECK(port_has(0, "\"poe_admin\":1") && port_has(1, "\"poe_admin\":0") &&
	      port_has(2, "\"poe_admin\":1") && port_has(3, "\"poe_admin\":1"),
	      "PoE administrative state follows the GPIO map, not the Ethernet link");
	CHECK(port_has(0, "\"poe_on\":null") && !port_has(4, "\"poe_admin\":"),
	      "power delivery stays unknown and non-PoE ports are not presented as controllable");
}

static void t_unported(void)
{
	printf("[test] pages of unported features answer with valid JSON\n");
	send_vlan(1);		CHECK(balanced(), "/vlan.json");
	send_vlanlist();	CHECK(balanced() && has("\"vlan\":[]"), "/vlanlist");
	send_l2(0);		CHECK(balanced(), "/l2.json");
	l2_delete(0);		CHECK(balanced(), "/l2_del.json");
	send_mirror();		CHECK(balanced(), "/mirror.json");
	send_lag();		CHECK(balanced(), "/lag.json");
	send_bandwidth();	CHECK(balanced(), "/bandwidth.json");
	send_storm();		CHECK(balanced(), "/storm.json");
	send_isolation();	CHECK(balanced(), "/isolation.json");
	send_mtu();		CHECK(balanced(), "/mtu.json");
	send_lacp();		CHECK(balanced(), "/lacp.json");
	send_lldp();		CHECK(balanced(), "/lldp.json");
	send_stp();		CHECK(balanced(), "/stp.json");
	send_stp_counters();	CHECK(balanced(), "/stpcnt.json");
	extern uint8_t stp_test_no_entities;
	stp_test_no_entities = 1;
	send_stp();		CHECK(balanced() && has("\"ports\":[]"), "/stp.json without entities");
	send_stp_counters();	CHECK(balanced() && has("\"ports\":[]"), "/stpcnt.json without entities");
}

int main(void)
{
	printf("== httpd/page_impl.c RTL836x JSON tests ==\n");
	t_mib();
	t_status();
	t_info();
	t_counters();
	t_eee();
	t_poe();
	t_unported();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
