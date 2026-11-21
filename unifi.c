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
#include <stddef.h>
#include "machine.h"
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "uip/uip.h"
#include "dns.h"
#include "rtl837x_flash.h"
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
#define UE_FLASH	6

/* Room for the HTTP request header in front of the TNBU packet: 148
 * bytes of fixed text and numbers, the host and the path at most 47 each.
 * u_url() admits no character that u_x() would escape in either. */
#define HROOM		256
#define UNIFI_PORT	8080
#define TNBU_HDR	40
#define TAG_LEN		16
#define STEP		32	/* bytes crypted per tick: two blocks, about 5.5 ms */

/* The adoption survives reboots and firmware updates in two flash sectors
 * of its own: an update rewrites flash below CONFIG_START only, and the
 * configuration uses the sector at CONFIG_START. Each save appends a
 * record in the next free slot of the current sector, and the valid record
 * with the highest sequence number counts. When the sector is full, the
 * other one is erased and written, so the last good record is never the
 * one being erased. A record counts only once its commit byte, written
 * last and on its own, reads 0: a power cut while saving loses at most the
 * record being written. */
#define UNIFI_STATE_START	(CONFIG_START + FLASH_SECTOR_SIZE)
#define UNIFI_SLOT		64
#define UNIFI_SLOTS		(FLASH_SECTOR_SIZE / UNIFI_SLOT)
#define UNIFI_FLASH_TRIES	3	/* then a broken flash is left alone */

extern __xdata uint32_t uptime_secs;
extern __xdata struct flash_region_t flash_region;
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
/* The saved state: written on adoption, on a new cfgversion, and once per
 * boot while adopted, so that the boot counter keeps the IVs unique.
 * Changing the layout needs a new magic. */
struct u_rec {
	char magic[4];
	uint32_t seq;	/* one more on every save */
	uint32_t boot;
	uint8_t key[16];
	char cfgver[UNIFI_CFGVER_LEN];
	uint8_t sum;	/* the bytes before it add up to this */
	uint8_t commit;	/* 0 once the record is complete */
};
typedef char u_rec_fits[sizeof(struct u_rec) <= UNIFI_SLOT ? 1 : -1];
static __xdata struct u_rec u_rec, u_back;
static __xdata uint8_t u_sec, u_slot;	/* the current sector and its next free slot */
static __xdata uint8_t u_free[2];	/* the first free slot of each sector, at boot */
static __xdata uint32_t u_seq, u_best;
static __code const char u_magic[] = "UNF2";

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
 * encryption. Until step 5 the port table is empty. */
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
	u_s(",\"cfgversion\":\"");
	if (unifi.cfgver[0])
		u_x(unifi.cfgver);
	else
		u_c('?');
	/* State 1 while pending and 4 once adopted, as accepted in the lab */
	if (unifi.adopted)
		u_s("\",\"default\":false,\"state\":4,");
	else
		u_s("\",\"default\":true,\"state\":1,");
	u_s("\"fw_caps\":0,\"isolated\":false,\"locating\":false,"
	    "\"selfrun_beacon\":false,\"port_table\":[]}");
	unifi.end = unifi.pos;

	/* Packet version 0, then the fields that are not zero */
	u_p = u_buf + HROOM;
	memset(u_p, 0, TNBU_HDR);
	memcpyc(u_p, "TNBU", 4);
	memcpy(u_p + 8, uip_ethaddr.addr, 6);
	u_p[15] = 0x09;		/* encrypted, AES-GCM, not compressed */
	/* The IV: random, then the boot and message counters. Under the
	 * authkey it never repeats, even if the RNG does: the boot counter is
	 * saved before the first inform of each boot. The default key is
	 * public, so its IVs need not be unique. */
	for (u_i = 16; u_i < 24; u_i += 4) {
		get_random_32();
		memcpy(u_p + u_i, sfr_data, 4);
	}
	u_be32(u_p + 24, unifi.boot);
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


