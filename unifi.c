/*
 * UniFi inform client for the RTL837x platform
 *
 * Every interval the switch posts an inform to the controller: a TNBU packet
 * whose JSON payload is encrypted with AES-128-GCM, the 40-byte TNBU header
 * being the AAD. One buffer holds the request and then the reply. The crypto
 * runs from the main loop, a few blocks per tick, never inside a uIP
 * callback: the connection is opened once the request is encrypted, and the
 * reply is decrypted after it is closed.
 *
 * Until adoption the key is the public default key, MD5("ubnt"), and the
 * controller answers 404 while the device waits to be adopted.
 *
 * Locals are static XDATA: internal RAM is full.
 *
 * This code is in the Public Domain
 */

#include <stdint.h>
#include "machine.h"
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "uip/uip.h"
#include "dns.h"
#include "crypto/aes_gcm.h"
#include "unifi.h"

#pragma codeseg BANK5
#pragma constseg BANK5

/* The reported model. The TL-SG108PE V5 (RTL836x, 8 x 1G, PoE on 1-4)
 * maps to the USW-Lite-8-PoE, the 8 x 2.5G + SFP+ boards to the
 * USW-Flex-2.5G-8. The version is the model's current release as UniFi
 * Network 10.6.109 listed it, which is above its minimumFirmwareRequired. */
#ifndef SOC_FAMILY
#error "machine.h must define SOC_FAMILY"
#endif
#ifndef UNIFI_MODEL
#if SOC_FAMILY == RTL836X
#define UNIFI_MODEL	"USL8LP"
#define UNIFI_DISPLAY	"USW-Lite-8-PoE"
#define UNIFI_VERSION	"7.5.15.17146"
#else
#define UNIFI_MODEL	"USWED36"
#define UNIFI_DISPLAY	"USW-Flex-2.5G-8"
#define UNIFI_VERSION	"2.1.8.971"
#endif
#endif

#define U_IDLE		0
#define U_RESOLVE	1
#define U_ENCRYPT	2
#define U_SEND		3	/* connecting, then sending the request */
#define U_RECV		4
#define U_REPLY		5	/* reply received, check its header */
#define U_DECRYPT	6

#define UE_DNS		1
#define UE_CONN		2
#define UE_HTTP		3
#define UE_TNBU		4
#define UE_TAG		5

/* Room for the HTTP request header in front of the TNBU packet: 148
 * bytes of fixed text and numbers, the host and the path at most 47 each.
 * u_url() admits no character that u_x() would escape in either. */
#define HROOM		256
#define UNIFI_PORT	8080
#define TNBU_HDR	40
#define TAG_LEN		16
#define STEP		32	/* bytes crypted per tick: two blocks, about 5.5 ms */

extern __xdata uint32_t uptime_secs;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[15];

__xdata struct unifi_state unifi;
/* unifi_init() clears it with memset(), whose length is a byte */
typedef char u_state_fits[sizeof(struct unifi_state) < 256 ? 1 : -1];
static __xdata uint8_t u_buf[UNIFI_BUF_SIZE];

/* Scratch for the helpers below: internal RAM has no room for locals */
static __xdata uint16_t u_i, u_n;
static __xdata uint8_t u_d[14];	/* a 32-bit value, then its 10 digits */
static __xdata uint8_t *__xdata u_p, *__xdata u_q;

static __code const uint8_t u_default_key[16] = {
	0xba, 0x86, 0xf2, 0xbb, 0xe1, 0x07, 0xc7, 0xc5,
	0x7e, 0xb5, 0xf2, 0x69, 0x07, 0x75, 0xc7, 0x12
};
static __code const char u_default_host[] = "unifi";
static __code const char u_default_path[] = "/inform";
static __code const char u_hex[] = "0123456789abcdef";


/* JSON and HTTP writers, at unifi.pos in u_buf */
static void u_c(__xdata char c)
{
	u_buf[unifi.pos++] = c;
}

static void u_s(__code const char *s)
{
	while (*s)
		u_buf[unifi.pos++] = *s++;
}

/* A string from RAM, with " and \ escaped */
static void u_x(__xdata const char *s)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			u_c('\\');
		u_c(*s);
	}
}

