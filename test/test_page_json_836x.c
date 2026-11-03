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
#include "page_impl.h"
#include "support.h"

extern uint8_t  outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;

uint8_t sfr_data[4];
static uint16_t regfile[0x4000];
static uint16_t phy_regs[8][32];

void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
	(void)dev_id;
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
	SFR_DATA_8 = regfile[addr] >> 8;
	SFR_DATA_0 = regfile[addr];
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

static void t_status(void)
{
	printf("[test] /status.json: link and speed from PORTn_STATUS\n");
	regfile[RTL836X_REG_PORT0_STATUS + 0] = PORT_STATUS_LINK | 2;	/* 1000M */
	regfile[RTL836X_REG_PORT0_STATUS + 1] = PORT_STATUS_LINK | 1;	/* 100M */
	regfile[RTL836X_REG_PORT0_STATUS + 2] = PORT_STATUS_LINK;	/* 10M */
	regfile[RTL836X_REG_PORT0_STATUS + 3] = 2;			/* down */
	phy_regs[0][0] = 0x0800; /* powered down */
	phy_regs[1][4] = 0x01e0; /* 100F/H and 10F/H */
	phy_regs[1][9] = 0x0200; /* 1000F */
	send_status();
	CHECK(port_has(0, "\"link\":3,") && port_has(0, "\"enabled\":0,"), "1000M is LINKS[3], BMCR power down is disabled");
	CHECK(port_has(1, "\"link\":2,") && port_has(1, "\"enabled\":1,"), "100M is LINKS[2], powered up is enabled");
	CHECK(port_has(1, "\"adv\":\"011111\""), "MII ANAR and GBCR advertise the supported speeds");
	CHECK(port_has(2, "\"link\":1,"), "10M is LINKS[1]");
	CHECK(port_has(3, "\"link\":0,"), "link bit clear is down, even with speed bits set");
	CHECK(has("\"logPort\":7,") && !has("\"logPort\":8,"), "8 ports");
	CHECK(has("\"txG\":\"0x0\"") && !has("\"0x\""), "counters read 0, never an empty hex number");
	CHECK(slen <= TCP_OUTBUF_SIZE, "fits the buffer");
	CHECK(balanced(), "valid JSON");
}

static void t_info(void)
{
	printf("[test] /information.json without a temperature\n");
	send_basic_info();
	CHECK(has("\"chip_temp\":\"n/a\""), "temperature n/a");
	CHECK(balanced(), "valid JSON");
}

static void t_unported(void)
{
	printf("[test] pages of unported features answer with valid JSON\n");
	send_vlan(1);		CHECK(balanced(), "/vlan.json");
	send_vlanlist();	CHECK(balanced() && has("\"vlan\":[]"), "/vlanlist");
	CHECK(!send_counters(1) && balanced(), "/counters.json");
	send_l2(0);		CHECK(balanced(), "/l2.json");
	l2_delete(0);		CHECK(balanced(), "/l2_del.json");
	send_mirror();		CHECK(balanced(), "/mirror.json");
	send_lag();		CHECK(balanced(), "/lag.json");
	send_eee();		CHECK(balanced(), "/eee.json");
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
	t_status();
	t_info();
	t_unported();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
