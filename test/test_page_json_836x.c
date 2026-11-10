/*
 * test_page_json_836x.c - the RTL836x side of httpd/page_impl.c: /status.json
 * takes link and speed from PORTn_STATUS, /information.json shows no
 * temperature, VLANs reach the 4K table and the member configurations, and
 * the pages of features that are not ported yet still answer with valid JSON.
 * page_impl.c is compiled unmodified.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "uip.h"

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
#include "rtl837x_lacp.h"
#include "rtl837x_bandwidth.h"
#include "rtl837x_storm.h"
#include "rtl836x_rate.h"
#include "rtl837x_igmp.h"
#include "rtl836x_igmp.h"
#include "cmd_parser.h"
#include "page_impl.h"
#include <test/support.h>
#include "snmp_hw.h"

extern uint8_t  outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;
extern uint8_t  vlan_names[VLAN_NAMES_SIZE];
extern uint16_t vlan_ptr;

uint8_t sfr_data[4];
uint8_t igmpEnabled;
uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
static uint16_t regfile[0x4000];
static uint16_t phy_regs[8][32];
static uint16_t eee_adv[8];
/* Include the alternate selector just beyond the last physical port. */
static uint16_t mib_words[8 * 0x7c + 4];
static uint16_t mib_window[4];
static unsigned selector_writes, control_reads, other_writes, bad_table_cmds;
static unsigned busy_reads;
static bool selector_stuck;
static uint16_t vlan4k[4096][3];	/* the 4K VLAN table behind TABLE_ACCESS_CTRL */

/* RTL8370B LUT: six words, with SRAM/CAM address bits split in status. */
static struct {
	uint16_t idx, words[6];
	bool present;
} lut[48];
static bool lut_stuck, lut_write_miss;
static uint16_t lut_timeout_command;
static unsigned lut_commands, lut_status_reads;

static void lut_exec(uint16_t command)
{
	int found = -1, first = -1;
	uint16_t start = regfile[0x0501];
	lut_commands++;
	if (command == lut_timeout_command) {
		regfile[0x0502] = 0x3000; /* stale HIT must not count while BUSY */
		return;
	}
	if (command == 0x000c && lut_write_miss) {
		regfile[0x0502] = 0;
		return;
	}
	for (unsigned i = 0; i < 48; i++) {
		if (!lut[i].present)
			continue;
		if (command == 0x000c || command == 0x0004) {
			if (memcmp(lut[i].words, &regfile[0x0510], 6) ||
			    (lut[i].words[3] & 0x3fff) != (regfile[0x0513] & 0x3fff) ||
			    (!(lut[i].words[3] & 0x1000) &&
			     (lut[i].words[4] & 0x007f) != (regfile[0x0514] & 0x007f)))
				continue;
			found = i;
			break;
		}
		if (command == 0x0014) {
			if (lut[i].idx == start) { found = i; break; }
		} else if (command == 0x0034) {
			if ((lut[i].words[2] & 0x0100) || (lut[i].words[3] & 0x1000))
				continue;
			if (first < 0 || lut[i].idx < lut[first].idx)
				first = i;
			if (lut[i].idx >= start && (found < 0 || lut[i].idx < lut[found].idx))
				found = i;
		} else {
			bad_table_cmds++;
		}
	}
	if (command == 0x0034 && found < 0)
		found = first;
	if (command == 0x000c) {
		if (found < 0) {
			for (unsigned i = 0; i < 48; i++) {
				if (!lut[i].present) { found = i; lut[i].idx = 0x1000 + i; break; }
			}
		}
		if (found >= 0) {
			memcpy(lut[found].words, &regfile[0x0510], 12);
			uint16_t *w = lut[found].words;
			lut[found].present = (w[5] & 0x0020) || ((w[3] & 0x1000) ?
				((w[4] & 0x00ff) || (w[3] & 0xc000) || (w[5] & 0x0080)) : (w[4] & 0x3800));
		}
	}
	regfile[0x0502] = 0;
	if (found >= 0) {
		uint16_t idx = lut[found].idx;
		regfile[0x0502] = 0x1000 | (idx & 0x07ff) | ((idx & 0x0800) << 3) | ((idx & 0x1000) >> 1);
		if (command != 0x000c)
			memcpy(&regfile[0x0520], lut[found].words, 12);
	}
}

static void lut_put(unsigned slot, uint16_t idx, const uint8_t *mac, uint16_t vid,
		    uint8_t chip, bool is_static)
{
	lut[slot].present = true;
	lut[slot].idx = idx;
	for (unsigned i = 0; i < 3; i++)
		lut[slot].words[2 - i] = (uint16_t)mac[2 * i] << 8 | mac[2 * i + 1];
	lut[slot].words[3] = vid | ((uint16_t)(chip >> 3) << 15);
	lut[slot].words[4] = (uint16_t)(chip & 7) << 8 | 0x0800;
	lut[slot].words[5] = is_static ? 0x0020 : 0;
}