/* The sum of u_rec's bytes before .sum */
static uint8_t u_sum(void)
{
	u_d[0] = 0;
	for (u_d[1] = 0; u_d[1] < offsetof(struct u_rec, sum); u_d[1]++)
		u_d[0] += ((__xdata uint8_t *)&u_rec)[u_d[1]];
	return u_d[0];
}

static uint32_t u_addr(void)
{
	return UNIFI_STATE_START + (uint16_t)u_sec * FLASH_SECTOR_SIZE + (uint16_t)u_slot * UNIFI_SLOT;
}

/* Writes the state as a new record. Returns 1 when it reads back as
 * written, else leaves unifi.unsaved set for a later try. */
static uint8_t u_save(void)
{
	unifi.unsaved = 1;
	if (unifi.flash_fails >= UNIFI_FLASH_TRIES)
		return 0;
	memcpyc(u_rec.magic, u_magic, 4);
	u_rec.seq = ++u_seq;
	u_rec.boot = unifi.boot;
	memcpy(u_rec.key, unifi.key, 16);
	memcpy(u_rec.cfgver, unifi.cfgver, UNIFI_CFGVER_LEN);
	u_rec.sum = u_sum();
	u_rec.commit = 0xff;
	if (u_slot >= UNIFI_SLOTS) {
		u_sec ^= 1;
		u_slot = 0;
		flash_region.addr = u_addr();
		flash_sector_erase();
	}
	flash_region.addr = u_addr();
	flash_region.len = sizeof(u_rec) - 1;
	flash_write_bytes((__xdata uint8_t *)&u_rec);
	u_rec.commit = 0;
	flash_region.addr = u_addr() + offsetof(struct u_rec, commit);
	flash_region.len = 1;
	flash_write_bytes(&u_rec.commit);
	flash_region.addr = u_addr();
	flash_region.len = sizeof(u_back);
	flash_read_bulk((__xdata uint8_t *)&u_back);
	u_slot++;
	if (memcmp(&u_back, &u_rec, sizeof(u_rec))) {
		unifi.flash_fails++;
		return 0;
	}
	unifi.flash_fails = 0;
	unifi.unsaved = 0;
	return 1;
}

/* Back to the default key: not adopted, no cfgversion. The boot counter
 * stays, so a later adoption with the same key repeats no IV. */
static void u_forget(void)
{
	if (!unifi.adopted)
		return;
	memcpyc(unifi.key, u_default_key, 16);
	unifi.adopted = 0;
	unifi.keyed = 0;
	unifi.cfgver[0] = 0;
	if (!u_save())
		print_string("Error: the UniFi state could not be saved\n");
}


/* The reply, decrypted, is at u_p and u_n bytes long. u_find() returns the
 * offset just past s in [from, to), or 0xffff. */
static __xdata uint16_t u_j, u_ms, u_me;
static __xdata uint8_t u_k[UNIFI_CFGVER_LEN];	/* a key or cfgversion being read */

static uint16_t u_find(__code const char *s, __xdata uint16_t from, __xdata uint16_t to)
{
	for (u_j = from; u_j < to; u_j++)
		if (strstart(u_p + u_j, s))
			break;
	u_j += strlen(s);
	if (u_j > to)
		return 0xffff;
	return u_j;
}

static uint8_t u_hexval(__xdata uint8_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return 0xff;
}

/* Is unifi.key the default key? */
static uint8_t u_is_default(void)
{
	for (u_j = 0; u_j < 16; u_j++)
		if (unifi.key[u_j] != u_default_key[u_j])
			return 0;
	return 1;
}

/* A key=value line of mgmt_cfg: the offset of its value, or 0xffff. The
 * key must start the string or follow an escaped newline, so that
 * "authkey=" does not match the end of another key. */
