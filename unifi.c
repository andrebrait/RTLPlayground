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
#include "snmp_hw.h"
#include "crypto/aes_gcm.h"
#include "unifi.h"
#include "rtl837x_lldp.h"
#ifdef MACHINE_POE_PORTS
#include "poe.h"
#endif

#pragma codeseg BANK5
#pragma constseg BANK5


#define U_IDLE		0
#define U_RESOLVE	1
#define U_ENCRYPT	2
#define U_SEND		3	/* connecting, then sending the request */
#define U_RECV		4
#define U_REPLY		5	/* reply received, check its header */
#define U_DECRYPT	6
#define U_PORTS		7	/* writing the port table, one port per tick */
#define U_APPLY		8	/* applying the controller's settings, one command per tick */

/* The areas "unifi apply" selects (unifi_apply.c) */
#define UA_VLANS	0x01
#define UA_PORTS	0x02
#define UA_LAG		0x04
#define UA_STP		0x08
#define UA_IGMP		0x10
#define UA_JUMBO	0x20
#define UA_MIRROR	0x40
#define UA_ALL		0x7f
#define UA_AREAS	7
static __xdata uint8_t ua_mask;
static void ua_caps(void);

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
#define UNIFI_SLOT		128
#define UNIFI_SLOTS		(FLASH_SECTOR_SIZE / UNIFI_SLOT)
#define UNIFI_FLASH_TRIES	3	/* then a broken flash is left alone */

extern __xdata uint32_t uptime_secs;
extern __code const struct machine machine;
extern __xdata struct flash_region_t flash_region;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[15];

__xdata struct unifi_state unifi;
/* unifi_init() clears it with memset(), whose length is a byte */
#ifndef RTLP_HOST_TEST	/* host pointers are wider; the host memset takes a size_t */
typedef char u_state_fits[sizeof(struct unifi_state) < 256 ? 1 : -1];
#endif
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
	char version[UNIFI_VERSION_LEN];
	uint8_t sum[2];	/* CRC-16/CCITT of the bytes before it */
	uint8_t commit;	/* 0 once the record is complete */
};
#define U_REC_LEN	(offsetof(struct u_rec, commit) + 1)	/* without host padding */
typedef char u_rec_fits[sizeof(struct u_rec) <= UNIFI_SLOT ? 1 : -1];
static __xdata struct u_rec u_rec, u_back;
static __xdata uint8_t u_sec, u_slot;	/* the current sector and its next free slot */
static __xdata uint8_t u_free[2];	/* the first free slot of each sector, at boot */
static __xdata uint32_t u_seq, u_best;
static __code const char u_magic[] = "UNF4";

static __code const char u_default_host[] = "unifi";
static __code const char u_default_version[] = UNIFI_VERSION;
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

/* The MAC at u_ma as aa:bb:cc:dd:ee:ff, or as AABBCCDDEEFF when sep is 0 */
static __xdata const uint8_t *__xdata u_ma;