void reg_write(uint16_t addr)
{
	uint16_t v = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;

	if (addr != RTL836X_REG_MIB_ADDRESS) {
		other_writes++;
		regfile[addr] = v;
		if (addr != RTL836X_REG_TABLE_ACCESS_CTRL)
			return;
		if ((v & 7) == 4) {
			lut_exec(v);
			return;
		}
		if (v != TABLE_ACCESS_CVLAN_READ && v != TABLE_ACCESS_CVLAN_WRITE)
			bad_table_cmds++;
		uint16_t vid = regfile[RTL836X_REG_TABLE_ACCESS_ADDR] & 0xfff;
		for (unsigned w = 0; w < 3; w++) {
			if (v == TABLE_ACCESS_CVLAN_WRITE)
				vlan4k[vid][w] = regfile[RTL836X_REG_TABLE_WRITE_DATA0 + w];
			else
				regfile[RTL836X_REG_TABLE_READ_DATA0 + w] = vlan4k[vid][w];
		}
		return;
	}
	selector_writes++;
	if (selector_stuck)
		return;
	if (regfile[addr] != v) {
		for (unsigned i = 0; i < 4; i++)
			mib_window[i] = mib_words[v * 4 + i];
	}
	regfile[addr] = v;
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
	if (addr == 0x0502) {
		lut_status_reads++;
		if (lut_stuck)
			SFR_DATA_U16 |= 0x2000;
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

void reg_bit_set(uint16_t addr, char bit) { regfile[addr] |= 1u << bit; other_writes++; }
void reg_bit_clear(uint16_t addr, char bit) { regfile[addr] &= ~(1u << bit); other_writes++; }
uint8_t reg_bit_test(uint16_t addr, char bit) { return (regfile[addr] >> bit) & 1; }

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
	snprintf(key, sizeof(key), "\"portNum\":%u,", port + 1);
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
	unsigned others = other_writes;
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
	CHECK(other_writes == others, "MIB reads write only the selector");
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
	phy_regs[1][5] = 0x0400; /* the link partner offers symmetric pause */
	send_status();
	CHECK(port_has(0, "\"link\":3,") && port_has(0, "\"enabled\":0,"), "1000M is LINKS[3], BMCR power down is disabled");
	CHECK(port_has(1, "\"link\":2,") && port_has(1, "\"enabled\":1,"), "100M is LINKS[2], powered up is enabled");
	CHECK(port_has(1, "\"adv\":\"011111\""), "MII ANAR and GBCR advertise the supported speeds");
	CHECK(port_has(1, "\"fc\":23,"),
	      "fc: both pause bits advertised, partner pause, negotiated TX pause only");
	CHECK(port_has(2, "\"link\":1,"), "10M is LINKS[1]");
	CHECK(port_has(3, "\"link\":0,"), "link bit clear is down, even with speed bits set");
	CHECK(has("\"portNum\":8,") && !has("\"portNum\":9,") && !has("logPort"), "8 ports, logPort only in /information.json");
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
	CHECK(has("\"logPort\":[0,1,2,3,4,5,6,7]"), "front-panel to chip port map");
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
	send_status();
	CHECK(port_has(0, "\"eee\":1025,"), "eee: 100M advertised and LPI active from native registers");
	CHECK(port_has(7, "\"eee\":3,"), "eee: 100M and 1G advertised, not active, partner unknown");
	CHECK(balanced(), "status JSON with eee is valid");
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

#define MC(i, word)	regfile[RTL836X_REG_VLAN_MEMBER_CONFIGURATION0 + 4 * (i) + (word)]
#define MISC_CFG(port)	regfile[RTL836X_REG_PORT0_MISC_CFG + ((port) << 5)]
#define ACCEPT(i)	regfile[RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0 + (i)]

/* The member configuration a chip port selects */
static unsigned pvid_index(unsigned port)
{
	return (regfile[RTL836X_REG_VLAN_PVID_CTRL0 + port / 2] >> ((port & 1) * 8)) & 0x1f;
}

static void name_vlan(uint16_t vid, const char *name)
{
	static const char h[] = "0123456789abcdef";
	vlan_names[vlan_ptr++] = h[(vid >> 8) & 0xf];
	vlan_names[vlan_ptr++] = h[(vid >> 4) & 0xf];
	vlan_names[vlan_ptr++] = h[vid & 0xf];
	while (*name) vlan_names[vlan_ptr++] = *name++;
	vlan_names[vlan_ptr++] = ' ';
	vlan_names[vlan_ptr] = 0;
}

static void t_vlan(void)
{
	printf("[test] VLANs: 4K table, member configurations, PVID, frame types and their JSON\n");
	for (unsigned i = 0; i < 4 * 32; i++)
		MC(0, i) = 0x5a5a;	/* whatever the chip holds before the setup */
	regfile[RTL836X_REG_VLAN_PVID_CTRL0 + 2] = 0x0303;
	for (unsigned p = 0; p < 8; p++)
		MISC_CFG(p) = 0x4cb0;
	MISC_CFG(10) = PORT_MISC_CFG_VLAN_EGRESS_MODE | 0x0001;
	ACCEPT(0) = ACCEPT(1) = 0xffff;
	vlan_setup();
	CHECK(vlan4k[1][0] == 0xffff && vlan4k[1][1] == 0 && vlan4k[1][2] == 0x0024,
	      "VLAN 1: front ports and CPU port 10 untagged, FID 0, shared learning");
	CHECK(MC(0, 0) == 0x04ff && MC(0, 3) == 1 && MC(1, 3) == 0 && MC(31, 0) == 0,
	      "configuration 0 is VLAN 1 with its members, the others are cleared");
	int pvid1 = 1;
	for (unsigned p = 0; p <= 10; p++)
		pvid1 &= pvid_index(p) == 0;
	CHECK(pvid1 && port_pvid_get(0) == 1 && port_pvid_get(7) == 1 && port_pvid_get(CPU_PORT) == 1,
	      "every port has PVID 1");
	CHECK((MISC_CFG(10) & PORT_MISC_CFG_VLAN_EGRESS_MODE) == 0 && (MISC_CFG(10) & 1),
	      "egress tag mode original, the rest of PORT10_MISC_CFG kept");
	for (unsigned p = 0; p < 8; p++)
		CHECK(MISC_CFG(p) == 0x4c80, "front-port egress follows VLAN untag bits without changing rate-control flags");
	CHECK(regfile[RTL836X_REG_VLAN_INGRESS] == 0x04ff && ACCEPT(0) == 0 && ACCEPT(1) == 0,
	      "ingress filter on the front ports and the CPU port, all frame types accepted");
	CHECK(regfile[RTL836X_REG_VLAN_CTRL] & VLAN_CTRL_ENABLE, "VLANs on");

	vlan_settings.vlan = 10;
	vlan_settings.members = 0x0006;
	vlan_settings.tagged = 0x0004;
	vlan_create();
	CHECK(vlan4k[10][0] == 0x0206 && vlan4k[10][2] == 0x0004,
	      "VLAN 10: port 1 untagged, port 2 tagged, the CPU port tagged to retain ingress VID");
	port_pvid_set(1, 10);
	unsigned idx = pvid_index(1);
	CHECK(idx != 0 && MC(idx, 3) == 10 && MC(idx, 0) == 0x0406 && pvid_index(0) == 0,
	      "PVID 10 takes a free configuration with the members of the 4K entry");
	port_pvid_set(2, 10);
	CHECK(pvid_index(2) == idx && port_pvid_get(2) == 10, "a second port shares it");
	vlan_settings.vlan = 10;
	vlan_settings.members = 0x0002;
	vlan_settings.tagged = 0;
	vlan_create();
	CHECK(MC(idx, 0) == 0x0402, "changing VLAN 10 updates its configuration");
	port_pvid_set(1, 1);
	port_pvid_set(2, 1);
	port_pvid_set(3, 20);
	CHECK(pvid_index(1) == 0 && pvid_index(3) == idx && port_pvid_get(3) == 20 && port_pvid_get(1) == 1,
	      "a configuration no port selects is reused");
	port_pvid_set(5, 0);
	CHECK(port_pvid_get(5) == 0 && port_pvid_get(3) == 20, "PVID 0, as the VLAN page parks tagged-only ports");

	CHECK(port_ingress_filter(4, VLAN_TAGGED) && port_ingress_filter(CPU_PORT, VLAN_UNTAGGED)
	      && ACCEPT(0) == 0x0100 && ACCEPT(1) == 0x0020, "frame types: 2 bits per port, the CPU port is port 10");
	CHECK(port_ingress_filter_get(4) == VLAN_TAGGED && port_ingress_filter_get(CPU_PORT) == VLAN_UNTAGGED
	      && port_ingress_filter_get(0) == VLAN_ALL, "and read back");
	CHECK(!port_ingress_filter(8, VLAN_ALL) && !port_ingress_filter(0, VLAN_INVALID)
	      && port_ingress_filter_get(8) == VLAN_INVALID, "no port 8 and no mode 3");
	CHECK(port_ingress_vlan_filter_set(5, false) && !port_ingress_vlan_filter_get(5)
	      && port_ingress_vlan_filter_get(CPU_PORT) && regfile[RTL836X_REG_VLAN_INGRESS] == 0x04df,
	      "ingress filter per port");

	vlan_ptr = 0;
	vlan_names[0] = 0;
	name_vlan(10, "lab");
	port_pvid_set(1, 10);
	send_vlan(10);
	CHECK(has("\"members\":\"0x2000a02\"") && has("\"name\":\"lab\"") && has("\"pvid\":\"0x0002\"") && balanced(),
	      "/vlan.json: native CPU membership is tagged, with name and front-port PVIDs");
	send_vlanlist();
	CHECK(has("{\"id\":1,\"name\":\"\"}") && has("{\"id\":10,\"name\":\"lab\"}") && !has("\"id\":20") && balanced(),
	      "/vlanlist: the VLANs with members");
	unsigned idx10 = pvid_index(1);
	vlan_delete(10);
	CHECK(!vlan4k[10][0] && !vlan4k[10][2] && MC(idx10, 0) == 0 && port_pvid_get(1) == 10,
	      "a deleted VLAN loses its members, ports keep it as PVID");
	CHECK(vlan_name(10) == 0xffff, "and its name");
	send_vlanlist();
	CHECK(!has("\"id\":10") && has("\"id\":1,") && balanced(), "/vlanlist drops it");
	vlan_settings.vlan = 4095;
	vlan_create();
	vlan_delete(0);
	CHECK(!vlan4k[4095][0] && !vlan4k[0][0] && vlan_get(4095) == -1, "VLANs 0 and 4095 are refused");
	CHECK(!bad_table_cmds, "only 4K VLAN table commands");
}

static void t_isolation(void)
{
	printf("[test] isolation: logical masks with the CPU as bit 9, chip masks with it as bit 10\n");
	for (unsigned p = 0; p <= 10; p++)
		regfile[RTL836X_REG_PORT_ISOLATION_PORT0_MASK + p] = 0x07ff;	/* reset value */
	CHECK(port_isolation_get(3) == (0xff | PMASK_CPU), "all ports and the CPU at reset");
	port_isolate(2, 0x0001 | PMASK_CPU);
	CHECK(regfile[RTL836X_REG_PORT_ISOLATION_PORT0_MASK + 2] == 0x0401 && port_isolation_get(2) == (0x0001 | PMASK_CPU),
	      "port 3 may send to port 1 and the CPU");
	port_isolate(CPU_PORT, 0);
	port_isolate(8, 0);
	CHECK(regfile[RTL836X_REG_PORT_ISOLATION_PORT0_MASK + 10] == 0x07ff && regfile[RTL836X_REG_PORT_ISOLATION_PORT0_MASK + 8] == 0x07ff,
	      "the CPU port and ports past the last front port are left alone");
	send_isolation();
	CHECK(has("{\"portNum\":3,\"allow\":\"0201\"}") && has("{\"portNum\":4,\"allow\":\"02ff\"}") && balanced(),
	      "/isolation.json");
}

#define MAX_LEN_CFG(i)	regfile[RTL836X_REG_MAX_LEN_CFG0 + (i)]
static void t_mtu(void)
{
	printf("[test] MTU: two length configurations, picked per port for both speeds\n");
	regfile[RTL836X_REG_MAX_LENGTH_CFG] = 0;
	MAX_LEN_CFG(0) = MAX_LEN_CFG(1) = 0x3ff0;	/* reset values */
	CHECK(port_mtu_get(0) == 16368 && port_mtu_get(7) == 16368, "16368 on every port at reset");
	CHECK(port_mtu_set(2, 16368) && regfile[RTL836X_REG_MAX_LENGTH_CFG] == 0, "a length already held is only selected");
	CHECK(port_mtu_set(2, 1522) && MAX_LEN_CFG(1) == 1522 && MAX_LEN_CFG(0) == 16368
	      && regfile[RTL836X_REG_MAX_LENGTH_CFG] == 0x0404 && port_mtu_get(2) == 1522 && port_mtu_get(3) == 16368,
	      "a new length takes the configuration no other port uses, for 10/100M and 1G");
	CHECK(!port_mtu_set(4, 9000) && port_mtu_get(4) == 16368, "a third length is refused");
	CHECK(port_mtu_set(4, 1522) && regfile[RTL836X_REG_MAX_LENGTH_CFG] == 0x1414, "a second port shares 1522");
	CHECK(!port_mtu_set(2, 9000), "while two ports use 1522, neither can move to a third length");
	CHECK(port_mtu_set(4, 16368) && port_mtu_set(2, 9000) && MAX_LEN_CFG(1) == 9000 && port_mtu_get(2) == 9000
	      && port_mtu_get(4) == 16368, "the last port on a configuration changes its length");
	send_mtu();
	CHECK(has("{\"portNum\":3,\"mtu\":\"0x2328\"}") && has("{\"portNum\":1,\"mtu\":\"0x3ff0\"}") && balanced(), "/mtu.json");
}

static void t_mirror(void)
{
	__xdata struct port_mirror m;

	printf("[test] mirroring: MIRROR_CTRL and the shared source mask, logical ports on the API\n");
	regfile[RTL836X_REG_MIRROR_CTRL] = 0x0800;	/* the isolation bit is not ours */
	port_mirror_get(&m);
	CHECK(!m.enabled, "off at reset");
	/* front port 3 (index 2) RX+TX to front port 2 (index 1) */
	CHECK(port_mirror_set(1, PORT_BIT(2), PORT_BIT(2)), "accepted");
	CHECK(regfile[RTL836X_REG_MIRROR_CTRL] == 0x0e12 && regfile[RTL836X_REG_MIRROR_SRC_PMSK] == 0x0004,
	      "source 2, monitor 1, RX and TX on, source mask port 2");
	port_mirror_get(&m);
	CHECK(m.enabled && m.port == 1 && m.rx_pmask == PORT_BIT(2) && m.tx_pmask == PORT_BIT(2), "read back");
	send_mirror();
	CHECK(has("{\"enabled\":1,\"mPort\":2,\"mirror_rx\":\"0000000000000100\",\"mirror_tx\":\"0000000000000100\"}")
	      && balanced(), "/mirror.json");
	CHECK(!port_mirror_set(1, PORT_BIT(3), PORT_BIT(4)) && !port_mirror_set(1, PORT_BIT(1), 0)
	      && !port_mirror_set(1, 0, 0) && regfile[RTL836X_REG_MIRROR_CTRL] == 0x0e12,
	      "mixed directions, a mirrored monitor port and no source are refused untouched");
	CHECK(port_mirror_set(0, PORT_BIT(3) | PMASK_CPU, 0) && regfile[RTL836X_REG_MIRROR_CTRL] == 0x0a03
	      && regfile[RTL836X_REG_MIRROR_SRC_PMSK] == 0x0408,
	      "RX only, lowest source first, the CPU is chip port 10");
	port_mirror_del();
	port_mirror_get(&m);
	CHECK(regfile[RTL836X_REG_MIRROR_CTRL] == 0x0803 && !m.enabled && !m.rx_pmask && !m.tx_pmask, "off");
	send_mirror();
	CHECK(has("{\"enabled\":0,\"mPort\":1,") && balanced(), "/mirror.json when off");
}

/*
 * Static LAG: GROUP_MASK 0x08d0 holds one nibble per group (rtl8367c_setAsicTrunkingGroup), nibble n
 * bit k being chip port 4n + k, so its low byte is the chip port mask; the 7-bit hash field mask
 * in TRUNK_CTRL 0x08cf is global (rtl8367c_setAsicTrunkingHashSelect). The RTL8370N has two groups.
 */
static void t_lag(void)
{
	unsigned writes;

	printf("[test] static LAG: two fixed groups on ports 1-4 and 5-8, one hash for both\n");
	CHECK(PORT_LAG_COUNT == 2 && port_lag_members_get(0) == 0 && port_lag_members_get(1) == 0
	      && port_lag_of(2) == PORT_LAG_NONE, "two groups, none configured at reset");
	regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK] = 0x0300;	/* chip ports 8 and 9 are not ours */
	regfile[RTL836X_REG_PORT_TRUNK_CTRL] = 0x0100;		/* reset: dumb mode on, no hash field */
	CHECK(!port_lag_members_set(0, PORT_BIT(4)) && port_lag_members_set(0, 0)
	      && regfile[RTL836X_REG_PORT_TRUNK_CTRL] == 0x0100, "a refused or empty set leaves a zero hash selector alone");
	regfile[RTL836X_REG_PORT_TRUNK_CTRL] = 0x0180;		/* the mode and flood bits are not ours */
	CHECK(port_lag_members_set(0, PORT_BIT(0) | PORT_BIT(2)) && regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK] == 0x0305
	      && regfile[RTL836X_REG_PORT_TRUNK_CTRL] == 0x01ff,
	      "LAG 1 takes ports 1 and 3 in the low nibble, and a zero hash selector becomes all seven fields");
	CHECK(port_lag_members_set(1, PORT_BIT(6) | PORT_BIT(7)) && regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK] == 0x03c5,
	      "LAG 2 takes ports 7 and 8 in the high nibble, LAG 1 and chip ports 8, 9 stay");
	CHECK(port_lag_members_get(0) == 0x0005 && port_lag_members_get(1) == 0x00c0 && port_lag_members_get(2) == 0,
	      "members read back as logical masks, per group");
	CHECK(port_lag_of(0) == 0 && port_lag_of(2) == 0 && port_lag_of(1) == PORT_LAG_NONE && port_lag_of(7) == 1
	      && port_lag_of(5) == PORT_LAG_NONE && port_lag_of(8) == PORT_LAG_NONE && port_lag_of(CPU_PORT) == PORT_LAG_NONE,
	      "the group of a port, none for others and the CPU port");

	writes = other_writes;
	CHECK(!port_lag_members_set(2, PORT_BIT(0)) && !port_lag_members_set(0, PORT_BIT(0) | PORT_BIT(4))
	      && !port_lag_members_set(1, PORT_BIT(3) | PORT_BIT(4)) && !port_lag_members_set(0, PMASK_CPU)
	      && !port_lag_members_set(1, PORT_BIT(8)) && !port_lag_members_set(0, 0x0100),
	      "a third group, members from the other group, the CPU port and chip ports 8, 9 are refused");
	CHECK(other_writes == writes && regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK] == 0x03c5, "nothing was written");

	CHECK(port_lag_members_set(0, 0) && regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK] == 0x03c0
	      && port_lag_members_get(0) == 0 && port_lag_members_get(1) == 0x00c0 && port_lag_of(0) == PORT_LAG_NONE,
	      "an empty set clears one group only");
	CHECK(port_lag_members_set(0, PORT_BIT(1) | PORT_BIT(3)), "LAG 1 again");

	CHECK(port_lag_hash_set(1, LAG_HASH_L2_SMAC | LAG_HASH_L4_DPORT) && regfile[RTL836X_REG_PORT_TRUNK_CTRL] == 0x01c2,
	      "the hash takes bits 6:0, SPA smac dmac sip dip sport dport, and leaves bits 8:7");
	CHECK(port_lag_hash_get(0) == 0x42 && port_lag_hash_get(1) == 0x42, "and is the same for both groups");
	CHECK(port_lag_hash_set(0, LAG_HASH_SOURCE_PORT_NUMBER | LAG_HASH_L3_DIP) && port_lag_hash_get(1) == 0x11,
	      "whichever group names it");
	writes = other_writes;
	CHECK(!port_lag_hash_set(2, LAG_HASH_L2_SMAC) && !port_lag_hash_set(0, 0x80) && !port_lag_hash_set(0, 0xff)
	      && other_writes == writes && port_lag_hash_get(0) == 0x11, "a third group and bits above 6 are refused untouched");
	CHECK(port_lag_members_set(0, 0) && port_lag_members_set(0, PORT_BIT(1) | PORT_BIT(3)) && port_lag_hash_get(0) == 0x11
	      && regfile[RTL836X_REG_PORT_TRUNK_CTRL] == 0x0191, "a hash already set survives a LAG being created again");

	send_lag();
	CHECK(has("[{\"lagNum\":0,\"members\":\"0000000000001010\",\"hash\":\"00000011\"},"
		  "{\"lagNum\":1,\"members\":\"0000000011000000\",\"hash\":\"00000011\"}]") && !has("\"lagNum\":2")
	      && balanced(), "/lag.json: the two groups, the global hash twice");

	port_pvid_set(1, 1);
	port_pvid_set(3, 1);
	port_pvid_set(2, 1);
	port_pvid_set(3, 10);
	CHECK(port_pvid_get(1) == 10 && port_pvid_get(3) == 10 && port_pvid_get(2) == 1,
	      "a PVID for one member goes to every member of its group");
	port_pvid_set(2, 20);
	CHECK(port_pvid_get(2) == 20 && port_pvid_get(1) == 10 && port_pvid_get(5) != 20,
	      "and a port in no group keeps its own");

	/* LACP state, as the engine leaves it with group 2 negotiated and group 1 idle */
	lacpEnabled = 1;
	lacp_lag_ports[1] = 0x00c0;
	lacp_agg_valid[1] = 1;
	memcpy(lacp_agg_sys[1], "\x02\xbb\xbb\xbb\xbb\x02", MAC_ADDR_LEN);
	lacp_members_last[1] = 0x0040;
	send_lacp();
	unsigned lacp_groups = 0;
	for (const char *p = body(); (p = strstr(p, "\"cfg\":")); p++)
		lacp_groups++;
	CHECK(lacp_groups == PORT_LAG_COUNT && lacp_groups == 2 && balanced()
	      && has("\"lags\":[{\"cfg\":\"0000\",\"aggValid\":0,")
	      && has("{\"cfg\":\"00c0\",\"aggValid\":1,\"agg\":\"02bbbbbbbb02\",\"members\":\"0040\"}],\"ports\":["),
	      "/lacp.json: the chip's two groups with their candidates, aggregator and active members");
}

