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

static char out[512];
static size_t out_len;
void write_char(char c) { if (out_len < sizeof out - 1) out[out_len++] = c; out[out_len] = 0; }
void print_string(__code const char *p) { while (*p) write_char(*p++); }
void print_string_x(__xdata char *p) { while (*p) write_char(*p++); }
void itoa_short(uint16_t v) { char s[8]; snprintf(s, sizeof s, "%u", v); print_string(s); }
void print_long(uint32_t v) { char s[12]; snprintf(s, sizeof s, "%u", v); print_string(s); }
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

/* Runs an inform up to the reply: returns the request as sent */
static uint8_t req[2048];
static uint16_t req_len;

static void inform(void)
{
	if (uptime_secs < unifi.next)
		uptime_secs = unifi.next;
	ticks();
	req_len = 0;
	appcall(UIP_CONNECTED, 0, 0);
	memcpy(req + req_len, sent, sent_len);
	req_len += sent_len;
	while (unifi.phase == U_SEND) {
		appcall(UIP_ACKDATA, 0, 0);
		memcpy(req + req_len, sent, sent_len);
		req_len += sent_len;
	}
}

static const uint8_t key[16] = {
	0xba, 0x86, 0xf2, 0xbb, 0xe1, 0x07, 0xc7, 0xc5,
	0x7e, 0xb5, 0xf2, 0x69, 0x07, 0x75, 0xc7, 0x12
};

/* An encrypted TNBU reply around json, after an HTTP header */
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
	memcpy(k, key, 16);
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
	};
	int same = 1;
	for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
		cmd(bad[i]);
		same &= !strcmp(unifi.host, "10.0.0.1") && unifi.port == 8080 && strstr(out, "Error") != 0;
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
	uint8_t *p = (uint8_t *)body + 4;
	uint16_t plen = (uint16_t)(p[38] << 8 | p[39]);
	check(!memcmp(p, "TNBU", 4) && !memcmp(p + 8, uip_ethaddr.addr, 6) && p[15] == 0x09 && p[35] == 1
	      && plen == clen - 40,
	      "request: TNBU header, GCM flags, payload length with the tag");

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
	appcall(UIP_REXMIT, 0, 0);
	check(sent_len == n && !memcmp(sent, first, n), "send: a retransmission resends the same bytes");
	unifi.enabled = 0;
	u_stop();
	appcall(UIP_POLL, 0, 0);
	check(uip_flags == UIP_ABORT && unifi.phase == U_IDLE && !unifi.conn, "unifi off: the connection is reset");
	unifi.enabled = 1;
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

	inform();
	n = reply(r, noop, 1, 1);
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TAG && !unifi.type[0] && unifi.phase == U_IDLE, "reply: a bad tag is refused");

	inform();
	n = sprintf((char *)r, "HTTP/1.1 200 OK\r\nContent-Length: 99999\r\n\r\n");
	deliver(r, n);
	check(unifi.err == UE_HTTP && uip_flags == UIP_ABORT && !unifi.conn, "reply: a reply larger than the buffer is reset");

	inform();
	n = reply(r, noop, 1, 0);
	(strstr((char *)r, "\r\n\r\n") + 4)[39] ^= 1;	/* the TNBU length field */
	deliver(r, n);
	ticks();
	check(unifi.err == UE_TNBU, "reply: a TNBU length that disagrees with Content-Length is refused");

	cmd("unifi selftest");
	check(selftests == 1 && !unifi.keyed, "selftest: runs while idle and forces a new key setup");
}

int main(void)
{
	unifi_init();
	test_url();
	test_request();
	test_rexmit();
	test_replies();
	cmd("unifi");
	check(strstr(out, "UniFi on, inform http://10.0.0.1:8080/inform") != 0, "show: state and URL");
	return failures != 0;
}
