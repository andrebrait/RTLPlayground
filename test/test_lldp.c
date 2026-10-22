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

static char console[512];
static unsigned console_len;

void write_char(char c)
{
	if (console_len < sizeof(console) - 1)
		console[console_len++] = c;
	console[console_len] = 0;
}
void print_string(const char *s) { while (*s) write_char(*s++); }
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

/* An LLDPDU in the receive buffer: start it, append TLVs, end it. */
static void rx_start(uint8_t port, bool vlan)
{
	memcpy(uip_buf, LLDP_MULTICAST_ADDR, MAC_ADDR_LEN);
	uip_len = TRAP_RX_HDR_SIZE - 1;
	uip_buf[uip_len++] = port;
	if (vlan) {
		memcpy(uip_buf + uip_len, "\x81\x00\x00\x01", VLAN_TAG_SIZE);
		uip_len += VLAN_TAG_SIZE;
	}
	uip_buf[uip_len++] = LLDP_ETHERTYPE >> 8;
	uip_buf[uip_len++] = LLDP_ETHERTYPE & 0xff;
}

static void tlv(uint8_t type, const void *v, uint16_t len)
{
	uip_buf[uip_len++] = type << 1 | len >> 8;
	uip_buf[uip_len++] = len;
	memcpy(uip_buf + uip_len, v, len);
	uip_len += len;
}

static const uint8_t ttl120[] = {0, 120};
static const uint8_t chassis_mac[] = {LLDP_CHASSIS_ID_TLV_SUBTYPE, 0x02, 0, 0x22, 0, 0x44, 0x55};
static const uint8_t port_text[] = {LLDP_PORT_ID_TLV_SUBTYPE, 'p'};

static void ids(const uint8_t *chassis, uint16_t chassis_len, const uint8_t *port, uint16_t port_len)
{
	tlv(LLDP_CHASSIS_ID_TLV_TYPE, chassis, chassis_len);
	tlv(LLDP_PORT_ID_TLV_TYPE, port, port_len);
	tlv(LLDP_TTL_TLV_TYPE, ttl120, sizeof(ttl120));
}

static bool rx_end(void)
{
	tlv(LLDP_END_TLV_TYPE, "", 0);
	return lldp_in();
}

static const char *nb_json(void)
{
	slen = 0;
	lldp_neighbors_to_html();
	outbuf[slen] = 0;
	return (const char *)outbuf;
}

static const char *nb_shown(void)
{
	console_len = 0;
	console[0] = 0;
	lldp_show();
	return console;
}

static void test_neighbor_fields(void)
{
	static const uint8_t port_mac[] = {LLDP_PORT_ID_SUBTYPE_MAC, 0, 0x11, 0, 0xab, 0, 0xff};
	static const uint8_t dirty[] = {'N', '"', '\\', 0, 2, 0x7f, 0xff, 'Z'};
	static const char forty[] = "0123456789012345678901234567890123456789";
	int vlan;

	for (vlan = 0; vlan < 2; vlan++) {
		reset(true, 0x01ff);
		rx_start(4, vlan);
		ids(chassis_mac, sizeof(chassis_mac), port_mac, sizeof(port_mac));
		tlv(LLDP_SYSNAME_TLV_TYPE, "desk-pc", 7);
		tlv(LLDP_SYSDESC_TLV_TYPE, "Linux 6.8 x86_64", 16);
		tlv(LLDP_PORT_DESC_TLV_TYPE, "eth0", 4);
		CHECK(rx_end() && nb_count() == 1 && strstr(nb_json(),
		      "{\"port\":5,\"chassis\":\"02:00:22:00:44:55\",\"portId\":\"00:11:00:ab:00:ff\","
		      "\"sysName\":\"desk-pc\",\"sysDesc\":\"Linux 6.8 x86_64\",\"portDesc\":\"eth0\""),
		      vlan ? "N1 neighbour fields in /lldp.json, behind a VLAN tag"
			   : "N1 neighbour fields in /lldp.json, system description between name and port description");
		CHECK(strstr(nb_shown(), "00:11:00:ab:00:ff  desk-pc  Linux 6.8 x86_64  eth0  "),
		      vlan ? "N2 lldp show prints the same fields, behind a VLAN tag"
			   : "N2 lldp show prints the system description between name and port description");
	}

	reset(true, 0x01ff);
	rx_start(4, false);
	ids(chassis_mac, sizeof(chassis_mac), port_text, sizeof(port_text));
	tlv(LLDP_SYSNAME_TLV_TYPE, dirty, sizeof(dirty));
	tlv(LLDP_SYSDESC_TLV_TYPE, dirty, sizeof(dirty));
	tlv(LLDP_PORT_DESC_TLV_TYPE, dirty, sizeof(dirty));
	rx_end();
	CHECK(strstr(nb_json(), "\"sysName\":\"N......Z\",\"sysDesc\":\"N......Z\",\"portDesc\":\"N......Z\""),
	      "N3 NUL, control characters, quotes and backslashes in the text fields become dots");

	reset(true, 0x01ff);
	rx_start(4, false);
	ids(chassis_mac, sizeof(chassis_mac), port_text, sizeof(port_text));
	tlv(LLDP_SYSNAME_TLV_TYPE, forty, 40);
	tlv(LLDP_SYSDESC_TLV_TYPE, forty, 40);
	tlv(LLDP_PORT_DESC_TLV_TYPE, forty, 40);
	rx_end();
	CHECK(strstr(nb_json(), "\"sysName\":\"0123456789012345\","
		    "\"sysDesc\":\"01234567890123456789012345678901\",\"portDesc\":\"0123456789012345\""),
	      "N4 name and port description are cut at 16 characters, system description at 32");

	rx_start(4, false);
	ids(chassis_mac, sizeof(chassis_mac), port_text, sizeof(port_text));
	tlv(LLDP_SYSNAME_TLV_TYPE, "X", 1);
	rx_end();
	CHECK(nb_count() == 1 && strstr(nb_json(), "\"sysName\":\"X\",\"sysDesc\":\"\",\"portDesc\":\"\""),
	      "N5 a refresh replaces the entry, system description and port description not sent again are cleared");

	/* worst case: every field full, a MAC address Port ID, all digits at their maximum */
	reset(true, 0x01ff);
	memset(&lldp_nb[0], 'x', sizeof(lldp_nb[0]));
	lldp_nb[0].port = 3;
	lldp_nb[0].chassis_len = LLDP_NB_CHASSIS_LENGTH;
	lldp_nb[0].port_id_mac = 1;
	lldp_nb[0].sys_name[LLDP_NB_SYS_NAME_LENGTH] = 0;
	lldp_nb[0].sys_desc[LLDP_NB_SYS_DESC_LENGTH] = 0;
	lldp_nb[0].port_desc[LLDP_NB_PORT_DESC_LENGTH] = 0;
	memset(lldp_nb[0].mgmt_addr, 0xff, sizeof(lldp_nb[0].mgmt_addr));
	lldp_nb[0].caps = 0xffff;
	lldp_nb[0].ttl = 0xffff;
	/* the reply is ',"neighbors":[' + entry + ']' and the guard counts the separator too */
	CHECK(strlen(nb_json()) - strlen(",\"neighbors\":[]") + 1 <= LLDP_NB_JSON_LENGTH,
	      "N6 LLDP_NB_JSON_LENGTH holds a worst-case entry and its separator");
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

	test_neighbor_fields();

	printf("\n%s (%d failure%s)\n", failures ? "SANDBOX: FAILURES" : "SANDBOX: ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
