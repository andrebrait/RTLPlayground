/* unifi.c, the inform client, with the real AES-GCM and a mocked uIP: the
 * inform URL parser, the request (HTTP header, TNBU header, encrypted JSON)
 * checked by decrypting it, sending it in segments with a retransmission,
 * and replies: 200 with an encrypted noop in pieces, with and without
 * Content-Length, 404, a bad tag, an oversized reply, and "unifi off" while
 * a reply is coming in. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../crypto/aes_gcm.c"
#include "../unifi.c"

/* What unifi.c needs from the rest of the firmware */
__xdata uint32_t uptime_secs;
__xdata uint8_t sfr_data[4];
__xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
__xdata uint8_t cmd_words_len;
__xdata uint8_t cmd_words_b[15];
__xdata uint8_t err_status;
__xdata char hostname[24] = "sw\"1";
__xdata struct uip_eth_addr uip_ethaddr = { { 0x02, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e } };
__xdata uip_ipaddr_t uip_hostaddr, uip_netmask;
__xdata struct dns_state dns_state;
struct uip_conn *uip_conn;
__xdata u8_t uip_flags;
void *uip_appdata;
__xdata u16_t uip_len;

static struct uip_conn fake_conn;
static int connects;
static uint8_t connect_addr[4];
static uint16_t connect_port;
static uint8_t sent[2048];
static uint16_t sent_len;
static int selftests;

void dns_lookup(void)
{
	static const uint8_t a[4] = { 10, 0, 0, 1 };
	memcpy(dns_state.addr, a, 4);
	dns_state.status = DNS_DONE;
}

struct uip_conn *uip_connect(uip_ipaddr_t *ripaddr, u16_t rport)
{
	connects++;
	memcpy(connect_addr, ripaddr, 4);
	connect_port = rport;
	return &fake_conn;
}

void uip_send(const void *data, uint16_t len)
{
	memcpy(sent, data, len);
	sent_len = len;
}

void get_random_32(void)
{
	static uint8_t n;
	for (int i = 0; i < 4; i++)
		sfr_data[i] = n++;
}

void memcpyc(uint8_t *dst, const uint8_t *src, uint16_t len) { memcpy(dst, src, len); }

bool strstart(const uint8_t *a, const uint8_t *b)
{
	return !strncmp((const char *)a, (const char *)b, strlen((const char *)b));
}

void gcm_selftest(void) { selftests++; }

/* The UniFi state sector */
struct flash_region_t flash_region;
static uint8_t sector[8192];	/* both UniFi sectors */
static int erases, saves, cut, broken, no_commit, partial;	/* cut: the next write stops after that many bytes */
static uint8_t *at(void)
{
	uint32_t o = flash_region.addr - UNIFI_STATE_START;
	if (flash_region.addr < UNIFI_STATE_START || o + flash_region.len > sizeof(sector)) {
		printf("FAIL flash access outside the UniFi sector: %x+%u\n", flash_region.addr, flash_region.len);
		exit(1);
	}
	return sector + o;
}
void flash_sector_erase(void)
{
	if (flash_region.addr != UNIFI_STATE_START && flash_region.addr != UNIFI_STATE_START + 4096) {
		printf("FAIL erase outside the UniFi sectors\n");
		exit(1);
	}
	if (partial) {
		/* Cut short: bits only went up, here the top byte of each seq */
		for (int i = 0; i < 64; i++)
			at()[i * 64 + 7] = 0xff;
		partial = 0;
	} else {
		memset(at(), 0xff, 4096);
	}
	erases++;
}
void flash_write_bytes(uint8_t *p)
{
	uint8_t *d = at();
	if (flash_region.len == 1) {	/* the commit byte */
		if (!no_commit && !broken)
			*d &= *p;
		return;
	}
	saves++;
	if (broken)
		return;
	for (int i = 0; i < flash_region.len && (!cut || i < cut); i++)
		d[i] &= p[i];	/* NOR flash only clears bits */
	cut = 0;
}
void flash_read_bulk(uint8_t *d)
{
	memcpy(d, at(), flash_region.len);
}

/* The complete record with the highest sequence number. Layout: magic 0,
 * seq 4, boot 8, key 12, cfgversion 28, sum 45, commit 47. */
static const uint8_t *last_rec(void)
{
	const uint8_t *r = sector;
	uint32_t best = 0, seq;
	for (int i = 0; i < 128; i++) {
		const uint8_t *s = sector + i * 64;
		memcpy(&seq, s + 4, 4);
		if (!memcmp(s, "UNF3", 4) && !s[47] && seq > best) {
			best = seq;
			r = s;
		}
	}
	return r;
}

