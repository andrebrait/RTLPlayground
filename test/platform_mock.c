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

__xdata uint8_t port_buf[24];

/* Every counter reads 0x0000000100000002. */
void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked
{
	static const uint8_t v[8] = { 0, 0, 0, 1, 0, 0, 0, 2 };
	for (uint8_t i = 0; i < 8; i++)
		port_buf[i] = v[i];
}

void port_macs_learned_get(uint8_t port) __banked
{
	port_buf[0] = port_buf[1] = port_buf[2] = 0;
	port_buf[3] = 5;
}

uint16_t port_mtu_get(uint8_t port) __banked { return 1522; }
void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked { }

uint8_t port_eee_adv_get(uint8_t port) __banked { return 7; }
uint8_t port_eee_lp_get(uint8_t port) __banked { return 5; }
bool port_eee_active(uint8_t port) __banked { return true; }

/* Mirror enabled on logical port 8, RX mask 0x0003, TX mask 0x0001. */
void port_mirror_get(void) __banked
{
	static const uint8_t v[6] = { 1, 8, 0x00, 0x03, 0x00, 0x01 };
	for (uint8_t i = 0; i < 6; i++)
		port_buf[i] = v[i];
}

/* Every LAG hashes 0x00000007. */
void port_lag_hash_get(uint8_t lag) __banked
{
	port_buf[0] = port_buf[1] = port_buf[2] = 0;
	port_buf[3] = 7;
}

/* Every port is limited at 0xabcde; ingress also has flow control. */
void bandwidth_get(uint8_t port, __xdata uint8_t egress) __banked
{
	static const uint8_t v[5] = { 1, 0x0a, 0xbc, 0xde, 0 };
	for (uint8_t i = 0; i < 5; i++)
		port_buf[i] = v[i];
	port_buf[4] = !egress;
}

/* Storm types: enabled 1010, pps 1100, rate 0x000123 + type. */
void storm_get(uint8_t port) __banked
{
	for (uint8_t t = 0; t < 4; t++) {
		port_buf[t] = !(t & 1);
		port_buf[4 + t] = t < 2;
		port_buf[8 + 4 * t] = 0;
		port_buf[9 + 4 * t] = 0x00;
		port_buf[10 + 4 * t] = 0x01;
		port_buf[11 + 4 * t] = 0x23 + t;
	}
}
