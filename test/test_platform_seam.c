/*
 * test_platform_seam.c - the shared page code against nothing but platform.h.
 *
 * Links cmd_parser.c, cmd_editor.c and httpd/page_impl.c with only
 * platform_mock.c, so it builds only once none of them reaches a register
 * header. See `make seam_check`.
 */
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "page_impl.h"

extern uint8_t  outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;

int main(void)
{
	send_mtu();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"mtu\":\"0x05f2\"")) {
		printf("FAIL: send_mtu: no \"mtu\":\"0x05f2\" in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_mtu reports MTU 1522 through platform.h\n");

	send_status();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"link\":3")) {
		printf("FAIL: send_status: no \"link\":3 in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_status reports link code 3 through platform.h\n");

	send_counters(1);
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"0x100000002\"")) {
		printf("FAIL: send_counters: no \"0x100000002\" in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_counters prints the 64-bit counter through platform.h\n");

	send_mirror();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"enabled\":1") ||
	    !strstr((const char *)outbuf, "\"mirror_rx\":\"0000000000000011\",\"mirror_tx\":\"0000000000000001\"")) {
		printf("FAIL: send_mirror: no enabled mirror with RX mask 0003, TX mask 0001 in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_mirror reports the mirror through platform.h\n");

	send_lag();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"hash\":\"7\"")) {
		printf("FAIL: send_lag: no \"hash\":\"7\" in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_lag reports the hash through platform.h\n");

	send_bandwidth();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"iBW\":\"0abcde\",\"iFC\":1,\"eLimited\":1,\"eBW\":\"0abcde\"")) {
		printf("FAIL: send_bandwidth: no \"iBW\":\"0abcde\" in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_bandwidth reports the limits through platform.h\n");

	send_storm();
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "\"en\":\"1010\",\"pps\":\"1100\",\"rate\":\"000123000124000125000126\"")) {
		printf("FAIL: send_storm: no storm entry in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_storm reports the storm controls through platform.h\n");

	send_vlan(10);
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "{\"members\":\"0x2001c15\"")) {
		printf("FAIL: send_vlan: no \"members\":\"0x2001c15\" in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_vlan reports the VLAN entry through platform.h\n");

	send_l2(0);
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "[{\"vlan\":\"001\",\"mac\":\"00:11:22:33:44:01\",\"type\":\"l\",\"port\":5,")
	    || !strstr((const char *)outbuf, "{\"vlan\":\"00a\",\"mac\":\"00:11:22:33:44:02\",\"type\":\"s\",\"port\":9,")
	    || !strstr((const char *)outbuf, "\"idx\":\"0020\"}")) {
		printf("FAIL: send_l2: entries not in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: send_l2 lists the entries through platform.h\n");

	l2_delete(0x010);
	outbuf[slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE - 1] = 0;
	if (!strstr((const char *)outbuf, "{\"result\":1}")) {
		printf("FAIL: l2_delete: no {\"result\":1} in:\n%s\n", outbuf);
		return 1;
	}
	printf("ok: l2_delete reports the platform's result\n");
	return 0;
}
