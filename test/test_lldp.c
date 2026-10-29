#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "uip.h"
#include "rtl837x_lldp.h"
#include "rtl837x_trap.h"

uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
struct uip_eth_addr uip_ethaddr = { .addr = {0x02,0x11,0x22,0x33,0x44,0x55} };
uip_ipaddr_t uip_hostaddr;
const struct machine machine = {
			   .log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 9} };
char hostname[24] = "rtlplayground";
uint8_t outbuf[2500];
uint16_t slen;
volatile uint32_t ticks;
bool lldp_enabled;
uint8_t sfr_data[4];

static uint32_t links_sts;
static int reg_reads, tx_frames;

void print_string(const char *s) { (void)s; }
void write_char(char c) { (void)c; }
void tcpip_output(void) { tx_frames++; }

void reg_read_m(uint16_t reg_addr)
{
	if (reg_addr == RTL837X_REG_LINKS_STS)
		reg_reads++;
	sfr_data[0] = 0;
	sfr_data[1] = links_sts;
	sfr_data[2] = links_sts >> 8;
	sfr_data[3] = 0;
}

void memcpyc(uint8_t *dst, const uint8_t *src, uint16_t len) { memcpy(dst, src, len); }
uint16_t strlen_x(const char *s) { return strlen(s); }
char port_names[9][PORT_NAME_SIZE];	/* read by lldp_portdesc() from #590 */
uint16_t strtox(uint8_t *dst, const char *s) { uint16_t n = strlen(s); memcpy(dst, s, n); return n; }
/* Read by the 802.1/802.3 organisational TLVs from #600 */
volatile uint16_t SFR_DATA_U16;
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg) { (void)phy_id; (void)dev_id; (void)reg; SFR_DATA_U16 = 0; }
uint16_t port_pvid_get(uint8_t port) { (void)port; return 1; }
uint8_t port_lag_of(uint8_t port) { (void)port; return 0xff; }

static int failures;
#define CHECK(cond, name) do { \
	if (cond) printf("PASS  %s\n", name); \
	else { printf("FAIL  %s\n", name); failures++; } \
} while (0)

static void step(int seconds)
{
	while (seconds--) {
		ticks += SYS_TICK_HZ;
		lldp_tick();
	}
}

static void put(uint8_t slot, uint8_t port, uint16_t ttl)
{
	lldp_nb[slot].port = port;
	lldp_nb[slot].ttl = ttl;
}

static const uint8_t ap_mac[] = {0x94, 0x2a, 0x6f, 0xd2, 0x25, 0x6e};
#define AP_CHASSIS LLDP_CHASSIS_ID_TLV_SUBTYPE, (const char *)ap_mac, MAC_ADDR_LEN

static bool frame(uint8_t port, uint8_t chassis_subtype, const char *chassis, uint8_t chassis_len,
		  uint8_t subtype, const char *id, uint8_t id_len)
{
	uint8_t *p = uip_buf;

	memset(uip_buf, 0, sizeof(uip_buf));
	memcpy(p, LLDP_MULTICAST_ADDR, MAC_ADDR_LEN);
	p = uip_buf + TRAP_RX_HDR_SIZE - 1;
	*p++ = port;
	*p++ = LLDP_ETHERTYPE >> 8;
	*p++ = LLDP_ETHERTYPE & 0xff;
	*p++ = LLDP_CHASSIS_ID_TLV_TYPE << 1;
	*p++ = chassis_len + 1;
	*p++ = chassis_subtype;
	memcpy(p, chassis, chassis_len);
	p += chassis_len;
	*p++ = LLDP_PORT_ID_TLV_TYPE << 1;
	*p++ = id_len + 1;
	*p++ = subtype;
	memcpy(p, id, id_len);
	p += id_len;
	*p++ = LLDP_TTL_TLV_TYPE << 1;
	*p++ = LLDP_TTL_TLV_LENGTH;
	*p++ = 0;
	*p++ = 120;
	*p++ = 0;
	*p++ = 0;
	uip_len = p - uip_buf;
	return lldp_in();
}

static int nb_count(void)
{
	uint8_t i;
	int n = 0;

	for (i = 0; i < LLDP_NB_MAX; i++)
		n += lldp_nb[i].ttl != 0;
	return n;
}

static void reset(bool enabled, uint16_t links)
{
	ticks = 0;
	lldp_init();
	lldp_enabled = enabled;
	links_sts = links;
	reg_reads = tx_frames = 0;
}