static char out[512];
static size_t out_len;
void write_char(char c) { if (out_len < sizeof out - 1) out[out_len++] = c; out[out_len] = 0; }
void print_string(__code const char *p) { while (*p) write_char(*p++); }
void print_string_x(__xdata char *p) { while (*p) write_char(*p++); }
void itoa_short(uint16_t v) { char s[8]; snprintf(s, sizeof s, "%u", v); print_string(s); }
void print_ip(__xdata uint8_t *ip) { char s[16]; snprintf(s, sizeof s, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); print_string(s); }

static int failures;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

/* A command line, split into words as cmd_tokenize() does */
static void cmd(const char *s)
{
	err_status = ERR_OK;
	memset(cmd_buffer, 0, sizeof(cmd_buffer));
	strcpy((char *)cmd_buffer, s);
	cmd_words_len = 0;
	for (int i = 0; s[i]; i++)
		if (s[i] != ' ' && (!i || s[i - 1] == ' '))
			cmd_words_b[cmd_words_len++] = i;
	out_len = 0;
	out[0] = 0;
	unifi_parse();
}

static void appcall(uint8_t flags, const void *data, uint16_t len)
{
	static uint8_t rx[2048];
	if (len)
		memcpy(rx, data, len);
	uip_appdata = rx;
	uip_len = len;
	uip_flags = flags;
	uip_conn = &fake_conn;
	sent_len = 0;
	unifi_appcall();
}

/* Ticks until the phase leaves the crypto and resolver phases */
static void ticks(void)
{
	for (int i = 0; i < 1000; i++) {
		unifi_tick();
		if (unifi.phase == U_IDLE || unifi.phase == U_SEND || unifi.phase == U_RECV)
			return;
	}
}

/* The request as sent */
static uint8_t req[2048];
static uint16_t req_len;

static void keep_sent(void)
{
	if (req_len + sent_len > sizeof(req)) {
		printf("FAIL the request is larger than the test's copy\n");
		exit(1);
	}
	memcpy(req + req_len, sent, sent_len);
	req_len += sent_len;
}

/* Acknowledges segments until the request is taken, or with keep_last
 * until only the last one is outstanding. Exits on an ACK that makes no
 * progress, so that a regression cannot hang the suite. */
static void acks(int keep_last)
{
	for (int i = 0; i < 100 && unifi.phase == U_SEND; i++) {
		if (keep_last && unifi.off + unifi.sent >= unifi.end)
			return;
		uint16_t off = unifi.off;
		appcall(UIP_ACKDATA, 0, 0);
		keep_sent();
		if (unifi.phase == U_SEND && unifi.off == off)
			break;
	}
	if (unifi.phase == U_SEND && !(keep_last && unifi.off + unifi.sent >= unifi.end)) {
		printf("FAIL the request was not taken: an ACK made no progress, or over 100 segments\n");
		exit(1);
	}
}

/* Runs an inform up to the reply */
static void inform(void)
{
	if (uptime_secs < unifi.next)
		uptime_secs = unifi.next;
	ticks();
	req_len = 0;
	appcall(UIP_CONNECTED, 0, 0);
	keep_sent();
	acks(0);
}

/* MD5("ubnt"), kept apart from u_default_key so that a change there fails */
static const uint8_t key[16] = {
	0xba, 0x86, 0xf2, 0xbb, 0xe1, 0x07, 0xc7, 0xc5,
	0x7e, 0xb5, 0xf2, 0x69, 0x07, 0x75, 0xc7, 0x12
};

/* An encrypted TNBU reply around json, after an HTTP header */
static const uint8_t *cur_key = key;