static void t_l2(void)
{
	const uint8_t a[6] = { 2, 0x11, 0x22, 0x33, 0x44, 1 };
	const uint8_t b[6] = { 2, 0x11, 0x22, 0x33, 0x44, 2 };
	printf("[test] native LUT walk, exact deletion, management protection and busy refusal\n");
	memset(lut, 0, sizeof lut);
	regfile[0x08d0] = 0;
	lut_put(0, 0x0010, a, 0x123, 2, false);
	lut_put(1, 0x0815, b, 2, 7, false);
	lut[1].words[3] |= 0x2000; /* IVL */
	lut[1].words[4] |= 0x004b; /* FID/EFID must survive the deletion write */
	lut_put(2, 0x103f, uip_ethaddr.addr, 1, 10, true);
	lut[2].words[4] &= ~0x3800; /* static entries need no dynamic age */
	send_l2(0);
	CHECK(has("\"vlan\":\"123\",\"mac\":\"02:11:22:33:44:01\",\"type\":\"l\",\"port\":2,\"lag\":0,\"idx\":\"0010\""),
	      "native words decode to the shared row contract");
	CHECK(has("\"idx\":\"0815\"") && has("\"type\":\"s\",\"port\":9,\"lag\":0,\"idx\":\"103f\""),
	      "extended SRAM/CAM indices and CPU chip10 to logical9 are retained");
	CHECK(balanced() && slen <= TCP_OUTBUF_SIZE, "native rows fit a valid JSON page");
	regfile[0x08d0] = 4;
	send_l2(0);
	CHECK(has("\"port\":2,\"lag\":1,"), "the learned port is associated with its native LAG");
	regfile[0x08d0] = 0;
	l2_delete(0x0814);
	CHECK(has("\"result\":0") && lut[1].present, "an absent exact index never deletes the following row");
	l2_delete(0x2010);
	CHECK(has("\"result\":0") && lut[0].present, "out-of-range requests do not alias an SRAM index");
	l2_delete(0x103f);
	CHECK(has("\"result\":0") && lut[2].present, "the switch's management address cannot be removed");
	l2_delete(0x0815);
	CHECK(has("\"result\":1") && !lut[1].present && lut[0].present && lut[2].present,
	      "deleting a high index preserves its full key and removes only that entry");
	lut_stuck = true; lut_commands = lut_status_reads = 0;
	send_l2(0);
	CHECK(!strcmp(body(), "null") && lut_status_reads <= 256, "busy lookup reports unavailable, not an empty table");
	l2_delete(0x0010);
	CHECK(has("\"result\":0") && lut[0].present && lut_commands == 0 && lut_status_reads <= 512,
	      "busy deletion is bounded and leaves the table untouched");
	lut_stuck = false;
	lut[0].words[4] &= ~0x3800;
	l2_delete(0x0010);
	CHECK(has("\"result\":0") && lut[0].present,
	      "an address HIT with neither age nor static is inactive, not deletable");
	lut_put(0, 0x0010, a, 0x123, 2, false);
	lut_timeout_command = 0x0034; lut_commands = lut_status_reads = 0;
	send_l2(0);
	CHECK(!strcmp(body(), "null") && lut_status_reads <= 257 && lut_commands == 1,
	      "lookup completion timeout is bounded and never returns stale read data");
	lut_timeout_command = 0; regfile[0x0502] = 0;
	lut_timeout_command = 0x000c; lut_commands = lut_status_reads = 0;
	l2_delete(0x0010);
	CHECK(has("\"result\":0") && lut_status_reads <= 258 && lut_commands == 2 && lut[2].present,
	      "write completion timeout is refused without retrying or touching management");
	lut_timeout_command = 0; regfile[0x0502] = 0;
	lut_write_miss = true;
	l2_delete(0x0010);
	CHECK(has("\"result\":0") && lut[0].present, "a completed write without HIT is not reported as success");
	lut_write_miss = false;
	memset(lut, 0, sizeof lut);
	for (unsigned i = 0; i < 40; i++) {
		uint8_t mac[6] = { 2, 1, 2, 3, 4, i };
		lut_put(i, 1 + i * 65, mac, 1, i % 8, false);
	}
	uint8_t seen[4096] = { 0 };
	unsigned idx = 0, unique = 0, pages = 0;
	bool wrapped = false;
	do {
		send_l2(idx); pages++;
		CHECK(balanced() && slen <= TCP_OUTBUF_SIZE, "every native pagination response fits");
		for (const char *p = body(); (p = strstr(p, "\"idx\":\"")); p += 7) {
			idx = strtoul(p + 7, NULL, 16);
			if (idx >= sizeof seen || seen[idx]) { wrapped = true; break; }
			seen[idx] = 1; unique++; idx++;
		}
	} while (!wrapped && pages < 10);
	CHECK(wrapped && unique == 40, "client-style pagination finds every MAC once and terminates");
	memset(lut, 0, sizeof lut);
	send_l2(0);
	CHECK(!strcmp(body(), "[]"), "an empty native table returns an empty array");
}

