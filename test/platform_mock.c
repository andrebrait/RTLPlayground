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

__xdata char port_names[9][PORT_NAME_SIZE];
__xdata uint8_t sfp_pins_last;

/* Ports: fixed values the seam test looks for in the pages. */
uint8_t port_link_get(uint8_t port) __banked { return 3; }
bool port_phy_enabled(uint8_t port) __banked { return true; }
uint8_t port_phy_adv_get(uint8_t port) __banked { return 0x3f; }

/* Every counter reads 0x0000000100000002. */
void port_counter_get(uint8_t port, uint8_t counter, __xdata uint8_t *out8) __banked
{
	static const uint8_t v[8] = { 0, 0, 0, 1, 0, 0, 0, 2 };
	for (uint8_t i = 0; i < 8; i++)
		out8[i] = v[i];
}

void port_macs_learned_get(uint8_t port, __xdata uint8_t *out4) __banked
{
	out4[0] = out4[1] = out4[2] = 0;
	out4[3] = 5;
}

uint16_t port_mtu_get(uint8_t port) __banked { return 1522; }
void port_mtu_set(uint8_t port, uint16_t mtu) __banked { }

uint8_t port_eee_adv_get(uint8_t port) __banked { return 7; }
uint8_t port_eee_lp_get(uint8_t port) __banked { return 5; }
bool port_eee_active(uint8_t port) __banked { return true; }