static uint16_t reply(uint8_t *p, const char *json, int with_clen, int tamper)
{
	uint8_t pkt[1024], k[16];
	uint16_t n = strlen(json), hl;

	memset(pkt, 0, 40);
	memcpy(pkt, "TNBU", 4);
	memcpy(pkt + 8, uip_ethaddr.addr, 6);
	pkt[15] = 0x09;
	for (int i = 0; i < 16; i++)
		pkt[16 + i] = 0xa0 + i;
	pkt[35] = 1;
	pkt[38] = (n + 16) >> 8;
	pkt[39] = n + 16;
	memcpy(pkt + 40, json, n);
	memcpy(k, cur_key, 16);
	gcm_setkey(k);
	gcm_start(pkt + 16, 0);
	gcm_aad(pkt, 40);
	gcm_crypt(pkt + 40, n);
	gcm_finish();
	memcpy(pkt + 40 + n, gcm.blk, 16);
	if (tamper)
		pkt[40]++;
	if (with_clen)
		hl = sprintf((char *)p, "HTTP/1.1 200 OK\r\nContent-Type: application/x-binary\r\n"
			     "content-LENGTH: %u\r\n\r\n", n + 56);
	else
		hl = sprintf((char *)p, "HTTP/1.1 200 OK\r\n\r\n");
	memcpy(p + hl, pkt, n + 56);
	return hl + n + 56;
}

/* Delivers a reply in 50-byte pieces */
static void deliver(const uint8_t *p, uint16_t len)
{
	for (uint16_t o = 0; o < len && unifi.phase == U_RECV; o += 50)
		appcall(UIP_NEWDATA, p + o, len - o > 50 ? 50 : len - o);
}

static void test_url(void)
{
	cmd("unifi inform http://ctl.example:8443/a/inform");
	check(!strcmp(unifi.host, "ctl.example") && unifi.port == 8443 && !strcmp(unifi.path, "/a/inform"),
	      "url: host, port and path");
	cmd("unifi inform http://10.0.0.1");
	check(!strcmp(unifi.host, "10.0.0.1") && unifi.port == 8080 && !strcmp(unifi.path, "/inform"),
	      "url: default port and path");
	static const char *bad[] = {
		"unifi inform 10.0.0.2", "unifi inform http://", "unifi inform http://h:0",
		"unifi inform http://h:65536", "unifi inform http://h:80x", "unifi inform http://h/a\"b",
		"unifi inform http://0123456789012345678901234567890123456789012345678/",
		"unifi inform http://a\"b/", "unifi inform http://a\\\\b/",
	};
	int same = 1;
	unifi.next = 77;
	for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
		cmd(bad[i]);
		same &= unifi.next == 77 && !unifi.abort;
		same &= !strcmp(unifi.host, "10.0.0.1") && unifi.port == 8080 && !strcmp(unifi.path, "/inform")
			&& strstr(out, "Error") != 0 && err_status == ERR_INVALID_ARGUMENT;
	}
	check(same, "url: a bad URL is refused and changes nothing");
	cmd("unifi inform http://h:65535/");
	check(unifi.port == 65535 && !strcmp(unifi.path, "/"), "url: port 65535 and root path");
	cmd("unifi inform http://10.0.0.1");
}

