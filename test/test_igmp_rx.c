/* Production IGMP parser; only the ASIC register edge is simulated. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_igmp.h"
#include "uip.h"
#include "uip_arp.h"
#include <test/hw_mock.h>

uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
void print_string(const char *s) { (void)s; }
void print_short(uint16_t v) { (void)v; }
void print_sfr_data(void) {}
void write_char(char c) { (void)c; }

static int failures, checks;
#define CHECK(c, name) do { checks++; if (!(c)) { printf("FAIL %s\n", name); failures++; } } while (0)
/* Trapped IGMP has the RTL CPU tag but no VLAN tag. */
#define IP (ETHER_HEADER_SIZE + RTL_TAG_SIZE)
static unsigned igmp;
static void be16(unsigned p, unsigned v) { uip_buf[p] = v >> 8; uip_buf[p + 1] = v; }
static void total(unsigned n) { be16(IP + 2, n); }
static void packet(uint8_t type, unsigned ihl)
{
	memset(uip_buf, 0, sizeof uip_buf);
	uip_buf[0] = 1; uip_buf[2] = 0x5e;
	be16(IP - 2, UIP_ETHTYPE_IP);
	/* RTL CPU tag: ingress port 2, in network order. */
	be16(12 + offsetof(struct rtl_tag, pmask), 2);
	uip_buf[IP] = 0x40 | ihl;
	uip_buf[IP + 8] = 1; uip_buf[IP + 9] = IGMP_PROTO;
	igmp = IP + ihl * 4;
	uip_buf[igmp] = type;
	unsigned record = igmp;
	if (type == IGMP_V3_REPORT) {
		be16(igmp + 6, 1);
		record += 8; uip_buf[record] = IGMP_V3_TO_EXCLUDE;
	}
	uip_buf[record + 4] = 239; uip_buf[record + 5] = 1;
	uip_buf[record + 6] = 2; uip_buf[record + 7] = 3;
	uip_len = record + 8;
	total(uip_len - IP);
}
static void rejected(const char *name)
{
	const uint8_t mac[] = {2, 0, 0, 0, 0, 1};
	hw_reset(); hw_l2_put(123, mac, 1, 3, false);
	unsigned received = uip_len;
	unsigned handled = igmp_packet_handler();
	unsigned recognized = received >= IP + 20 && received <= sizeof uip_buf;
	CHECK(!hw_reads && !hw_writes && hw_l2_count() == 1 && hw_l2_find(mac, 1) &&
	      handled == recognized && uip_len == (recognized ? 0 : received), name);
}
static void joined(const char *name)
{
	hw_reset();
	CHECK(igmp_packet_handler() == 1 && uip_len == 0 && hw_l2_count() == 1 &&
	      hw_l2_at(0)->b == 0x1f010203 && hw_l2_at(0)->c == 1, name);
}
int main(void)
{
	packet(IGMP_V2_REPORT, 5); joined("valid v2 report installs the requested group and ingress port");
	packet(IGMP_V2_LEAVE, 5);
	CHECK(igmp_packet_handler() == 1 && hw_l2_count() == 0, "valid leave removes the existing group");
	packet(IGMP_V1_REPORT, 5); joined("valid v1 report remains supported");
	packet(IGMP_V3_REPORT, 6); joined("valid v3 report with IPv4 options joins its group");
	packet(IGMP_V3_REPORT, 5);
	be16(igmp + 10, 2); uip_buf[igmp + 9] = 1;
	uip_len += 12; total(uip_len - IP);
	joined("complete source and auxiliary data remain supported");
	packet(IGMP_V3_REPORT, 5);
	be16(igmp + 6, 2); uip_buf[igmp + 16] = IGMP_V3_TO_EXCLUDE;
	uip_buf[igmp + 20] = 239; uip_buf[igmp + 23] = 4;
	uip_len += 8; total(uip_len - IP);
	joined("complete multiple records retain the existing first-record behavior");

	packet(IGMP_V2_REPORT, 5); uip_len = IP - 1; rejected("truncated link header ignores stale tail");
	packet(IGMP_V2_REPORT, 5); uip_len = IP + 10; rejected("truncated IPv4 header ignores stale protocol/report");
	packet(IGMP_V2_REPORT, 5); uip_len = IP + 20; rejected("absent v2 payload ignores stale report");
	packet(IGMP_V2_REPORT, 5); uip_len--; rejected("truncated v2 group address cannot update hardware");
	packet(IGMP_V2_REPORT, 5); total(20); rejected("Ethernet padding cannot substitute for an IPv4 payload");
	packet(IGMP_V2_REPORT, 5); total(29); rejected("IPv4 total length cannot exceed received data");
	packet(IGMP_V2_REPORT, 5); uip_buf[IP] = 0x44; rejected("IHL below five is refused");
	packet(IGMP_V2_REPORT, 5); uip_buf[IP] = 0x4f; rejected("missing IPv4 options are refused");
	packet(IGMP_V2_REPORT, 5); uip_buf[IP] = 0x65; rejected("non-IPv4 version is refused");
	packet(IGMP_V2_REPORT, 5); be16(IP + 6, 0x2000); rejected("first fragment is not a complete IGMP message");
	packet(IGMP_V2_REPORT, 5); be16(IP + 6, 1); rejected("later fragment is not interpreted as a report");
	packet(IGMP_V2_REPORT, 5); uip_len = sizeof uip_buf + 1; rejected("oversized received length is refused");

	packet(IGMP_V3_REPORT, 5); uip_len = igmp + 8; total(uip_len - IP); rejected("missing v3 record ignores stale record bytes");
	packet(IGMP_V3_REPORT, 5); total(uip_len - IP - 1); rejected("partial v3 record cannot read Ethernet padding");
	packet(IGMP_V3_REPORT, 5); be16(igmp + 10, 1); rejected("missing source address is refused");
	packet(IGMP_V3_REPORT, 5); be16(igmp + 10, 0xffff); rejected("large source count cannot wrap the record length");
	packet(IGMP_V3_REPORT, 5); uip_buf[igmp + 9] = 1; rejected("missing auxiliary data is refused");
	packet(IGMP_V3_REPORT, 5); be16(igmp + 6, 2); rejected("later missing record prevents a partial first update");
	packet(IGMP_V3_REPORT, 5); be16(igmp + 6, 0); rejected("empty v3 report does not reuse a stale first record");
	packet(IGMP_V2_REPORT, 5); uip_buf[IP + 9] = 17; hw_reset();
	unsigned len = uip_len;
	CHECK(igmp_packet_handler() == 0 && uip_len == len && !hw_writes, "non-IGMP traffic remains available to the IP stack");
	printf("IGMP RX: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