static void t_rate_json(void)
{
	printf("[test] native bandwidth counts and byte-rate storm meters on shared JSON endpoints\n");
	for (unsigned p = 0; p < 8; p++) {
		regfile[0x000f + 32 * p] = regfile[0x038c + 2 * p] = 0xffff;
		regfile[0x0010 + 32 * p] = regfile[0x038d + 2 * p] = 0xa5f9;
	}
	regfile[0x000f + 32 * 2] = 1;
	regfile[0x0010 + 32 * 2] = 0xa5f8;
	regfile[0x000e + 32 * 2] |= 0x0800;
	regfile[0x038c + 2 * 2] = 0xe848;
	regfile[0x038d + 2 * 2] = 0xa5f9;
	send_bandwidth();
	CHECK(port_has(2, "\"iLimited\":1,\"iBW\":\"000001\",\"iFC\":1,\"eLimited\":1,\"eBW\":\"01e848\""),
	      "8-kbps ingress and 1-Gbps egress retain native count precision and flow-control mode");
	CHECK(port_has(0, "\"iLimited\":0,\"iBW\":\"01ffff\""), "the documented native unlimited count is reported as off");
	CHECK(port_has(7, "\"eLimited\":0,\"eBW\":\"01ffff\"") && balanced() && slen <= TCP_OUTBUF_SIZE,
	      "all eight bandwidth rows fit valid JSON");
	for (unsigned t = 0; t < 4; t++)
		regfile[0x0a40 + t] = 0;
	regfile[0x0a40] = 4;
	regfile[0x0a45] = 32;
	regfile[0x1740] = 1; regfile[0x1741] = 0;
	regfile[0x0a43] = 0x0080;
	regfile[0x0a5f] = 55 << 8;
	regfile[0x176e] = 0xe848; regfile[0x176f] = 1;
	send_storm();
	CHECK(port_has(2, "\"en\":\"1000\",\"pps\":\"0000\",\"rate\":\"000008000000000000000000\""),
	      "broadcast reports 8 kbit/s, not its hardware count, and never claims PPS support");
	CHECK(port_has(7, "\"en\":\"0001\",\"pps\":\"0000\",\"rate\":\"0000000000000000000f4240\""),
	      "the last native meter reports a 1-Gbit/s unknown-multicast limit");
	CHECK(balanced() && slen <= TCP_OUTBUF_SIZE, "native storm rows fit the response buffer");
}