static void test_request(void)
{
	uint8_t ip[4] = { 10, 0, 0, 7 }, mask[4] = { 255, 255, 0, 0 };
	memcpy(uip_hostaddr, ip, 4);
	memcpy(uip_netmask, mask, 4);
	fake_conn.mss = 300;
	uptime_secs = 1234;
	cmd("unifi on");
	inform();
	check(connects == 1 && !memcmp(connect_addr, dns_state.addr, 4) && connect_port == HTONS(8080),
	      "request: connects to the resolved address and port");
	check(unifi.phase == U_RECV && req_len == unifi.end - unifi.start && !memcmp(req, u_buf + unifi.start, req_len),
	      "request: sent in segments, in order, then waits for the reply");

	char *body = strstr((char *)req, "\r\n\r\n");
	unsigned clen = 0;
	char *cl = strstr((char *)req, "Content-Length: ");
	if (cl)
		clen = atoi(cl + 16);
	static const char start[] = "POST /inform HTTP/1.1\r\nHost: 10.0.0.1:8080\r\n";
	check(!strncmp((char *)req, start, sizeof(start) - 1) && body
	      && clen == req_len - ((uint8_t *)body + 4 - req),
	      "request: HTTP header with the body's Content-Length");
	if (!body || clen < 56 || clen > 2048 || clen != req_len - ((uint8_t *)body + 4 - req))
		return;	/* nothing below can read a request that is not framed */
	uint8_t *p = (uint8_t *)body + 4;
	uint16_t plen = (uint16_t)(p[38] << 8 | p[39]);
	check(!memcmp(p, "TNBU", 4) && !memcmp(p + 8, uip_ethaddr.addr, 6) && p[15] == 0x09 && p[35] == 1
	      && plen == clen - 40,
	      "request: TNBU header, GCM flags, payload length with the tag");
	if (plen != clen - 40)
		return;

	uint8_t k[16], copy[2048];
	memcpy(copy, p, clen);
	memcpy(k, key, 16);
	gcm_setkey(k);
	gcm_start(copy + 16, 1);
	gcm_aad(copy, 40);
	gcm_crypt(copy + 40, plen - 16);
	gcm_finish();
	check(!memcmp(gcm.blk, copy + 40 + plen - 16, 16), "request: the tag verifies under the default key");
	copy[40 + plen - 16] = 0;
	const char *want = "{\"mac\":\"02:1a:2b:3c:4d:5e\",\"serial\":\"021A2B3C4D5E\",\"model\":\"" UNIFI_MODEL "\","
		"\"model_display\":\"" UNIFI_DISPLAY "\",\"version\":\"" UNIFI_VERSION "\",\"ip\":\"10.0.0.7\","
		"\"netmask\":\"255.255.0.0\",\"hostname\":\"sw\\\"1\",\"inform_url\":\"http://10.0.0.1:8080/inform\","
		"\"uptime\":1234,\"cfgversion\":\"?\",\"default\":true,\"state\":1,\"fw_caps\":0,"
		"\"isolated\":false,\"locating\":false,\"selfrun_beacon\":false,\"port_table\":[]}";
	check(!strcmp((char *)copy + 40, want), "request: the decrypted JSON");
	if (strcmp((char *)copy + 40, want))
		printf("  got %s\n", copy + 40);

	static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
	appcall(UIP_NEWDATA, nf, sizeof(nf) - 1);

	/* The next IV carries the next message counter */
	uint8_t iv[16];
	memcpy(iv, p + 16, 16);
	inform();
	p = (uint8_t *)strstr((char *)req, "\r\n\r\n") + 4;
	check(p[31] == (uint8_t)(iv[15] + 1) && !memcmp(p + 28, iv + 12, 3), "request: the IV counter advances");
	appcall(UIP_NEWDATA, nf, sizeof(nf) - 1);

	/* Another client takes the resolver over before its answer is read */
	uptime_secs = unifi.next;
	unifi_tick();
	strcpy(dns_state.name, "pool.ntp.org");
	unifi_tick();
	check(unifi.phase == U_IDLE && connects == 2, "resolve: an answer for another name is not used");
	ticks();
	appcall(UIP_CONNECTED, 0, 0);
	acks(0);
	appcall(UIP_NEWDATA, nf, sizeof(nf) - 1);
	check(connects == 3 && unifi.status == 404, "resolve: then it asks again");
}

static void test_rexmit(void)
{
	uptime_secs = unifi.next;
	ticks();
	check(unifi.phase == U_SEND, "send: the next inform after one interval");
	appcall(UIP_CONNECTED, 0, 0);
	uint8_t first[300];
	uint16_t n = sent_len;
	memcpy(first, sent, n);
	fake_conn.mss = 100;	/* a smaller window since the segment went out */
	appcall(UIP_REXMIT, 0, 0);
	fake_conn.mss = 300;
	check(sent_len == n && !memcmp(sent, first, n), "send: a retransmission resends the same bytes");
	appcall(UIP_ACKDATA | UIP_NEWDATA, "HTTP/1.1 400 Bad", 16);
	check(unifi.err == UE_HTTP && uip_flags == UIP_ABORT && !unifi.conn,
	      "send: a reply before the request is taken fails the inform");
	uptime_secs = unifi.next;
	ticks();
	appcall(UIP_CONNECTED, 0, 0);
	cmd("unifi off");
	appcall(UIP_POLL, 0, 0);
	int before = connects;
	ticks();
	check(uip_flags == UIP_ABORT && unifi.phase == U_IDLE && !unifi.conn && connects == before,
	      "unifi off: the connection is reset and no inform starts");
	cmd("unifi on");
}