static uint16_t u_line(__code const char *key)
{
	for (u_i = u_ms; u_i < u_me; u_i = u_j) {
		u_j = u_find(key, u_i, u_me);
		if (u_j == 0xffff)
			return 0xffff;
		u_i = u_j - strlen(key);
		if (u_i == u_ms || (u_i >= u_ms + 2 && u_p[u_i - 2] == '\\' && u_p[u_i - 1] == 'n'))
			return u_j;
	}
	return 0xffff;
}

/* The authkey at u_i: exactly 32 hex digits, into unifi.key */
static uint8_t u_authkey(void)
{
	for (u_j = 0; u_j < 32; u_j++) {
		if (u_i + u_j >= u_me)
			return 0;
		u_d[0] = u_hexval(u_p[u_i + u_j]);
		if (u_d[0] == 0xff)
			return 0;
		u_k[u_j >> 1] = u_k[u_j >> 1] << 4 | u_d[0];
	}
	if (u_i + 32 < u_me && u_hexval(u_p[u_i + 32]) != 0xff)
		return 0;
	memcpy(unifi.key, u_k, 16);
	return 1;
}

/* The cfgversion at u_i, up to the first character that is not a hex
 * digit. Returns 1 when it differs from the one kept. */
static uint8_t u_cfgver(void)
{
	for (u_j = 0; u_j < UNIFI_CFGVER_LEN - 1 && u_i + u_j < u_n && u_hexval(u_p[u_i + u_j]) != 0xff; u_j++)
		u_k[u_j] = u_p[u_i + u_j];
	u_k[u_j] = 0;
	if (!u_j || !memcmp(u_k, unifi.cfgver, u_j + 1))
		return 0;
	memcpy(unifi.cfgver, u_k, u_j + 1);
	return 1;
}

/* The tag checked out: act on the reply */
static __code const char u_type[] = "\"_type\":\"";
static __code const char u_mgmt[] = "\"mgmt_cfg\":\"";
static __xdata uint8_t u_dirty;