static void t_rate_backend(void)
{
	const unsigned rma[] = { 0x0800, 0x0801, 0x0802, 0x080e, 0x0832 };
	const uint32_t invalid[] = { 0, 7, 9, 1000008, UINT32_MAX };
	printf("[test] native rate limits preserve register neighbors, calibration and control frames\n");
	for (unsigned p = 0; p < 8; p++) {
		regfile[0x000f + 32 * p] = regfile[0x038c + 2 * p] = 0x1234;
		regfile[0x0010 + 32 * p] = regfile[0x038d + 2 * p] = 0xa5ff;
		regfile[0x000e + 32 * p] = 0xace5;
	}
	regfile[0x03a0] = 0x1357; /* chip CPU port 10's egress count is not ours to configure */
	for (unsigned t = 0; t < 4; t++) regfile[0x0a40 + t] = 0xffff;
	for (unsigned i = 0; i < sizeof rma / sizeof rma[0]; i++) regfile[rma[i]] = 0xa5a5;
	regfile[0x1214] = 0xa5a0; regfile[0x0300] = 0xa5a5; regfile[0x1c00] = 0x9a55;
	regfile[0x1618] = 0xbfff; regfile[0x17a7] = 0xa55a;
	bandwidth_setup();
	for (unsigned p = 0; p < 8; p++)
		CHECK(rtl836x_bw_get(p, 1) == 0x1ffff && rtl836x_bw_get(p, 0) == 0x1ffff &&
		      regfile[0x0010 + 32 * p] == 0xa5f9 && regfile[0x000e + 32 * p] == 0xa4e5,
		      "warm initialization removes both limits, preserves reserved bits and resets only the exceed mode");
	for (unsigned t = 0; t < 4; t++)
		CHECK(regfile[0x0a40 + t] == 0xfb00, "storm state resets on front ports and chip CPU10, leaving unused ports alone");
	for (unsigned i = 0; i < sizeof rma / sizeof rma[0]; i++)
		CHECK(regfile[rma[i]] == 0xa5e5, "control-frame storm bypass preserves every action and unrelated RMA bit");
	CHECK(regfile[0x1214] == 0xa5a4 && regfile[0x0300] == 0xa5a4 && regfile[0x1c00] == 0xba55 &&
	      regfile[0x03a0] == 0x1357, "accounting/bypass fields change without imposing a CPU rate");
	bandwidth_ingress_set(0, 8);
	CHECK(rtl836x_bw_get(0, 1) == 1 && regfile[0x000e] == 0xa8e5,
	      "minimum ingress limit uses one token, enables pause-on-exceed and excludes IFG");
	bandwidth_ingress_drop(0);
	CHECK(regfile[0x000e] == 0xa0e5, "drop changes only the ingress exceed mode");
	bandwidth_ingress_fc(0);
	bandwidth_egress_set(7, 524288);
	CHECK(rtl836x_bw_get(7, 0) == 65536 && regfile[0x039a] == 0 && regfile[0x039b] == 0xa5f9,
	      "egress retains bit16 when the low word is zero");
	bandwidth_egress_set(7, 1000000);
	CHECK(rtl836x_bw_get(7, 0) == 125000, "the maximum front-port rate uses the documented 8-kbit units");
	regfile[0x0a44] = 0xa5c0; regfile[0x0a5f] = 0xc0a5;
	regfile[0x1713] = regfile[0x17b5] = 0xffff;
	regfile[0x1440] = regfile[0x1780] = 0xa558;
	regfile[0x1441] = regfile[0x1781] = 0xffff;
	regfile[0x176f] = 0xf8ff;
	storm_set(0, 0, 8, 0);
	storm_set(7, 3, 1000000, 0);
	CHECK(regfile[0x0a44] == 0xa5d8 && regfile[0x0a5f] == 0xf7a5 &&
	      regfile[0x1713] == 0xfeff && regfile[0x17b5] == 0xff7f && regfile[0x176f] == 0xf8f9,
	      "both meter banks and packed selectors preserve adjacent ports, flags and high bits");
	CHECK(regfile[0x1441] == 0xfeff && regfile[0x1781] == 0xff7f &&
	      regfile[0x1440] == 0xa558 && regfile[0x1780] == 0xa558,
	      "kbps storm limits replace retained PPS mode in both banks without changing adjacent meters");
	CHECK(rtl836x_storm_get(0, 0) == 8 && rtl836x_storm_get(7, 3) == 1000000 &&
	      regfile[0x1618] == 0xbfff && regfile[0x17a7] == 0xa55a, "storm readback is kbit/s and retains bucket calibration");
	regfile[0x0a44] = (regfile[0x0a44] & ~0x3f) | 1;
	regfile[0x1402] = 2; regfile[0x1403] = 0;
	CHECK(rtl836x_storm_get(0, 0) == 16, "readback follows the actual ASIC selector, not a cached allocation");
	unsigned before = other_writes;
	for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
		err_status = ERR_OK; bandwidth_ingress_set(2, invalid[i]);
		CHECK(err_status == ERR_INVALID_ARGUMENT && other_writes == before, "unsupported rates are rejected before any write");
	}
	bandwidth_egress_set(8, 8); bandwidth_egress_set(9, 8);
	storm_set(2, 4, 8, 0); storm_set(2, 0, 8, 1); storm_off(9, 0);
	CHECK(other_writes == before && rtl836x_bw_get(9, 1) == UINT32_MAX &&
	      rtl836x_storm_get(2, 4) == UINT32_MAX, "CPU/unmapped ports, unknown types and PPS never reach register writes");
	storm_off(0, 0);
	CHECK(!(regfile[0x0a40] & 1) && (regfile[0x0a43] & 0x80), "disabling one storm class leaves the other port/type enabled");
	bandwidth_ingress_disable(0); bandwidth_egress_disable(7);
	CHECK(rtl836x_bw_get(0, 1) == 0x1ffff && rtl836x_bw_get(7, 0) == 0x1ffff,
	      "off restores unlimited native counts");
	err_status = ERR_OK;
}