static void test_replies(void)
{
	static uint8_t r[2048];
	uint16_t n;
	const char *noop = "{\"_type\":\"noop\",\"interval\":10,\"server_time_in_utc\":\"1\"}";

	inform();
	n = reply(r, noop, 1, 0);
	deliver(r, n);
	check(uip_flags == UIP_CLOSE && unifi.phase == U_REPLY, "reply: complete at Content-Length, then closed");
	ticks();
	check(unifi.phase == U_IDLE && unifi.status == 200 && !unifi.err && !strcmp(unifi.type, "noop")
	      && unifi.next == uptime_secs + 10,
	      "reply: 200 decrypted and its _type kept");

	inform();
	n = reply(r, noop, 0, 0);
	deliver(r, n);
	check(unifi.phase == U_RECV, "reply: without Content-Length it waits for the close");
	appcall(UIP_CLOSE, 0, 0);
	ticks();
	check(!unifi.err && !strcmp(unifi.type, "noop"), "reply: without Content-Length it ends at the close");

	inform();
	n = sprintf((char *)r, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
	deliver(r, n);
	check(unifi.phase == U_IDLE && unifi.status == 404 && !unifi.err && !unifi.type[0],
	      "reply: 404 while the device waits for adoption");

	/* The server closes right after writing: the FIN rides on the data */
	inform();
	appcall(UIP_NEWDATA | UIP_CLOSE, r, n);
	check(uip_flags == (UIP_NEWDATA | UIP_CLOSE) && unifi.phase == U_IDLE && unifi.status == 404 && !unifi.err,
	      "reply: a 404 that carries the FIN");

	/* The reply arrives with the acknowledgement of the last request bytes */
	uptime_secs = unifi.next;
	ticks();
	appcall(UIP_CONNECTED, 0, 0);
	acks(1);
	appcall(UIP_ACKDATA | UIP_NEWDATA, r, n);
	check(unifi.phase == U_IDLE && unifi.status == 404 && !unifi.err,
	      "reply: data together with the last acknowledgement");

	inform();
	n = reply(r, noop, 1, 0);
	deliver(r, n - 100);
	appcall(UIP_CLOSE, 0, 0);
	check(unifi.err == UE_HTTP && unifi.phase == U_IDLE && !unifi.conn && unifi.status == 200,
	      "reply: a close before Content-Length is reached fails");

	inform();
	appcall(UIP_CLOSE, 0, 0);
	check(unifi.err == UE_CONN && !unifi.status, "reply: a close without any reply is a connection failure");

	inform();
	n = reply(r, noop, 1, 0);
	appcall(UIP_NEWDATA | UIP_CLOSE, r, n);
	ticks();
	check(!unifi.err && !strcmp(unifi.type, "noop"), "reply: a 200 that carries the FIN");

	inform();
	n = reply(r, noop, 1, 1);
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TAG && !unifi.type[0] && unifi.phase == U_IDLE, "reply: a bad tag is refused");

	inform();
	n = sprintf((char *)r, "HTTP/1.1 200 OK\r\nX-A: 1\r\ncontent\rlength: 5\r\nContent-Length: 99999\r\n\r\n");
	deliver(r, n);
	check(unifi.err == UE_HTTP && uip_flags == UIP_ABORT && !unifi.conn, "reply: a reply larger than the buffer is reset");

	inform();
	n = reply(r, noop, 1, 0);
	(strstr((char *)r, "\r\n\r\n") + 4)[39] ^= 1;	/* the TNBU length field */
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TNBU, "reply: a TNBU length that disagrees with Content-Length is refused");

	inform();
	n = reply(r, noop, 1, 0);
	memcpy(strstr((char *)r, "\r\n\r\n") + 4, "TNBV", 4);
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TNBU, "reply: a bad TNBU magic is refused");

	inform();
	n = reply(r, noop, 1, 0);
	(strstr((char *)r, "\r\n\r\n") + 4)[15] = 0x0b;	/* zlib */
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TNBU, "reply: a compressed reply is refused");

	inform();
	n = reply(r, "{\"_type\":\"averyveryverylongtype\"}", 1, 0);
	deliver(r, n);
	ticks();
	check(!unifi.err && !strcmp(unifi.type, "averyveryverylo"), "reply: a long _type is cut to the field");

	inform();
	n = sprintf((char *)r, "HTTP/1.1 20x OK\r\nContent-Length: 0\r\n\r\n");
	deliver(r, n);
	check(unifi.err == UE_HTTP && !unifi.status, "reply: a status that is not three digits is refused");

	inform();
	deliver((const uint8_t *)"HTTP/1\r\n\r\n", 11);
	check(unifi.err == UE_HTTP, "reply: a status line too short is refused");

	/* More data than the buffer holds, without a header end */
	inform();
	memset(r, 'x', 1000);
	for (int i = 0; i < 11 && unifi.phase == U_RECV; i++)
		appcall(UIP_NEWDATA, r, 1000);
	check(unifi.err == UE_HTTP && !unifi.conn, "reply: data beyond the buffer is refused");

	/* Events for another connection, such as the previous inform's close */
	inform();
	struct uip_conn other;
	uip_conn = &other;
	uip_flags = UIP_CLOSE;
	unifi_appcall();
	check(unifi.phase == U_RECV && unifi.conn == &fake_conn, "appcall: another connection's events are ignored");
	deliver(r, sprintf((char *)r, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"));

	cmd("unifi selftest");
	check(selftests == 1 && !unifi.keyed, "selftest: runs while idle and forces a new key setup");
}

/* The JSON of the request last sent, decrypted with cur_key; NULL if its
 * tag does not verify */
static uint8_t req_iv[16];

static const char *req_json(void)
{
	static uint8_t copy[2048];
	char *body = strstr((char *)req, "\r\n\r\n");
	if (!body)
		return NULL;
	uint8_t *p = (uint8_t *)body + 4, k[16];
	uint16_t plen = (uint16_t)(p[38] << 8 | p[39]);
	if (plen < 16 || plen > sizeof(copy) - 41)
		return NULL;
	memcpy(copy, p, 40 + plen);
	memcpy(req_iv, copy + 16, 16);
	memcpy(k, cur_key, 16);
	gcm_setkey(k);
	gcm_start(copy + 16, 1);
	gcm_aad(copy, 40);
	gcm_crypt(copy + 40, plen - 16);
	gcm_finish();
	if (memcmp(gcm.blk, copy + 40 + plen - 16, 16))
		return NULL;
	copy[40 + plen - 16] = 0;
	return (const char *)copy + 40;
}

static void answer(const char *json)
{
	static uint8_t r[2048];
	uint16_t n = reply(r, json, 1, 0);
	deliver(r, n);
	ticks();
}

static void test_adopt(void)
{
	static const uint8_t newkey[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
	};
	const char *j;

	cur_key = key;
	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"capability=notif\\nauthkey=00112233445566778899"
	       "AABBCCDDEEFF\\ncfgversion=cfe5c9df7757b6f5\\nuse_aes_gcm=true\\n\",\"server_time_in_utc\":\"1\"}");
	check(unifi.adopted && !memcmp(unifi.key, newkey, 16) && !strcmp(unifi.cfgver, "cfe5c9df7757b6f5")
	      && unifi.next == uptime_secs && !unifi.keyed,
	      "adopt: the authkey and cfgversion are taken, and the next inform is due");
	check(saves == 1 && !erases && !memcmp(sector, "UNF3", 4) && !memcmp(sector + 12, newkey, 16)
	      && !strcmp((char *)sector + 28, "cfe5c9df7757b6f5"),
	      "adopt: the key and cfgversion are saved to flash");

	cur_key = newkey;
	inform();
	j = req_json();
	check(j && strstr(j, "\"cfgversion\":\"cfe5c9df7757b6f5\",\"default\":false,\"state\":4,")
	      && req_iv[11] == unifi.boot && unifi.boot == 1,
	      "adopt: the next inform uses the new key, echoes the cfgversion and counts the boot");

	answer("{\"_type\":\"setparam\",\"cfgversion\":\"0123456789abcdef\",\"system_cfg\":\"x=1\","
	       "\"mgmt_cfg\":\"authkey=ffffffffffffffffffffffffffffffff\\ncfgversion=0123456789abcdef\\n\"}");
	check(!strcmp(unifi.cfgver, "0123456789abcdef") && !memcmp(unifi.key, newkey, 16) && saves == 2
	      && last_rec() == sector + 64,
	      "provision: the new cfgversion is kept; a second authkey is ignored");

	inform();
	answer("{\"_type\":\"noop\",\"interval\":18,\"server_time_in_utc\":\"1\"}");
	check(unifi.interval == 18 && unifi.next == uptime_secs + 18 && saves == 2,
	      "noop: the controller's interval paces the informs");
	inform();
	answer("{\"_type\":\"noop\",\"interval\":2}");
	check(unifi.interval == 18, "noop: an interval below 5 s is ignored");
	inform();
	answer("{\"_type\":\"noop\",\"interval\":36000}");
	check(unifi.interval == 18, "noop: an interval above 3600 s is ignored, whatever its length");
	inform();
	answer("{\"_type\":\"upgrade\",\"version\":\"9.9.9\"}");
	check(unifi.next == uptime_secs + 18 && !strcmp(unifi.type, "upgrade"),
	      "reply: a command not handled waits the interval");

	/* A reboot: the adoption comes back, and the boot is counted again */
	unifi_init();
	cmd("unifi inform http://10.0.0.1");
	cmd("unifi on");
	check(unifi.adopted && !memcmp(unifi.key, newkey, 16) && !strcmp(unifi.cfgver, "0123456789abcdef")
	      && unifi.boot == 1,
	      "reboot: the adoption is loaded from flash");
	inform();
	j = req_json();
	check(j && unifi.boot == 2 && req_iv[11] == 2 && saves == 3 && strstr(j, "\"default\":false"),
	      "reboot: the first inform counts the boot in flash and in the IV");

	answer("{\"_type\":\"setdefault\"}");
	check(!unifi.adopted && !memcmp(unifi.key, key, 16) && !unifi.cfgver[0] && saves == 4
	      && !memcmp(last_rec() + 12, key, 16) && last_rec()[8] == 2 && unifi.next == uptime_secs,
	      "setdefault: back to the default key, saved");

	cur_key = key;
	inform();
	j = req_json();
	check(j && strstr(j, "\"cfgversion\":\"?\",\"default\":true,\"state\":1,"),
	      "setdefault: the next inform is a pending device's again");
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"authkey=0011223344556677889900aabbccddee\\n\"}");
	check(unifi.adopted && unifi.key[0] == 0x00 && unifi.key[15] == 0xee, "adopt: again after setdefault");
	cmd("unifi forget");
	check(!unifi.adopted && !memcmp(unifi.key, key, 16) && !memcmp(last_rec() + 12, key, 16),
	      "forget: the command goes back to the default key");

	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"authkey=0011223344556677889900aabbccdde\\n\"}");
	check(!unifi.adopted && !memcmp(unifi.key, key, 16), "adopt: a key that is not 32 hex digits is ignored");
	inform();
	check(unifi.phase == U_RECV, "adopt: the next inform after a refused key");
	answer("{\"_type\":\"setparam\",\"system_cfg\":\"authkey=00112233445566778899aabbccddeeff\","
	       "\"mgmt_cfg\":\"led_enabled=true\\n\"}");
	check(!unifi.adopted, "adopt: an authkey outside mgmt_cfg is ignored");
	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"authkey=00112233445566778899aabbccddeeff0\\n\"}");
	check(!unifi.adopted, "adopt: a key of 33 hex digits is ignored");
	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"x_authkey=ffffffffffffffffffffffffffffffff\\n"
	       "authkey=00112233445566778899aabbccddeeff\\n\"}");
	check(unifi.adopted && !memcmp(unifi.key, newkey, 16), "adopt: only a line that starts with authkey= counts");

	/* A power cut while a record is written: the previous one counts */
	int before = saves;
	cut = 20;
	cmd("unifi forget");
	unifi_init();
	check(saves == before + 1 && unifi.adopted && !memcmp(unifi.key, newkey, 16),
	      "flash: a record cut short is skipped and the previous one used");

	/* A full sector is erased, and the newest record survives */
	for (int i = 0; i < 70; i++) {
		unifi.boot++;
		u_save();
	}
	uint32_t b = unifi.boot;
	unifi_init();
	check(erases == 1 && unifi.boot == b && unifi.adopted && u_sec == 1,
	      "flash: when a sector is full, the other one is erased and used");

	/* A record whose commit byte never got written does not count */
	no_commit = 1;
	unifi.boot++;
	u_save();
	no_commit = 0;
	unifi_init();
	check(unifi.boot == b && unifi.adopted, "flash: a record without its commit byte is skipped");

	/* The write after an erase fails: the full sector stays */
	while (u_slot < 64) {
		unifi.boot++;
		u_save();
	}
	b = unifi.boot;
	broken = 1;
	unifi.boot++;
	check(!u_save() && unifi.unsaved, "flash: a save that does not read back is reported");
	broken = 0;
	unifi_init();
	check(erases == 2 && unifi.boot == b && unifi.adopted && u_sec == 1,
	      "flash: a failed write after an erase keeps the last good record in the other sector");

	/* A power cut during that erase: stale records with a raised seq stay.
	 * Fill sector 0, so that the next save erases sector 1, which still
	 * holds older committed records. */
	while (!(u_sec == 0 && u_slot == 64)) {
		unifi.boot++;
		u_save();
	}
	check(last_rec()[47] == 0 && sector[4096 + 47] == 0, "flash: both sectors hold committed records");
	b = unifi.boot;
	partial = 1;
	broken = 1;
	unifi.boot++;
	u_save();
	broken = 0;
	unifi_init();
	check(unifi.boot == b && u_sec == 0,
	      "flash: an erase cut short brings no stale record back");

	unifi_reset();
	unifi_init();
	check(!unifi.adopted && unifi.boot == b, "factory reset: back to the default key, the boot counter kept");
}

