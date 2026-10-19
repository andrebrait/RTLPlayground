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
	return 0;
}
