/*
 * platform_mock.c - the platform side of the seam check.
 *
 * Defines, against platform.h only, what cmd_parser.c, cmd_editor.c and
 * httpd/page_impl.c need from the platform, with no register file behind it.
 * Each task that moves hardware access behind platform.h adds its mock here.
 */
#include "platform.h"

const struct machine machine = {
	.machine_name = "SEAMTEST",
	.min_port = 0,
	.max_port = 8,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};

uint8_t  outbuf[TCP_OUTBUF_SIZE];
uint16_t slen;

uint16_t strtox(uint8_t *dst, const char *s)
{
	uint8_t *b = dst;
	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}

/* The same MTU on every port. Not yet in platform.h: send_mtu() still reads
 * the register itself, which is what the seam check reports. */
uint16_t port_mtu_get(uint8_t port) __banked { return 1522; }