/* Bounds of the reply parser, and a flash that does not take writes */
static void test_bounds(void)
{
	static const uint8_t newkey[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
	};

	cur_key = key;
	cmd("unifi inform http://10.0.0.1");
	cmd("unifi on");
	inform();
	answer("{\"_type\":\"setparam\",\"cfgversion\":\"0123456789abcdef\","
	       "\"mgmt_cfg\":\"led_enabled=true\\n\",\"x\":\"authkey=00112233445566778899aabbccddeeff\"}");
	check(!unifi.adopted && !unifi.cfgver[0],
	      "bounds: an authkey after mgmt_cfg ends is ignored, and no cfgversion is kept while pending");
	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"authkey=00112233445566778899aabb\"ccddeeff}");
	check(!unifi.adopted, "bounds: an authkey cut by the end of mgmt_cfg is ignored");
	inform();
	answer("{\"_type\":\"setparam\",\"mgmt_cfg\":\"authkey=ba86f2bbe107c7c57eb5f2690775c712\\n\"}");
	check(!unifi.adopted, "bounds: the default key is not an adoption");
	inform();
	answer("{\"_type\":\"setparam\",\"cfgversion\":\"0123456789abcdef0123\","
	       "\"mgmt_cfg\":\"authkey=00112233445566778899aabbccddeeff\\n\"}");
	check(unifi.adopted && !strcmp(unifi.cfgver, "0123456789abcdef"),
	      "bounds: a cfgversion longer than 16 digits is cut to the field");
	cur_key = newkey;
	inform();
	answer("{\"_type\":\"noop\",\"interval\":3601}");
	check(unifi.interval == 10, "bounds: an interval of 3601 s is ignored");
	inform();
	answer("{\"_type\":\"noop\",\"interval\":65541}");
	check(unifi.interval == 10, "bounds: an interval that would wrap 16 bits is ignored");

	/* A flash that does not take writes: no inform under the authkey */
	broken = 1;
	unifi_init();
	cmd("unifi inform http://10.0.0.1");
	cmd("unifi on");
	uint32_t b = unifi.boot;
	int c = connects;
	uptime_secs += 100;
	ticks();
	check(unifi.err == UE_FLASH && connects == c && unifi.phase == U_IDLE && unifi.unsaved,
	      "flash: the boot counter must reach flash before an inform under the authkey");
	broken = 0;
	inform();
	check(connects == c + 1 && unifi.boot == b + 1 && !unifi.unsaved && req_json() && req_iv[11] == (uint8_t)(b + 1),
	      "flash: once it is saved, the inform goes out with that boot counter");

	/* The failed save above left an erased slot before the good one */
	unifi_init();
	check(unifi.boot == b + 1 && u_slot == 4, "flash: a record after an erased slot is found, and writing goes on after it");

	/* A flash that never takes writes is tried three times, then left alone */
	broken = 1;
	unifi_init();
	cmd("unifi inform http://10.0.0.1");
	cmd("unifi on");
	int before = saves;
	for (int i = 0; i < 6; i++) {
		uptime_secs += 100;
		ticks();
	}
	check(saves == before + 3 && unifi.err == UE_FLASH && connects == c + 1,
	      "flash: a broken flash is not worn by endless retries");
	cmd("unifi forget");
	c = connects;
	uptime_secs += 100;
	ticks();
	check(!unifi.adopted && unifi.unsaved && connects == c + 1,
	      "flash: a failed save does not stop informs under the public default key");
	broken = 0;
}

int main(void)
{
	memset(sector, 0xff, sizeof(sector));
	unifi_init();
	test_url();
	test_request();
	test_rexmit();
	test_replies();
	cmd("unifi");
	check(strstr(out, "informs sent 23, last reply 404, failed 11,") != 0, "show: the informs and failures counted");
	unifi.count = 999999;
	unifi.fails = 0xffffffff;
	cmd("unifi");
	check(strstr(out, "informs sent 999999,") && strstr(out, "failed 4294967295,"),
	      "show: the largest 32-bit count fits the digit scratch");
	check(strstr(out, "UniFi on, inform http://10.0.0.1:8080/inform") != 0, "show: state and URL");
	test_adopt();
	test_bounds();
	return failures != 0;
}