static void u_decrypted(void)
{
	unifi.type[0] = 0;
	u_p = u_buf + unifi.hdr + TNBU_HDR;
	u_n = unifi.clen - TNBU_HDR - TAG_LEN;
	u_i = u_find(u_type, 0, u_n);
	if (u_i != 0xffff) {
		for (u_d[0] = 0; u_d[0] < UNIFI_TYPE_LEN - 1 && u_i < u_n && u_p[u_i] != '"'; u_d[0]++)
			unifi.type[u_d[0]] = u_p[u_i++];
		unifi.type[u_d[0]] = 0;
	}
	unifi.err = 0;
	if (!strcmp(unifi.type, "noop")) {
		/* The controller paces the informs */
		u_i = u_find("\"interval\":", 0, u_n);
		if (u_i != 0xffff) {
			for (u_j = 0; u_i < u_n && u_p[u_i] >= '0' && u_p[u_i] <= '9'; u_i++)
				if (u_j <= 3600)	/* larger stays larger, without overflow */
					u_j = u_j * 10 + u_p[u_i] - '0';
			if (u_j >= 5 && u_j <= 3600)
				unifi.interval = u_j;
		}
		u_done();
		return;
	}
	u_dirty = 0;
	if (!strcmp(unifi.type, "setparam")) {
		/* mgmt_cfg is a string of key=value lines */
		u_ms = u_find(u_mgmt, 0, u_n);
		if (u_ms != 0xffff) {
			for (u_me = u_ms; u_me < u_n && u_p[u_me] != '"'; u_me++)
				;
			/* A key is taken only while on the default one */
			u_i = u_line("authkey=");
			if (!unifi.adopted && u_i != 0xffff && u_authkey() && !u_is_default()) {
				unifi.adopted = 1;
				unifi.keyed = 0;
				unifi.boot++;
				unifi.booted = 1;
				u_dirty = 1;
			}
		}
		/* The provisioning cfgversion, else the one in mgmt_cfg */
		u_i = u_find("\"cfgversion\":\"", 0, u_n);
		if (u_i == 0xffff && u_ms != 0xffff)
			u_i = u_line("cfgversion=");
		if (unifi.adopted && u_i != 0xffff && u_cfgver())
			u_dirty = 1;
		/* A save that fails is tried again before the next inform */
		if (u_dirty)
			u_save();
	} else if (!strcmp(unifi.type, "setdefault")) {
		/* Removed in the controller: wait for adoption again */
		u_dirty = unifi.adopted;
		u_forget();
	}
	u_done();
	/* After a change the controller expects the next inform at once; other
	 * replies, such as commands not handled here, wait the interval */
	if (u_dirty)
		unifi.next = uptime_secs;
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
		if (unifi.adopted && !unifi.booted) {
			/* One flash write per boot keeps the IVs unique */
			unifi.boot++;
			unifi.booted = 1;
			unifi.unsaved = 1;
		}
		/* No inform goes out until the state is in flash: under the
		 * authkey, the boot counter in the IV must be saved first */
		if (unifi.unsaved && !u_save()) {
			u_fail(UE_FLASH);
			return;
		}
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
	/* The valid record with the highest sequence number, in either
	 * sector. A save that failed can leave an erased slot between used
	 * ones, so every slot is read, and writing goes on after the last
	 * used one. */
	memcpyc(u_k, u_default_key, 16);
	u_seq = 0;
	u_best = 0;
	u_sec = 0;
	for (u_d[2] = 0; u_d[2] < 2; u_d[2]++) {
		u_free[u_d[2]] = 0;
		for (u_d[3] = 0; u_d[3] < UNIFI_SLOTS; u_d[3]++) {
			flash_region.addr = UNIFI_STATE_START + (uint16_t)u_d[2] * FLASH_SECTOR_SIZE
				+ (uint16_t)u_d[3] * UNIFI_SLOT;
			flash_region.len = sizeof(u_rec);
			flash_read_bulk((__xdata uint8_t *)&u_rec);
			for (u_d[4] = 0; u_d[4] < sizeof(u_rec); u_d[4]++)
				if (((__xdata uint8_t *)&u_rec)[u_d[4]] != 0xff)
					break;
			if (u_d[4] == sizeof(u_rec))
				continue;	/* erased */
			u_free[u_d[2]] = u_d[3] + 1;
			if (!strstart(u_rec.magic, u_magic) || u_rec.sum != u_sum() || u_rec.commit)
				continue;
			if (u_rec.seq <= u_best)
				continue;
			u_best = u_rec.seq;
			u_sec = u_d[2];
			unifi.boot = u_rec.boot;
			unifi.adopted = 0;
			if (memcmp(u_rec.key, u_k, 16))
				unifi.adopted = 1;
			memcpy(unifi.key, u_rec.key, 16);
			memcpy(unifi.cfgver, u_rec.cfgver, UNIFI_CFGVER_LEN);
			unifi.cfgver[UNIFI_CFGVER_LEN - 1] = 0;
		}
	}
	u_seq = u_best;
	u_slot = u_free[u_sec];
}


/* A factory reset leaves the controller too */
void unifi_reset(void) __banked
{
	u_forget();
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
	"bad TNBU header", "tag mismatch", "flash write failed"
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
	print_string(unifi.adopted ? "\nAdopted, cfgversion " : "\nNot adopted, cfgversion ");
	if (unifi.cfgver[0])
		print_string_x(unifi.cfgver);
	else
		write_char('?');
	print_string(", interval ");
	itoa_short(unifi.interval);
	print_string(" s\nModel " UNIFI_MODEL ", informs sent ");
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
	if (cmd_words_len == 2 && u_word("forget")) {
		/* Leave the controller: back to the default key */
		u_stop();
		u_forget();
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
	u_error("unifi [on|off|inform <url>|forget|selftest]\n");
}