int main(void)
{
	uint8_t i;
	int busy = 0, bad = 0;
	uint16_t cut, full;

	reset(true, 0x0108);
	put(0, 3, 120);
	put(1, 8, 120);
	step(5);
	CHECK(lldp_nb[0].ttl == 115 && lldp_nb[1].ttl == 115 && reg_reads == 5,
	      "L1 link up on ports 3 and 8: one second off the TTL per step");

	links_sts = 0x0100;
	step(1);
	CHECK(lldp_nb[0].ttl == 0 && lldp_nb[1].ttl == 114,
	      "L2 port 3 loses its link: its entry is freed, port 8 keeps counting");

	links_sts = 0x0008;
	step(1);
	CHECK(lldp_nb[1].ttl == 0, "L3 port 8 loses its link: the second byte of the mask is read");

	reset(true, 0x0001);
	put(0, 0, 2);
	put(1, 0, 0);
	step(2);
	CHECK(lldp_nb[0].ttl == 0 && lldp_nb[1].ttl == 0, "L4 TTL runs out with the link up, a free slot stays free");

	reset(false, 0x01ff);
	put(0, 3, 120);
	step(LLDP_TX_INTERVAL_S + LLDP_TX_INTERVAL_S / 2);
	CHECK(reg_reads == 0 && tx_frames == 0 && lldp_nb[0].ttl == 120,
	      "L5 LLDP off: no link read, no ageing, nothing sent");

	lldp_enabled = true;
	step(LLDP_TX_INTERVAL_S - 1);
	CHECK(tx_frames == 0, "L6 after lldp on, nothing is sent before the interval");
	step(1);
	CHECK(tx_frames > 0, "L7 the first transmission comes one interval after lldp on");

	for (i = 0; i < LLDP_NB_MAX; i++)
		put(i, 3, 120);
	lldp_init();
	for (i = 0; i < LLDP_NB_MAX; i++)
		busy += lldp_nb[i].ttl != 0;
	CHECK(busy == 0, "L8 lldp off frees every slot of the table");

	reset(true, 0x01ff);
	frame(7, LLDP_CHASSIS_ID_TLV_SUBTYPE, (const char *)ap_mac, MAC_ADDR_LEN, LLDP_PORT_ID_SUBTYPE_MAC, (const char *)ap_mac, MAC_ADDR_LEN);
	slen = 0;
	lldp_neighbors_to_html();
	outbuf[slen] = 0;
	CHECK(lldp_nb[0].port == 7 && strstr((char *)outbuf, "\"portId\":\"94:2a:6f:d2:25:6e\""),
	      "L9 a MAC address port ID is shown as aa:bb:cc:dd:ee:ff");
	CHECK(frame(8, LLDP_CHASSIS_ID_TLV_SUBTYPE, (const char *)ap_mac, MAC_ADDR_LEN, LLDP_PORT_ID_TLV_SUBTYPE, "GigabitEthernet1/0/24", 21)
	      && !strcmp(lldp_nb[1].port_id, "GigabitEther") && !lldp_nb[1].port_id_mac,
	      "L10 an interface name port ID is cut at 12 characters");
	CHECK(frame(6, LLDP_CHASSIS_ID_TLV_SUBTYPE, (const char *)ap_mac, MAC_ADDR_LEN, LLDP_PORT_ID_TLV_SUBTYPE, "\x94*o\xd2%n", MAC_ADDR_LEN)
	      && !strcmp(lldp_nb[2].port_id, ".*o.%n"),
	      "L11 six bytes with another subtype stay text with dots for the unprintable");
	CHECK(frame(5, LLDP_PORT_ID_TLV_SUBTYPE, "94:2a:6f:d2:25:6e", 17, LLDP_PORT_ID_TLV_SUBTYPE, "eth0", 4)
	      && lldp_nb[3].chassis_len == 17 && !memcmp(lldp_nb[3].chassis, "94:2a:6f:d2:25:6e", 17),
	      "L12 a MAC address sent as text is kept whole as the chassis ID");

	reset(true, 0x01ff);
	frame(1, AP_CHASSIS, LLDP_PORT_ID_TLV_SUBTYPE, "", 0);
	frame(2, AP_CHASSIS, 0, "abc", 3);
	frame(3, AP_CHASSIS, 8, "abc", 3);
	frame(4, AP_CHASSIS, LLDP_PORT_ID_SUBTYPE_MAC, (const char *)ap_mac, MAC_ADDR_LEN - 1);
	frame(5, AP_CHASSIS, LLDP_PORT_ID_SUBTYPE_MAC, "\x94\x2a\x6f\xd2\x25\x6e\x00", MAC_ADDR_LEN + 1);
	CHECK(nb_count() == 0,
	      "L13 a Port ID with no octets, a reserved subtype or a MAC address of another length is ignored");
	frame(1, AP_CHASSIS, LLDP_PORT_ID_TLV_SUBTYPE, "ge1", 3);
	CHECK(nb_count() == 1, "L14 the same frame with a valid Port ID is kept");

	/* The only cuts that may leave a neighbour are the TLV boundaries after the Port ID and the TTL. */
	full = uip_len;
	for (cut = 0; cut < full; cut++) {
		lldp_init();
		uip_len = cut;
		lldp_in();
		if (nb_count() && cut != full - 6 && cut != full - 2)
			bad++;
	}
	CHECK(!bad, "L15 a frame cut inside a TLV or inside the header publishes no neighbour");

	printf("\n%s (%d failure%s)\n", failures ? "SANDBOX: FAILURES" : "SANDBOX: ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