static void u_mac(__xdata char sep)
{
	for (u_i = 0; u_i < 6; u_i++) {
		if (u_i && sep)
			u_c(sep);
		u_d[0] = u_ma[u_i];
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


/* Port counters: a 64-bit big-endian value, its sum, and its digits. The
 * last good values are kept per port, so that a read that fails repeats
 * them rather than sending 0 or a partial sum, which the controller would
 * take for a counter that went back or a link that flapped. A counter
 * never read yet is left out. */
#if SOC_FAMILY == RTL836X
#define U_CTRS	2	/* bytes only: the packet counters are 32 bits */
#else
#define U_CTRS	4
#endif
struct u_pc {
	uint8_t seen;	/* bit k: counter k read once */
	uint8_t adm;	/* 2 when the PHY is powered down */
	uint16_t mbps;
	struct snmp_counter c[U_CTRS];
};
static __xdata struct u_pc u_pc[MACHINE_MAX_PORT - MACHINE_MIN_PORT + 1];
static __xdata struct u_pc *__xdata u_pcp;	/* u_ph's */
static __xdata struct snmp_counter u_ctr, u_acc;
static __xdata uint16_t u_links;	/* last good link state, one bit per port */
static __xdata uint8_t u_dg[20];
static __xdata uint8_t u_ph, u_up, u_cnt, u_adm, u_ok;	/* u_d is the digit helpers' */
static __xdata uint32_t u_link;
static __xdata uint16_t u_mbps;
#ifdef MACHINE_POE_PORTS
static __xdata uint8_t u_poe;	/* PoE permitted, bit 0 = front port 1 */
#endif

/* u_acc in decimal; u_acc is used up */
static void u_dec64(void)
{
	u_n = 0;
	do {
		u_dg[u_n++] = '0' + div_small(u_acc.bytes, 8, 10);
		u_d[0] = 0;
		for (u_d[1] = 0; u_d[1] < 8; u_d[1]++)
			u_d[0] |= u_acc.bytes[u_d[1]];
	} while (u_d[0]);
	while (u_n)
		u_c(u_dg[--u_n]);
}

/* Adds counter sel of u_ph to u_acc; clears u_ok when it cannot be read */
static void u_add(__xdata uint8_t sel)
{
	if (!snmp_hw_stat(u_ph, sel, true, true, &u_ctr)) {
		u_ok = 0;
		return;
	}
	u_i = 0;	/* the carry is the high byte */
	for (u_d[1] = 8; u_d[1]--; ) {
		u_i = u_acc.bytes[u_d[1]] + u_ctr.bytes[u_d[1]] + (u_i >> 8);
		u_acc.bytes[u_d[1]] = u_i;
	}
}

/* "name":<sum of counters first..last>, or the last good sum (slot k), or
 * nothing when slot k was never read */
static void u_counter(__code const char *__xdata name, __xdata uint8_t k, __xdata uint8_t first, __xdata uint8_t last)
{
	if (k >= U_CTRS)
		return;
	memset(u_acc.bytes, 0, 8);
	u_ok = 1;
	for (; first <= last; first++)
		u_add(first);
	if (u_ok) {
		memcpy(u_pcp->c[k].bytes, u_acc.bytes, 8);
		u_pcp->seen |= 1 << k;
	} else if (u_pcp->seen & 1 << k) {
		memcpy(u_acc.bytes, u_pcp->c[k].bytes, 8);
	} else {
		return;
	}
	u_s(name);
	u_dec64();
}

/* The port table, numbered by front-panel port: the controller keys every
 * port setting on port_idx. log_to_phys_port, despite its name, maps an
 * ASIC port to its front-panel number, and 0 marks an unused port. On the
 * RTL837x, ASIC port 8 and, on an RTL8372, port 3 are the 10G lanes: an
 * SFP+ cage, or copper when the board has 10G PHYs (n_10g). Duplex is not
 * reported: the RTL837x status the firmware reads has no duplex bit. */
static void u_port_entry(void)
{
	if (snmp_hw_link(u_ph, &u_link)) {
		if ((uint16_t)u_link >> u_ph & 1)
			u_links |= (uint16_t)1 << u_ph;
		else
			u_links &= ~((uint16_t)1 << u_ph);
	}
	u_pcp = &u_pc[u_ph - MACHINE_MIN_PORT];
	u_up = 0;
	if (u_links >> u_ph & 1)
		u_up = 1;
	if (!u_up)
		u_pcp->mbps = 0;
	else if (snmp_hw_speed(u_ph, &u_mbps) && u_mbps)
		u_pcp->mbps = u_mbps;	/* 0 here is the reader's own view of a link going down */
	if (snmp_hw_admin(u_ph, &u_adm))
		u_pcp->adm = u_adm;
	if (u_cnt++)
		u_c(',');
	u_s("{\"port_idx\":");
	u_dec(machine.log_to_phys_port[u_ph]);
	if (machine.is_sfp[u_ph])
		u_s(",\"media\":\"SFP+\",\"speed_caps\":288");
#if SOC_FAMILY == RTL836X
	else
		u_s(",\"media\":\"GE\",\"speed_caps\":1048623");
#else
	else if (machine.n_10g && (u_ph == 8 || (u_ph == 3 && !machine.isRTL8373)))
		u_s(",\"media\":\"GE\",\"speed_caps\":1049064");	/* 100M to 10G */
	else
		u_s(",\"media\":\"GE\",\"speed_caps\":1048682");	/* 10M to 2.5G */
#endif
	/* EEE is offered on copper ports while ports are applied */
	if ((ua_mask & UA_PORTS) && !machine.is_sfp[u_ph])
		u_s(",\"port_caps\":1");
	if (u_up)
		u_s(",\"up\":true");
	else
		u_s(",\"up\":false");
	if (u_pcp->adm == 2)
		u_s(",\"enable\":false");
	else
		u_s(",\"enable\":true");
#ifdef MACHINE_POE_PORTS
	/* PoE is switched by GPIO and not measured, so no power is reported */
	u_d[0] = machine.log_to_phys_port[u_ph];
	if (u_d[0] <= MACHINE_POE_PORTS) {
		if (u_poe >> (u_d[0] - 1) & 1)
			u_s(",\"port_poe\":true,\"poe_caps\":7,\"poe_enable\":true,\"poe_mode\":\"auto\"");
		else
			u_s(",\"port_poe\":true,\"poe_caps\":7,\"poe_enable\":false,\"poe_mode\":\"off\"");
	} else {
		u_s(",\"poe_caps\":0");	/* as a USW-Lite-8-PoE reports ports 5-8 */
	}
#endif
	u_s(",\"speed\":");
	u_dec(u_pcp->mbps);
	u_counter(",\"rx_bytes\":", 0, 0, 0);
	u_counter(",\"tx_bytes\":", 1, 1, 1);
#if SOC_FAMILY != RTL836X
	/* The RTL836x packet counters are 32 bits: their sum would go back
	 * whenever one wraps, so only its 64-bit byte counters are sent */
	u_counter(",\"rx_packets\":", 2, 2, 4);
	u_counter(",\"tx_packets\":", 3, 5, 7);
#endif
	u_c('}');
}


/* The inform JSON up to its port table */
static void u_build(void)
{
	unifi.pos = HROOM + TNBU_HDR;
	u_s("{\"mac\":\"");
	u_ma = uip_ethaddr.addr;
	u_mac(':');
	u_s("\",\"serial\":\"");
	u_mac(0);
	u_s("\",\"model\":\"" UNIFI_MODEL "\",\"model_display\":\"" UNIFI_DISPLAY
	    "\",\"version\":\"");
	u_x(unifi.version);
	u_s("\",\"ip\":\"");
	u_ip((__xdata uint8_t *)uip_hostaddr);
	/* No netmask: a controller in the subnet it describes takes the switch
	 * for an L2 neighbour and adopts it over SSH, which this firmware lacks
	 * (Network 11.1.50); without one it adopts by inform */
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
	    "\"selfrun_beacon\":false,\"port_table\":[");
	u_ph = MACHINE_MIN_PORT;
	u_cnt = 0;	/* entries written */
#ifdef MACHINE_POE_PORTS
	u_poe = poe_admin_mask();
#endif
}

/* The LLDP neighbours, so the controller can draw the topology, with the
 * members a USW-Lite-8-PoE (7.5.15, libubnt) sends but mgmt_ips. Only
 * neighbours with a MAC chassis ID: the controller matches them to devices
 * by MAC. Budget: an entry is at most about 150 bytes (lldp_printable()
 * leaves nothing to escape), so LLDP_NB_MAX of them take about 2.4 KB;
 * with a port table of about 2.3 KB and the head, an inform stays under
 * 7 KB of the UNIFI_BUF_SIZE buffer. */
static __xdata struct lldp_neighbor *__xdata u_nb;

static void u_lldp(void)
{
	u_s("],\"lldp_table\":[");
	u_cnt = 0;
	for (u_nb = lldp_nb; u_nb < lldp_nb + LLDP_NB_MAX; u_nb++) {
		if (!u_nb->ttl || u_nb->chassis_subtype != LLDP_CHASSIS_ID_TLV_SUBTYPE
		    || u_nb->chassis_len != 6 || u_nb->port > MACHINE_MAX_PORT
		    || !machine.log_to_phys_port[u_nb->port])
			continue;
		if (u_cnt++)
			u_c(',');
		u_s("{\"chassis_id\":\"");
		u_ma = u_nb->chassis;
		u_mac(':');
		u_s("\",\"port_id\":\"");
		if (u_nb->port_id_mac) {
			u_ma = (__xdata const uint8_t *)u_nb->port_id;
			u_mac(':');
		} else {
			u_x(u_nb->port_id);
		}
		u_s("\",\"local_port_idx\":");
		u_dec(machine.log_to_phys_port[u_nb->port]);
		u_s(",\"local_port_name\":\"Port ");
		u_dec(machine.log_to_phys_port[u_nb->port]);
		u_s("\",\"is_wired\":true}");
	}
}

/* The next port table entry. Its hardware reads take a few milliseconds,
 * so one port per tick keeps the main loop responsive. Returns 1 once the
 * table is complete and the JSON closed. */
static uint8_t u_ports(void)
{
	while (u_ph <= MACHINE_MAX_PORT && !machine.log_to_phys_port[u_ph])
		u_ph++;
	if (u_ph <= MACHINE_MAX_PORT) {
		u_port_entry();
		u_ph++;
		return 0;
	}
	u_lldp();
	u_c(']');
	ua_caps();
	u_c('}');
	return 1;
}

/* The TNBU header in front of the JSON. Starts the encryption. */
static void u_seal(void)
{
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


/* CRC-16/CCITT of u_rec's bytes before .sum, into u_d[0] (high) and
 * u_d[1]. A partial erase only sets bits; a CRC sees any burst of up to
 * 16 bits, and passes other changes about 1 time in 65536. Bit by bit:
 * no table, no library division. */
static __xdata uint16_t u_crc;

static void u_sum(void)
{
	u_crc = 0xffff;
	for (u_d[5] = 0; u_d[5] < offsetof(struct u_rec, sum); u_d[5]++) {
		u_crc ^= (uint16_t)((__xdata uint8_t *)&u_rec)[u_d[5]] << 8;
		for (u_d[6] = 0; u_d[6] < 8; u_d[6]++) {
			if (u_crc & 0x8000)
				u_crc = u_crc << 1 ^ 0x1021;
			else
				u_crc <<= 1;
		}
	}
	u_d[0] = u_crc >> 8;
	u_d[1] = u_crc;
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
	memcpy(u_rec.version, unifi.version, UNIFI_VERSION_LEN);
	u_sum();
	u_rec.sum[0] = u_d[0];
	u_rec.sum[1] = u_d[1];
	u_rec.commit = 0xff;
	if (u_slot >= UNIFI_SLOTS) {
		u_sec ^= 1;
		u_slot = 0;
		flash_region.addr = u_addr();
		flash_sector_erase();
	}
	flash_region.addr = u_addr();
	flash_region.len = U_REC_LEN - 1;
	flash_write_bytes((__xdata uint8_t *)&u_rec);
	u_rec.commit = 0;
	flash_region.addr = u_addr() + offsetof(struct u_rec, commit);
	flash_region.len = 1;
	flash_write_bytes(&u_rec.commit);
	flash_region.addr = u_addr();
	flash_region.len = U_REC_LEN;
	flash_read_bulk((__xdata uint8_t *)&u_back);
	u_slot++;
	if (memcmp(&u_back, &u_rec, U_REC_LEN)) {
		unifi.flash_fails++;
		return 0;
	}
	unifi.flash_fails = 0;
	unifi.unsaved = 0;
	unifi.critical = 0;
	return 1;
}

/* Back to the default key: not adopted, no cfgversion. The boot counter
 * stays, so a later adoption with the same key repeats no IV. */
static void u_forget(void)
{
	/* Nothing to do once forgotten and saved; a save that failed is
	 * tried again */
	if (!unifi.adopted && !unifi.unsaved && !strcmp(unifi.version, u_default_version))
		return;
	memcpyc(unifi.key, u_default_key, 16);
	unifi.adopted = 0;
	unifi.keyed = 0;
	unifi.cfgver[0] = 0;
	/* A version a controller offered was for that controller */
	memcpyc(unifi.version, u_default_version, sizeof(u_default_version));
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

/* JSON whitespace */
static uint8_t u_ws(__xdata uint8_t c)
{
	if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
		return 1;
	return 0;
}

/* The offset of the closing quote of the string whose body starts at u_i,
 * skipping escapes, or u_n */
static uint16_t u_strend(void)
{
	for (; u_i < u_n && u_p[u_i] != '"'; u_i++)
		if (u_p[u_i] == '\\')
			u_i++;
	if (u_i > u_n)
		return u_n;
	return u_i;
}

/* A member of the reply's root object: the offset of its value, past the
 * colon and any whitespace, or 0xffff. Strings are skipped with their
 * escapes, and members of nested objects do not count. */
static __xdata uint8_t u_depth;
static __xdata uint16_t u_ks;

static uint16_t u_member(__code const char *name)
{
	u_depth = 0;
	for (u_i = 0; u_i < u_n; u_i++) {
		u_d[0] = u_p[u_i];
		if (u_d[0] == '{' || u_d[0] == '[') {
			u_depth++;
			continue;
		}
		if (u_d[0] == '}' || u_d[0] == ']') {
			u_depth--;
			continue;
		}
		if (u_d[0] != '"')
			continue;
		u_ks = ++u_i;
		u_i = u_strend();
		if (u_i >= u_n)
			return 0xffff;
		if (u_depth != 1)
			continue;
		for (u_j = u_i + 1; u_j < u_n && u_ws(u_p[u_j]); u_j++)
			;
		if (u_j >= u_n || u_p[u_j] != ':')
			continue;	/* a value, not a name */
		if ((uint16_t)(u_i - u_ks) != strlen(name) || !strstart(u_p + u_ks, name))
			continue;
		for (u_j++; u_j < u_n && u_ws(u_p[u_j]); u_j++)
			;
		return u_j;
	}
	return 0xffff;
}

/* A string member of the root object: the offset of its first character,
 * with u_me at its closing quote, or 0xffff */
static uint16_t u_string(__code const char *name)
{
	u_i = u_member(name);
	if (u_i == 0xffff || u_p[u_i] != '"')
		return 0xffff;
	u_ms = ++u_i;
	u_me = u_strend();
	if (u_me >= u_n)
		return 0xffff;	/* no closing quote */
	return u_ms;
}

/* A key=value line of mgmt_cfg, between u_ms and u_me: the offset of its
 * value, or 0xffff. The key must start the string or follow an escaped
 * newline, "\n" with its backslash not escaped itself, so that
 * "authkey=" matches neither the end of another key nor a value. */
static __xdata uint16_t u_b, u_ls;

static uint16_t u_line(__code const char *__xdata key)
{
	for (u_ls = u_ms; u_ls < u_me; u_ls = u_j) {
		u_j = u_find(key, u_ls, u_me);
		if (u_j == 0xffff)
			return 0xffff;
		u_i = u_j - strlen(key);
		if (u_i == u_ms)
			return u_j;
		if (u_i >= u_ms + 2 && u_p[u_i - 1] == 'n') {
			/* An odd number of backslashes before the n */
			u_b = 0;
			for (u_q = u_p + u_i - 2; u_q >= u_p + u_ms && *u_q == '\\'; u_q--)
				u_b++;
			if (u_b & 1)
				return u_j;
		}
	}
	return 0xffff;
}

/* The authkey at u_i: exactly 32 hex digits ending the line, into
 * unifi.key. */
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
	/* Then the end of mgmt_cfg or an escaped newline, nothing else */
	if (u_i + 32 < u_me && (u_i + 33 >= u_me || u_p[u_i + 32] != '\\' || u_p[u_i + 33] != 'n'))
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

/* The version at u_i, up to u_me: digits, letters, '.', '-' and '+'.
 * Returns 1 when it is valid and differs from the one reported. */
static uint8_t u_version(void)
{
	if (u_me <= u_i || u_me - u_i >= UNIFI_VERSION_LEN)
		return 0;
	for (u_j = u_i; u_j < u_me; u_j++) {
		u_d[0] = u_p[u_j] | 0x20;
		if ((u_d[0] < 'a' || u_d[0] > 'z') && (u_p[u_j] < '0' || u_p[u_j] > '9')
		    && u_p[u_j] != '.' && u_p[u_j] != '-' && u_p[u_j] != '+')
			return 0;
	}
	if (!memcmp(unifi.version, u_p + u_i, u_me - u_i) && !unifi.version[u_me - u_i])
		return 0;
	memcpy(unifi.version, u_p + u_i, u_me - u_i);
	unifi.version[u_me - u_i] = 0;
	return 1;
}

static void u_error(__code const char *msg);
#include "unifi_apply.c"

/* The tag checked out: act on the reply */
static __xdata uint8_t u_dirty;

static void u_decrypted(void)
{
	unifi.type[0] = 0;
	u_p = u_buf + unifi.hdr + TNBU_HDR;
	u_n = unifi.clen - TNBU_HDR - TAG_LEN;
	u_i = u_string("_type");
	if (u_i != 0xffff) {
		for (u_d[0] = 0; u_d[0] < UNIFI_TYPE_LEN - 1 && u_i < u_me; u_d[0]++)
			unifi.type[u_d[0]] = u_p[u_i++];
		unifi.type[u_d[0]] = 0;
	}
	unifi.err = 0;
	if (!strcmp(unifi.type, "noop")) {
		/* The controller paces the informs */
		u_i = u_member("interval");
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
		if (u_string("mgmt_cfg") != 0xffff) {
			/* A key is taken only while on the default one */
			u_i = u_line("authkey=");
			if (!unifi.adopted && u_i != 0xffff && u_authkey() && !u_is_default()) {
				unifi.adopted = 1;
				unifi.keyed = 0;
				unifi.boot++;
				unifi.booted = 1;
				unifi.critical = 1;
				u_dirty = 1;
			}
		}
		/* The provisioning cfgversion, else the one in mgmt_cfg */
		u_i = u_string("cfgversion");
		if (u_i == 0xffff && u_string("mgmt_cfg") != 0xffff)
			u_i = u_line("cfgversion=");
		if (unifi.adopted && u_i != 0xffff && u_cfgver()) {
			u_dirty = 1;
			/* A new configuration: apply what is selected */
			if (u_string("system_cfg") != 0xffff)
				ua_start();
		}
		/* A save that fails is tried again before the next inform */
		if (u_dirty)
			u_save();
	} else if (!strcmp(unifi.type, "upgrade")) {
		/* Report the offered version from now on, as if upgraded, so the
		 * controller stops offering it. Kept in flash with the adoption.
		 * Only from the controller that adopted the switch: anyone can
		 * encrypt with the default key. */
		u_i = u_string("version");
		if (unifi.adopted && u_i != 0xffff && u_version()) {
			u_dirty = 1;
			u_save();
		}
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
	if (ua_busy)
		unifi.phase = U_APPLY;
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
			unifi.critical = 1;
		}
		/* Under the authkey no inform goes out until the key and boot
		 * counter are in flash: the boot counter is part of the IV. A
		 * cfgversion that is not saved yet does not stop informs, and
		 * neither does anything under the public default key. */
		if (unifi.unsaved && !u_save() && unifi.adopted && unifi.critical) {
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
		unifi.phase = U_PORTS;
		return;
	case U_APPLY:
		if (!ua_run())
			return;
		ua_busy = 0;
		unifi.phase = U_IDLE;
		unifi.next = uptime_secs;
		return;
	case U_PORTS:
		if (!u_ports())
			return;
		u_seal();
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
	memcpyc(unifi.version, u_default_version, sizeof(u_default_version));
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
			flash_region.len = U_REC_LEN;
			flash_read_bulk((__xdata uint8_t *)&u_rec);
			for (u_d[4] = 0; u_d[4] < U_REC_LEN; u_d[4]++)
				if (((__xdata uint8_t *)&u_rec)[u_d[4]] != 0xff)
					break;
			if (u_d[4] == U_REC_LEN)
				continue;	/* erased */
			u_free[u_d[2]] = u_d[3] + 1;
			if (!strstart(u_rec.magic, u_magic) || u_rec.commit)
				continue;
			u_sum();
			if (u_rec.sum[0] != u_d[0] || u_rec.sum[1] != u_d[1])
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
			memcpy(unifi.version, u_rec.version, UNIFI_VERSION_LEN);
			unifi.version[UNIFI_VERSION_LEN - 1] = 0;
		}
	}
	u_seq = u_best;
	u_slot = u_free[u_sec];
}


/* A factory reset leaves the controller too. The reset follows at once,
 * so the save is tried as often as u_save() allows. */
void unifi_reset(void) __banked
{
	u_forget();
	while (unifi.unsaved && unifi.flash_fails < UNIFI_FLASH_TRIES)
		u_save();
	if (unifi.unsaved)
		print_string("Error: the UniFi adoption is not forgotten, it returns after the reset\n");
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
	print_string(" s\nShown as " UNIFI_DISPLAY " (" UNIFI_MODEL "), version ");
	print_string_x(unifi.version);
	print_string("\nInforms sent ");
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
	if (unifi.unsaved)
		print_string(", state not saved to flash");
	write_char('\n');
	ua_show();
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
		cmd_nosave = 1;	/* a status query, polled by the web page */
		return;
	}
	if (cmd_words_len == 2 && (u_word("on") || u_word("off"))) {
		u_d[0] = u_word("on");
		/* LLDP describes the switch as a UniFi one while on: neighbours
		 * get the new frames at once, the old entry ages out */
		if (unifi.enabled != u_d[0])
			lldp_resend();
		unifi.enabled = u_d[0];
		u_stop();
		return;
	}
	if (cmd_words_len >= 3 && u_word("apply")) {
		ua_set();
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
	u_error("unifi [on|off|inform <url>|apply <areas>|forget|selftest]\n");
}