static void t_unported(void)
{
	printf("[test] pages of unported features answer with valid JSON\n");
	send_lldp();		CHECK(balanced(), "/lldp.json");
	send_stp();		CHECK(balanced(), "/stp.json");
	send_stp_counters();	CHECK(balanced(), "/stpcnt.json");
	extern uint8_t stp_test_no_entities;
	stp_test_no_entities = 1;
	send_stp();		CHECK(balanced() && has("\"ports\":[]"), "/stp.json without entities");
	send_stp_counters();	CHECK(balanced() && has("\"ports\":[]"), "/stpcnt.json without entities");
}

static void t_snmp_hw(void)
{
	struct snmp_counter c;
	uint32_t links;
	uint16_t mbps;
	uint8_t admin;
	printf("[test] snmp_hw: Prometheus counters, link, speed and admin state from native registers\n");
	mib_set(2, 0, UINT64_C(0x100000007));
	CHECK(snmp_hw_stat(2, 0, true, true, &c) && value64(c.bytes) == UINT64_C(0x100000007),
	      "RX bytes are the native 64-bit ifInOctets");
	mib_set(2, 1, 42);
	CHECK(snmp_hw_stat(2, 15, false, true, &c) && value64(c.bytes) == 42, "RX CRC errors are the native FCS errors");
	mib_set(2, 34, 5);
	CHECK(snmp_hw_stat(2, 11, true, false, &c) && value64(c.bytes) == 5, "each word of a pair has its own native counter");
	memset(&c, 0xa5, sizeof(c));
	CHECK(!snmp_hw_stat(2, 14, true, false, &c) && !snmp_hw_stat(2, 21, true, false, &c) &&
	      !snmp_hw_stat(2, 8, true, true, &c) && !snmp_hw_stat(8, 0, true, true, &c) && c.bytes[0] == 0xa5,
	      "unmapped, paired-as-64-bit and CPU-port requests fail and leave the value alone");
	regfile[RTL836X_REG_PORT0_STATUS + 2] = PORT_STATUS_LINK | 1;
	CHECK(snmp_hw_link(2, &links) && links == 4 && snmp_hw_speed(2, &mbps) && mbps == 100,
	      "link bit and speed come from PORTn_STATUS");
	regfile[RTL836X_REG_PORT0_STATUS + 3] = 2;
	CHECK(snmp_hw_link(3, &links) && links == 0 && snmp_hw_speed(3, &mbps) && mbps == 0,
	      "speed bits without link give no speed");
	phy_regs[3][0] = 0x0800;
	phy_regs[2][0] = 0x1140;
	CHECK(snmp_hw_admin(3, &admin) && admin == 2 && snmp_hw_admin(2, &admin) && admin == 1,
	      "admin state is BMCR power down");
	mib_set(2, 5, 1);
	mib_set(2, 6, 2);
	CHECK(snmp_hw_counter32(2, 48, true, &c) && value64(c.bytes) == 45,
	      "ifInErrors sums FCS errors, fragments and jabbers");
	mib_set(2, 36, 7);
	mib_set(2, 32, 0xffffffff);
	CHECK(snmp_hw_counter32(2, 48, false, &c) && value64(c.bytes) == 6,
	      "ifOutErrors sums out discards and late collisions and wraps at 32 bits");
	CHECK(!snmp_hw_counter32(2, 47, true, &c), "only selector 48 is the error pair");
}