static void u_be32(__xdata uint8_t *p, __xdata uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* The decimal digits of v, last first, in u_d[4..4 + u_n) */
static void u_digits(__xdata uint32_t v)
{
	u_be32(u_d, v);
	u_n = 0;
	do
		u_d[4 + u_n++] = '0' + div_small(u_d, 4, 10);
	while (u_d[0] | u_d[1] | u_d[2] | u_d[3]);
}

static void u_dec(__xdata uint32_t v)
{
	u_digits(v);
	while (u_n)
		u_c(u_d[4 + --u_n]);
}

static void u_ip(__xdata const uint8_t *a)
{
	for (u_i = 0; u_i < 4; u_i++) {
		if (u_i)
			u_c('.');
		u_dec(a[u_i]);
	}
}

/* The MAC as aa:bb:cc:dd:ee:ff, or as AABBCCDDEEFF when sep is 0 */
static void u_mac(__xdata char sep)
{
	for (u_i = 0; u_i < 6; u_i++) {
		if (u_i && sep)
			u_c(sep);
		u_d[0] = uip_ethaddr.addr[u_i];
		u_d[1] = u_hex[u_d[0] >> 4];
		u_d[2] = u_hex[u_d[0] & 15];
		if (!sep) {
			if (u_d[1] >= 'a')
				u_d[1] -= 'a' - 'A';
			if (u_d[2] >= 'a')
				u_d[2] -= 'a' - 'A';
		}
		u_c(u_d[1]);
		u_c(u_d[2]);
	}
}


/* Back to idle, the next inform one interval from now */
static void u_done(void)
{
	unifi.phase = U_IDLE;
	unifi.next = uptime_secs + unifi.interval;
}

/* Ends a failed inform. The caller has let go of the connection. */
static void u_fail(__xdata uint8_t e)
{
	unifi.err = e;
	unifi.fails++;
	u_done();
}


/* The inform JSON, then the TNBU header in front of it. Starts the
 * encryption. Reported until adoption: pending, default key, no config. */
static void u_build(void)
{
	unifi.pos = HROOM + TNBU_HDR;
	u_s("{\"mac\":\"");
	u_mac(':');
	u_s("\",\"serial\":\"");
	u_mac(0);
	u_s("\",\"model\":\"" UNIFI_MODEL "\",\"model_display\":\"" UNIFI_DISPLAY
	    "\",\"version\":\"" UNIFI_VERSION "\",\"ip\":\"");
	u_ip((__xdata uint8_t *)uip_hostaddr);
	u_s("\",\"netmask\":\"");
	u_ip((__xdata uint8_t *)uip_netmask);
	u_s("\",\"hostname\":\"");
	u_x(hostname);
	/* An address, not a name: the controller refuses a host name here */
	u_s("\",\"inform_url\":\"http://");
	u_ip(unifi.addr);
	u_c(':');
	u_dec(unifi.port);
	u_x(unifi.path);
	u_s("\",\"uptime\":");
	u_dec(uptime_secs);
	u_s(",\"cfgversion\":\"?\",\"default\":true,\"state\":1,\"fw_caps\":0,"
	    "\"isolated\":false,\"locating\":false,\"selfrun_beacon\":false,"
	    "\"port_table\":[]}");
	unifi.end = unifi.pos;

	/* Packet version 0, then the fields that are not zero */
	u_p = u_buf + HROOM;
	memset(u_p, 0, TNBU_HDR);
	memcpyc(u_p, "TNBU", 4);
	memcpy(u_p + 8, uip_ethaddr.addr, 6);
	u_p[15] = 0x09;		/* encrypted, AES-GCM, not compressed */
	/* The IV: random, and a message counter so that it never repeats in
	 * one boot even if the RNG does */
	for (u_i = 16; u_i < 28; u_i += 4) {
		get_random_32();
		memcpy(u_p + u_i, sfr_data, 4);
	}
	u_be32(u_p + 28, ++unifi.msg);
	u_p[35] = 1;		/* payload version: JSON */
	unifi.left = unifi.end - (HROOM + TNBU_HDR);
	u_be32(u_p + 36, unifi.left + TAG_LEN);
	unifi.cp = u_p + TNBU_HDR;
	gcm_start(u_p + 16, 0);
	gcm_aad(u_p, TNBU_HDR);
}


/* The HTTP request header, written behind the packet and moved in front
 * of it, so that the request is one run of bytes */
static void u_http(void)
{
	unifi.pos = unifi.end;
	u_s("POST ");
	u_x(unifi.path);
	u_s(" HTTP/1.1\r\nHost: ");
	u_x(unifi.host);
	u_c(':');
	u_dec(unifi.port);
	u_s("\r\nUser-Agent: AirControl Agent v1.0\r\n"
	    "Content-Type: application/x-binary\r\nContent-Length: ");
	u_dec(unifi.end - HROOM);
	u_s("\r\nConnection: close\r\n\r\n");
	u_n = unifi.pos - unifi.end;
	unifi.start = HROOM - u_n;
	memcpy(u_buf + unifi.start, u_buf + unifi.end, u_n);
}


static void u_send(void)
{
	unifi.sent = unifi.end - unifi.off;
	if (unifi.sent > uip_mss())
		unifi.sent = uip_mss();
	uip_send(u_buf + unifi.off, unifi.sent);
}


/* Is the reply complete? Finds the end of the HTTP header, then its status
 * and Content-Length. Returns 0 while more is expected, 1 when complete,
 * 2 when it cannot be used. */
static __code const char u_clen[] = "\ncontent-length:";

static __xdata uint16_t u_scan;	/* the header end is not before this */

static uint8_t u_parse(void)
{
	if (!unifi.hdr) {
		for (u_i = u_scan < 3 ? 3 : u_scan; u_i < unifi.pos; u_i++)
			if (strstart(u_buf + u_i - 3, "\r\n\r\n"))
				break;
		if (u_i < unifi.pos) {
			unifi.hdr = u_i + 1;
			if (!strstart(u_buf, "HTTP/1.") || unifi.hdr < 13)
				return 2;
			u_n = 0;
			for (u_i = 9; u_i < 12; u_i++) {
				if (u_buf[u_i] < '0' || u_buf[u_i] > '9')
					return 2;
				u_n = u_n * 10 + u_buf[u_i] - '0';
			}
			unifi.status = u_n;
			/* Content-Length, any case. Without it the reply ends at close. */
			unifi.clen = 0xffff;
			for (u_i = 0; u_i + sizeof(u_clen) - 1 < unifi.hdr; u_i++) {
				for (u_n = 0; u_clen[u_n]; u_n++) {
					u_d[0] = u_buf[u_i + u_n];
					if (u_d[0] >= 'A' && u_d[0] <= 'Z')
						u_d[0] += 'a' - 'A';
					if (u_d[0] != u_clen[u_n])
						break;
				}
				if (u_clen[u_n])
					continue;
				u_i += u_n;
				while (u_buf[u_i] == ' ')
					u_i++;
				unifi.clen = 0;
				while (u_buf[u_i] >= '0' && u_buf[u_i] <= '9') {
					if (unifi.clen > (UNIFI_BUF_SIZE - unifi.hdr) / 10)
						return 2;
					unifi.clen = unifi.clen * 10 + u_buf[u_i++] - '0';
				}
				if (unifi.clen > UNIFI_BUF_SIZE - unifi.hdr)
					return 2;
				break;
			}
		}
	}
	if (!unifi.hdr || unifi.clen == 0xffff) {
		if (unifi.pos == UNIFI_BUF_SIZE)
			return 2;
		return 0;
	}
	if (unifi.pos < unifi.hdr + unifi.clen)
		return 0;
	return 1;
}


/* The reply is in: a 200 carries a TNBU packet, anything else ends here */
static void u_received(void)
{
	unifi.conn = 0;
	if (unifi.clen == 0xffff)
		unifi.clen = unifi.pos - unifi.hdr;
	if (unifi.status != 200) {
		unifi.type[0] = 0;
		unifi.err = 0;
		u_done();
		return;
	}
	unifi.phase = U_REPLY;
}


/* uIP can report several events at once: a FIN arrives together with the
 * last data, and an acknowledgement together with the reply. */
void unifi_appcall(void) __banked
{
	if (!unifi.conn || uip_conn != unifi.conn)
		return;
	if (unifi.abort) {
		unifi.abort = 0;
		unifi.conn = 0;
		uip_abort();
		return;
	}
	if (uip_aborted() || uip_timedout()) {
		unifi.conn = 0;
		u_fail(UE_CONN);
		return;
	}
	if (uip_connected()) {
		unifi.off = unifi.start;
		u_send();
		return;
	}
	if (unifi.phase == U_SEND) {
		if (uip_rexmit()) {
			/* The same bytes: uIP resends the segment's original length,
			 * and uip_mss() may have changed since */
			uip_send(u_buf + unifi.off, unifi.sent);
			return;
		}
		if (uip_acked()) {
			unifi.off += unifi.sent;
			if (unifi.off == unifi.end) {
				unifi.phase = U_RECV;
				unifi.pos = 0;
				unifi.hdr = 0;
			}
		}
		if (unifi.phase == U_SEND) {
			/* A reply before the whole request was taken: uIP has
			 * acknowledged it, so it cannot be dropped silently */
			if (uip_newdata()) {
				unifi.conn = 0;
				uip_abort();
				u_fail(UE_HTTP);
				return;
			}
			if (uip_acked() && !uip_closed()) {
				u_send();
				return;
			}
		}
	}
	if (unifi.phase == U_RECV && uip_newdata()) {
		u_d[0] = 2;
		if (uip_len <= UNIFI_BUF_SIZE - unifi.pos) {
			u_scan = unifi.pos;
			memcpy(u_buf + unifi.pos, uip_appdata, uip_len);
			unifi.pos += uip_len;
			u_d[0] = u_parse();
		}
		if (u_d[0] == 1) {
			if (!uip_closed())
				uip_close();
			u_received();
			return;
		}
		if (u_d[0] == 2) {
			unifi.conn = 0;
			uip_abort();
			u_fail(UE_HTTP);
			return;
		}
	}
	if (uip_closed()) {
		unifi.conn = 0;
		/* Only a reply without Content-Length may end at the close */
		if (unifi.phase == U_RECV && unifi.hdr && unifi.clen == 0xffff)
			u_received();
		else if (unifi.phase == U_RECV && unifi.pos)
			u_fail(UE_HTTP);
		else
			u_fail(UE_CONN);
	}
}


/* Checks the TNBU header of the reply and starts its decryption */
static void u_reply(void)
{
	u_p = u_buf + unifi.hdr;
	if (unifi.clen < TNBU_HDR + TAG_LEN || !strstart(u_p, "TNBU")
	    || (u_p[15] & 0x0f) != 0x09 || u_p[36] || u_p[37]
	    || ((uint16_t)u_p[38] << 8 | u_p[39]) != unifi.clen - TNBU_HDR) {
		u_fail(UE_TNBU);
		return;
	}
	unifi.left = unifi.clen - TNBU_HDR - TAG_LEN;
	unifi.cp = u_p + TNBU_HDR;
	gcm_start(u_p + 16, 1);
	gcm_aad(u_p, TNBU_HDR);
	unifi.phase = U_DECRYPT;
}


/* The tag checked out: keep the reply's _type */
static __code const char u_type[] = "\"_type\":\"";

static void u_decrypted(void)
{
	unifi.type[0] = 0;
	u_p = u_buf + unifi.hdr + TNBU_HDR;
	u_n = unifi.clen - TNBU_HDR - TAG_LEN;
	for (u_i = 0; u_i + sizeof(u_type) - 1 < u_n; u_i++) {
		if (!strstart(u_p + u_i, u_type))
			continue;
		u_i += sizeof(u_type) - 1;
		for (u_d[0] = 0; u_d[0] < UNIFI_TYPE_LEN - 1 && u_i < u_n && u_p[u_i] != '"'; u_d[0]++)
			unifi.type[u_d[0]] = u_p[u_i++];
		unifi.type[u_d[0]] = 0;
		break;
	}
	unifi.err = 0;
	u_done();
}


/* Two blocks of the current encryption or decryption. Returns 1 when the
 * last one is done, with the tag in gcm.blk. */
static uint8_t u_crypt(void)
{
	u_n = unifi.left > STEP ? STEP : unifi.left;
	gcm_crypt(unifi.cp, u_n);
	unifi.cp += u_n;
	unifi.left -= u_n;
	if (unifi.left)
		return 0;
	gcm_finish();
	return 1;
}


void unifi_tick(void) __banked
{
	if (!unifi.enabled || unifi.abort)
		return;
	switch (unifi.phase) {
	case U_IDLE:
		if (uptime_secs < unifi.next || dns_state.status == DNS_PENDING)
			return;
		if (!unifi.keyed) {
			/* About 30 ms, once per key */
			gcm_setkey(unifi.key);
			unifi.keyed = 1;
		}
		/* The reply fields describe the inform being made */
		unifi.status = 0;
		unifi.type[0] = 0;
		unifi.err = 0;
		memcpy(dns_state.name, unifi.host, DNS_NAME_LEN);
		dns_state.verbose = 0;
		dns_lookup();
		unifi.phase = U_RESOLVE;
		return;
	case U_RESOLVE:
		if (dns_state.status == DNS_PENDING)
			return;
		/* Another client took over the resolver: ask again */
		if (memcmp(dns_state.name, unifi.host, DNS_NAME_LEN)) {
			unifi.phase = U_IDLE;
			return;
		}
		if (dns_state.status != DNS_DONE) {
			u_fail(UE_DNS);
			return;
		}
		memcpy(unifi.addr, dns_state.addr, 4);
		u_build();
		unifi.phase = U_ENCRYPT;
		return;
	case U_ENCRYPT:
		if (!u_crypt())
			return;
		memcpy(u_buf + unifi.end, gcm.blk, TAG_LEN);
		unifi.end += TAG_LEN;
		u_http();
		unifi.conn = uip_connect((__xdata uip_ipaddr_t *)unifi.addr, HTONS(unifi.port));
		if (!unifi.conn) {
			u_fail(UE_CONN);
			return;
		}
		unifi.count++;
		unifi.phase = U_SEND;
		return;
	case U_REPLY:
		u_reply();
		return;
	case U_DECRYPT:
		if (!u_crypt())
			return;
		if (memcmp(gcm.blk, u_buf + unifi.hdr + unifi.clen - TAG_LEN, TAG_LEN)) {
			u_fail(UE_TAG);
			return;
		}
		u_decrypted();
	}
}


/* Stops the current inform and makes the next one due. A connection is
 * reset at its next event, which for one still opening is its SYN-ACK or
 * its timeout; the next inform waits for that. */
static void u_stop(void)
{
	if (unifi.conn)
		unifi.abort = 1;
	unifi.phase = U_IDLE;
	unifi.next = 0;
}


void unifi_init(void) __banked
{
	memset((__xdata uint8_t *)&unifi, 0, sizeof(unifi));
	memcpyc(unifi.host, u_default_host, sizeof(u_default_host));
	memcpyc(unifi.path, u_default_path, sizeof(u_default_path));
	unifi.port = UNIFI_PORT;
	unifi.interval = 10;
	memcpyc(unifi.key, u_default_key, 16);
}


/* "http://host[:port][/path]", into unifi.host, .port and .path. Nothing
 * changes unless the whole URL parses. */
static __code const char u_http_prefix[] = "http://";
static __xdata uint16_t u_port, u_plen;

static uint8_t u_url(void)
{
	u_p = &cmd_buffer[cmd_words_b[2]];
	if (!strstart(u_p, u_http_prefix))
		return 0;
	u_p += sizeof(u_http_prefix) - 1;
	/* Letters, digits, '.', '-' and '_' only: nothing u_x() escapes. A
	 * malformed name fails at the lookup, which "unifi" then shows. */
	for (u_i = 0; u_p[u_i] && u_p[u_i] != ':' && u_p[u_i] != '/' && u_p[u_i] != ' '; u_i++) {
		u_d[0] = u_p[u_i] | 0x20;
		if ((u_d[0] < 'a' || u_d[0] > 'z') && (u_p[u_i] < '0' || u_p[u_i] > '9')
		    && u_p[u_i] != '.' && u_p[u_i] != '-' && u_p[u_i] != '_')
			return 0;
	}
	if (!u_i || u_i >= DNS_NAME_LEN)
		return 0;
	u_n = u_i;
	u_port = UNIFI_PORT;
	if (u_p[u_n] == ':') {
		u_port = 0;
		while (u_p[++u_n] >= '0' && u_p[u_n] <= '9') {
			if (u_port > 6553 || (u_port == 6553 && u_p[u_n] > '5'))
				return 0;
			u_port = u_port * 10 + u_p[u_n] - '0';
		}
		if (!u_port)
			return 0;
	}
	/* u_p stays at the host, u_q walks the path */
	u_q = u_p + u_n;
	u_plen = 0;
	if (*u_q == '/') {
		for (; u_q[u_plen] && u_q[u_plen] != ' '; u_plen++)
			if (u_q[u_plen] == '"' || u_q[u_plen] == '\\'
			    || u_plen == UNIFI_PATH_LEN - 1)
				return 0;
	} else if (*u_q && *u_q != ' ') {
		return 0;
	}
	memcpy(unifi.host, u_p, u_i);
	unifi.host[u_i] = 0;
	unifi.port = u_port;
	if (u_plen) {
		memcpy(unifi.path, u_q, u_plen);
		unifi.path[u_plen] = 0;
	} else {
		memcpyc(unifi.path, u_default_path, sizeof(u_default_path));
	}
	return 1;
}


static __code const char *__code const u_errors[] = {
	"none", "DNS lookup failed", "connection failed", "bad HTTP reply",
	"bad TNBU header", "tag mismatch"
};

static void u_print(__xdata uint32_t v)
{
	u_digits(v);
	while (u_n)
		write_char(u_d[4 + --u_n]);
}

static void u_show(void)
{
	print_string(unifi.enabled ? "UniFi on, inform http://" : "UniFi off, inform http://");
	print_string_x(unifi.host);
	write_char(':');
	itoa_short(unifi.port);
	print_string_x(unifi.path);
	print_string(", address ");
	print_ip(unifi.addr);
	print_string("\nModel " UNIFI_MODEL ", informs sent ");
	u_print(unifi.count);
	print_string(", last reply ");
	itoa_short(unifi.status);
	if (unifi.type[0]) {
		write_char(' ');
		print_string_x(unifi.type);
	}
	print_string(", failed ");
	u_print(unifi.fails);
	print_string(", last error: ");
	print_string(u_errors[unifi.err]);
	write_char('\n');
}


/* Is word 1 of the command w? cmd_compare() is in another bank. */
static uint8_t u_word(__code const char *w)
{
	u_p = &cmd_buffer[cmd_words_b[1]];
	if (!strstart(u_p, w))
		return 0;
	u_p += strlen(w);
	if (*u_p && *u_p != ' ')
		return 0;
	return 1;
}


/* cmd_error() is in another bank */
static void u_error(__code const char *msg)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: ");
	print_string(msg);
}


void unifi_parse(void) __banked
{
	if (cmd_words_len < 2) {
		u_show();
		return;
	}
	if (cmd_words_len == 2 && u_word("on")) {
		unifi.enabled = 1;
		u_stop();
		return;
	}
	if (cmd_words_len == 2 && u_word("off")) {
		unifi.enabled = 0;
		u_stop();
		return;
	}
	if (cmd_words_len == 3 && u_word("inform")) {
		if (!u_url()) {
			u_error("unifi inform http://<host>[:<port>][/<path>]\n");
			return;
		}
		u_stop();
		return;
	}
	if (cmd_words_len == 2 && u_word("selftest")) {
		/* The self test rekeys gcm and reuses its state */
		if (unifi.phase != U_IDLE || unifi.abort) {
			u_error("an inform is in progress\n");
			return;
		}
		gcm_selftest();
		unifi.keyed = 0;
		return;
	}
	u_error("unifi [on|off|inform <url>|selftest]\n");
}