static void t_igmp_boot(void)
{
	printf("[test] native IGMP off replaces retained traps and lookup state\n");
	igmpEnabled = 1;
	regfile[0x0a30] = 0xffff;
	regfile[0x1c00] = 0xa555;
	regfile[0x08c9] = 0xffff;
	regfile[0x08ca] = 0xa55a;
	for (unsigned p = 0; p < 8; p++)
		regfile[0x1c05 + p] = 0xffff;
	regfile[0x1cb0] = 0x1234; regfile[0x1cb1] = 0x5678; regfile[0x1cb2] = 0xffff;
	igmp_setup();
	CHECK(!igmpEnabled && regfile[0x0a30] == 0xffef && regfile[0x1c00] == 0xa554,
	      "off disables IPv4 lookup and hardware snooping without changing unrelated global fields");
	CHECK(regfile[0x08c9] == 0 && regfile[0x08ca] == 0xa54a,
	      "off floods unknown IPv4 multicast on front ports and CPU10, preserving unused ports");
	for (unsigned p = 0; p < 8; p++)
		CHECK(regfile[0x1c05 + p] == 0xfd55, "off forwards IGMP/MLD and permits control/data on each front port");
	CHECK(regfile[0x1cb0] == 0x1234 && regfile[0x1cb1] == 0x5678 && regfile[0x1cb2] == 0xfd55,
	      "CPU10 control packets forward normally; unused chip ports retain their settings");
}

static void t_igmp_controls(void)
{
	printf("[test] native software IGMP traps IPv4 only and retains non-IGMP fields\n");
	regfile[0x0a3a] = 0xa55b;
	for (unsigned i = 0; i < 4; i++) regfile[0x0e87 + i] = 0xffff;
	igmp_enable();
	CHECK(igmpEnabled && regfile[0x0a30] == 0xffff && regfile[0x0a3a] == 0xa55a &&
	      regfile[0x1c00] == 0xa555, "on enables DIP-only lookup and IGMP processing without losing other global fields");
	CHECK(regfile[0x08c9] == 0x5555 && regfile[0x08ca] == 0xa54a,
	      "unknown IPv4 groups are dropped on front ingress, not on CPU10 or unused chip ports");
	for (unsigned p = 0; p < 8; p++)
		CHECK(regfile[0x1c05 + p] == 0xfd6a, "IPv4 reports trap to software while MLD continues flooding");
	CHECK(regfile[0x1cb2] == 0xfd55 && regfile[0x1cb0] == 0x1234 && regfile[0x1cb1] == 0x5678,
	      "enabling snooping does not trap the CPU or change unused port controls");
	CHECK(!(regfile[0x0e87] | regfile[0x0e88] | regfile[0x0e89] | regfile[0x0e8a]),
	      "retained source-specific classification cannot redirect DIP-only software groups");
	regfile[0x1c04] = 0xf800;
	igmp_router_port_set(0x0209);
	CHECK(regfile[0x1c04] == 0xfc09, "router masks translate logical CPU9 to chip CPU10 and preserve reserved bits");
	unsigned writes = other_writes;
	err_status = ERR_OK; igmp_router_port_set(0x0100);
	CHECK(err_status == ERR_INVALID_ARGUMENT && other_writes == writes, "unmapped router ports are refused before writes");
	err_status = ERR_OK;
	igmp_setup();
	CHECK(!igmpEnabled && regfile[0x08c9] == 0 && !(regfile[0x0a30] & 0x10),
	      "off restores ordinary IPv4 multicast forwarding after on");
}

static void t_igmp_lut(void)
{
	uint8_t group[4] = {239, 1, 2, 3}, other[4] = {239, 255, 200, 201}, bad[4] = {192, 0, 2, 1};
	const uint8_t uc[6] = {2, 1, 2, 3, 4, 5};
	uint16_t saved_uc[6];
	printf("[test] native IPv4 multicast membership, key isolation and unavailable-table refusal\n");
	memset(lut, 0, sizeof lut);
	lut_stuck = lut_write_miss = false; lut_timeout_command = 0; regfile[0x0502] = 0;
	lut_put(0, 123, uc, 1, 10, true); memcpy(saved_uc, lut[0].words, sizeof saved_uc);
	CHECK(rtl836x_igmp_update(group, 2, true) && lut[1].present &&
	      lut[1].words[0] == 0 && lut[1].words[1] == 0 && lut[1].words[2] == 0x0203 &&
	      lut[1].words[3] == 0x1f01 && lut[1].words[4] == 4 && lut[1].words[5] == 0x0020,
	      "a join installs the full DIP with SIP zero and only its requested front port");
	CHECK(rtl836x_igmp_update(group, 7, true) && lut[1].words[4] == 0x84,
	      "a later join preserves the first listener");
	lut[1].words[3] |= 0xc000; lut[1].words[5] |= 0x0080;
	CHECK(rtl836x_igmp_update(group, 2, false) && lut[1].words[3] == 0xdf01 &&
	      lut[1].words[4] == 0x80 && lut[1].words[5] == 0x00a0,
	      "a front-port leave preserves membership on chip ports 8, 9 and CPU10");
	lut[1].words[3] = 0x1f01; lut[1].words[4] = 0x84; lut[1].words[5] = 0x0020;
	CHECK(rtl836x_igmp_update(other, 1, true) && lut[2].words[2] == 0xc8c9 &&
	      lut[2].words[3] == 0x1fff && lut[2].words[4] == 2,
	      "another group retains high address octets and has independent membership");
	CHECK(rtl836x_igmp_update(group, 2, false) && lut[1].words[4] == 0x80 && lut[2].words[4] == 2,
	      "leave removes one listener without changing another group");
	CHECK(rtl836x_igmp_update(group, 7, false) && !lut[1].present && lut[2].present,
	      "the final leave removes only the matched multicast key");
	CHECK(rtl836x_igmp_update(group, 7, false) && !lut[1].present && lut[2].words[4] == 2,
	      "an absent leave cannot alias the following entry");
	unsigned writes = other_writes;
	CHECK(!rtl836x_igmp_update(group, 8, true) && !rtl836x_igmp_update(group, 10, true) &&
	      !rtl836x_igmp_update(bad, 2, true) && other_writes == writes,
	      "unmapped/CPU listeners and non-multicast keys never access the ASIC");
	lut_stuck = true; lut_status_reads = 0;
	CHECK(!rtl836x_igmp_update(other, 3, true) && other_writes == writes && lut_status_reads < 1024,
	      "a busy table is bounded and never stages a write");
	lut_stuck = false; lut_write_miss = true;
	CHECK(!rtl836x_igmp_update(other, 3, true) && lut[2].words[4] == 2,
	      "a refused write is not reported as a joined listener");
	lut_write_miss = false; lut_timeout_command = 0x000c; lut_status_reads = 0;
	CHECK(!rtl836x_igmp_update(other, 3, true) && lut_status_reads < 1024,
	      "write completion timeout is bounded and not reported as success");
	lut_timeout_command = 0; regfile[0x0502] = 0;
	CHECK(lut[0].present && !memcmp(lut[0].words, saved_uc, sizeof saved_uc),
	      "membership changes and failed operations preserve the management unicast entry");
	for (unsigned i = 0; i < 48; i++) {
		uint8_t mac[6] = {2, 0, 0, 0, 0, i};
		lut_put(i, 0x200 + i, mac, 1, 0, true);
	}
	CHECK(!rtl836x_igmp_update(group, 2, true) && lut[0].words[0] == 0 && lut[47].words[0] == 47,
	      "a full table is unavailable, not a fabricated successful join");
}

static void native_igmp_frame(uint8_t type, uint8_t port, uint8_t first_group_octet)
{
	memset(uip_buf, 0, sizeof uip_buf);
	uip_buf[0] = 1; uip_buf[2] = 0x5e;
	uip_buf[12] = 0x88; uip_buf[13] = 0x99; uip_buf[14] = 4; uip_buf[19] = port;
	uip_buf[20] = 0x81; uip_buf[24] = 8;
	uip_buf[26] = 0x45; uip_buf[34] = 1; uip_buf[35] = 2;
	uip_buf[46] = type;
	unsigned record = 46;
	if (type == IGMP_V3_REPORT) {
		uip_buf[53] = 1; record += 8; uip_buf[record] = IGMP_V3_TO_EXCLUDE;
	}
	uip_buf[record + 4] = first_group_octet; uip_buf[record + 5] = 1;
	uip_buf[record + 6] = 2; uip_buf[record + 7] = 3;
	uip_len = record + 8; uip_buf[29] = uip_len - 26;
}

static void t_igmp_rx(void)
{
	printf("[test] shared IGMP parser reads the native synthetic VLAN envelope\n");
	memset(lut, 0, sizeof lut); regfile[0x0502] = 0; igmpEnabled = 1;
	native_igmp_frame(IGMP_V2_REPORT, 2, 239);
	CHECK(igmp_packet_handler() == 1 && !uip_len && lut[0].present && lut[0].words[4] == 4,
	      "native IPv4 EtherType, IHL and source port reach the real membership backend");
	native_igmp_frame(IGMP_V3_REPORT, 7, 239);
	CHECK(igmp_packet_handler() == 1 && !uip_len && lut[0].words[4] == 0x84,
	      "native IGMPv3 uses the same validated first-record logic");
	native_igmp_frame(IGMP_V2_LEAVE, 2, 239);
	CHECK(igmp_packet_handler() == 1 && lut[0].words[4] == 0x80, "native leave retains the other listener");
	unsigned writes = other_writes;
	native_igmp_frame(IGMP_V2_REPORT, 2, 239); uip_len = 46;
	CHECK(igmp_packet_handler() == 1 && !uip_len && other_writes == writes && lut[0].words[4] == 0x80,
	      "truncated native payload cannot reuse the preceding report");
	native_igmp_frame(IGMP_V2_REPORT, 10, 239);
	CHECK(igmp_packet_handler() == 1 && !uip_len && other_writes == writes,
	      "a CPU-originated report cannot become a front-port listener");
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
	t_snmp_hw();
	t_vlan();
	t_isolation();
	t_mirror();
	t_mtu();
	t_lag();
	t_l2();
	t_rate_backend();
	t_rate_json();
	t_igmp_boot();
	t_igmp_controls();
	t_igmp_lut();
	t_igmp_rx();
	t_unported();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
