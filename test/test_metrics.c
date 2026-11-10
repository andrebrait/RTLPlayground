/* Prometheus exporter: number printers, the "prometheus" command and the
 * metric generator. metrics.c is included so its static helpers are
 * reachable; metrics_cli.c is linked unmodified. Hardware readers, SFP, NTP
 * and register access are mocked below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <regex.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "rtl837x_pins.h"
#include "sfp.h"
#include "ntp.h"
#include "snmp_hw.h"
#include "metrics.h"

__xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
__xdata uint16_t slen;

/* copied verbatim from rtlplayground.c: html_out.c's string_to_html() calls it */
uint16_t strtox(__xdata uint8_t *dst, __code const char *s)
{
	__xdata uint8_t *b = dst;
	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}

#include "../metrics.c"

/* Switch fixture: lan1 up at 1 Gbit/s named "uplink", lan2 link down,
 * lan3 PHY powered down, lan9 an SFP cage with an internally calibrated
 * module. Other ports are down. */
const struct machine machine = {
	.machine_name = "TESTBOARD",
	.log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 9},
	.is_sfp = {[8] = 1},
	.sfp_port = {{.pin_los = 5}, {.pin_los = GPIO_NA}},
};
char port_names[9][PORT_NAME_SIZE];
char hostname[24] = "sw-test";
struct uip_eth_addr uip_ethaddr = {{0x1c, 0x2a, 0xa3, 0x00, 0x00, 0x01}};
struct ntp_state ntp_state;
uint8_t sfr_data[4];
uint8_t sfp_pins_last, sfp_options[2], sfp_buf[16];
char sfp_module_vendor[2][17], sfp_module_model[2][17], sfp_module_serial[2][17];
static uint8_t a2[256];
static int fail_port = -1, fail_sel = -1, fail_a2 = -1;

bool snmp_hw_link(uint8_t port, uint32_t *v) { *v = 1u << 0; return port <= 8; }
bool snmp_hw_speed(uint8_t port, uint16_t *mbps) { *mbps = port == 0 ? 1000 : 0; return true; }
bool snmp_hw_admin(uint8_t port, uint8_t *state) { *state = port == 2 ? 2 : 1; return true; }
/* 64-bit: (port + 1) * 1000 + selector; high word: * 100; low word: * 10. */
bool snmp_hw_stat(uint8_t port, uint8_t sel, bool high, bool low, struct snmp_counter *c)
{
	uint64_t v;
	if (port == fail_port && sel == fail_sel)
		return false;
	v = high && low ? (port + 1) * 1000u + sel : (port + 1) * (high ? 100u : 10u) + sel;
	for (int i = 7; i >= 0; i--, v >>= 8)
		c->bytes[i] = v;
	return true;
}
bool sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len)
{
	assert(slot == 0 && len <= 16 && reg + len <= 256);
	if (reg == fail_a2)
		return false;
	memcpy(sfp_buf, a2 + reg, len);
	return true;
}
void reg_read_m(uint16_t reg)
{
	uint32_t v = reg == RTL837X_TM_RESULT ? 0x1700 : 0;
	sfr_data[0] = v >> 24; sfr_data[1] = v >> 16; sfr_data[2] = v >> 8; sfr_data[3] = v;
}
void read_reg_timer(uint32_t *tmr) { *tmr = 1000; }	/* uptime seconds, as ntp_uptime() reads it */

static void fixture(void)
{
	memset(port_names, 0, sizeof(port_names));
	strcpy(port_names[0], "uplink");
	ntp_state.synced = 1;
	ntp_state.utc = 1793531514;
	ntp_state.at = 100;
	sfp_pins_last = 0x30;	/* slot 0 present, LOS clear; slot 1 absent */
	sfp_options[0] = 0x60;	/* DDM implemented, internally calibrated */
	strcpy(sfp_module_vendor[0], "FS");
	strcpy(sfp_module_model[0], "SFP-10GSR-85");
	strcpy(sfp_module_serial[0], "S123");
	memcpy(a2 + 224, (uint8_t[]){0xfe, 0xc0, 0x80, 0xe8, 0x0b, 0xb8, 0x13, 0x88, 0x0f, 0xa0}, 10);
	fail_port = fail_sel = fail_a2 = -1;
}

static char scrape[65536];
static size_t scrape_len;

/* One response, outbuf by outbuf, as the TCP sender drains it. Between two
 * fills, between_fills() may change the switch. */
static const char *render(bool mib, void (*between_fills)(void))
{
	size_t hdr;
	metrics_start(mib);
	assert(slen <= TCP_OUTBUF_SIZE);
	hdr = strstr((char *)outbuf, "\r\n\r\n") + 4 - (char *)outbuf;
	assert(!strncmp((char *)outbuf, "HTTP/1.1 200 OK\r\nConnection: close\r\n"
		       "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n\r\n", hdr));
	scrape_len = 0;
	memcpy(scrape, outbuf + hdr, slen - hdr);
	scrape_len = slen - hdr;
	while (metrics_more) {
		if (between_fills)
			between_fills();
		slen = 0;
		metrics_fill();
		assert(slen <= TCP_OUTBUF_SIZE && scrape_len + slen < sizeof(scrape));
		memcpy(scrape + scrape_len, outbuf, slen);
		scrape_len += slen;
	}
	scrape[scrape_len] = 0;
	return scrape;
}

static void expect_line(const char *s, const char *line)
{
	if (!strstr(s, line)) {
		fprintf(stderr, "missing line: %s\nin scrape:\n%s", line, s);
		assert(0);
	}
}

/* Every line is a TYPE line or a sample of the family typed last, and every
 * family is typed once: families are contiguous blocks. */
static void expect_valid(const char *s)
{
	regex_t sample;
	char typed[128][96], cur[96] = "", line[512];
	int ntyped = 0;
	assert(!regcomp(&sample, "^[a-z_][a-z0-9_]*(\\{[a-z_]+=\"([^\"\\\\]|\\\\.)*\"(,[a-z_]+=\"([^\"\\\\]|\\\\.)*\")*\\})? -?[0-9.]+$",
			REG_EXTENDED | REG_NOSUB));
	for (const char *p = s; *p; p = strchr(p, '\n') + 1) {
		size_t n = strcspn(p, "\n");
		assert(p[n] == '\n' && n < sizeof(line));
		memcpy(line, p, n);
		line[n] = 0;
		if (!strncmp(line, "# TYPE ", 7)) {
			sscanf(line + 7, "%95s", cur);
			for (int i = 0; i < ntyped; i++)
				assert(strcmp(typed[i], cur));
			strcpy(typed[ntyped++], cur);
			continue;
		}
		if (regexec(&sample, line, 0, NULL, 0)) {
			fprintf(stderr, "bad line: %s\n", line);
			assert(0);
		}
		assert(!strncmp(line, cur, strlen(cur)) && strchr("{ ", line[strlen(cur)]));
	}
	regfree(&sample);
}

static void test_golden(void)
{
	const char *s;
	FILE *f;
	char *want;
	long n;
	fixture();
	s = render(false, NULL);
	expect_valid(s);
	/* VERSION_SW/BUILD_DATE are those metrics.c saw: a firmware build leaves a
	 * generated version.h beside it that wins over stub/version.h. */
	expect_line(s, "node_uname_info{sysname=\"RTLPlayground\",release=\"" VERSION_SW "\",version=\"" BUILD_DATE "\","
		       "machine=\"TESTBOARD\",nodename=\"sw-test\",domainname=\"(none)\"} 1\n");
	/* The golden file starts after node_uname_info, which carries the build. */
	s = strstr(s, "# TYPE node_boot_time_seconds");
	expect_line(s, "node_boot_time_seconds 1793531414\n");
	expect_line(s, "node_time_seconds 1793532414\n");
	expect_line(s, "node_hwmon_temp_celsius{chip=\"thermal_thermal_zone0\",sensor=\"temp1\"} 46.000\n");
	expect_line(s, "node_thermal_zone_temp{zone=\"0\",type=\"cpu-thermal\",policy=\"step_wise\",mode=\"enabled\"} 46.000\n");
	expect_line(s, "node_hwmon_chip_names{chip=\"sfp_lan9\",chip_name=\"sfp_lan9\"} 1\n");
	expect_line(s, "node_hwmon_temp_celsius{chip=\"sfp_lan9\",sensor=\"temp1\"} -1.250\n");
	expect_line(s, "node_network_info{device=\"lan1\",address=\"1c:2a:a3:00:00:01\",broadcast=\"ff:ff:ff:ff:ff:ff\","
		       "operstate=\"up\",ifalias=\"uplink\"} 1\n");
	expect_line(s, "{device=\"lan2\",address=\"1c:2a:a3:00:00:01\",broadcast=\"ff:ff:ff:ff:ff:ff\",operstate=\"lowerlayerdown\",ifalias=\"\"} 1\n");
	expect_line(s, "{device=\"lan3\",address=\"1c:2a:a3:00:00:01\",broadcast=\"ff:ff:ff:ff:ff:ff\",operstate=\"down\",ifalias=\"\"} 1\n");
	expect_line(s, "node_network_carrier{device=\"lan1\"} 1\n");
	expect_line(s, "node_network_carrier{device=\"lan2\"} 0\n");
	expect_line(s, "node_network_speed_bytes{device=\"lan1\"} 125000000\n");
	assert(!strstr(s, "node_network_speed_bytes{device=\"lan2\"}"));
	/* lan2 is logical port 1: 64-bit (1+1)*1000+sel, low word (1+1)*10+sel */
	expect_line(s, "node_network_receive_bytes_total{device=\"lan2\"} 2000\n");
	expect_line(s, "node_network_receive_packets_total{device=\"lan2\"} 6009\n");
	expect_line(s, "node_network_receive_errs_total{device=\"lan2\"} 35\n");
	expect_line(s, "node_network_receive_frame_total{device=\"lan2\"} 35\n");
	expect_line(s, "node_network_receive_drop_total{device=\"lan2\"} 33\n");
	expect_line(s, "node_network_transmit_drop_total{device=\"lan2\"} 208\n");
	expect_line(s, "node_network_transmit_errs_total{device=\"lan2\"} 241\n");
	expect_line(s, "node_network_transmit_colls_total{device=\"lan2\"} 220\n");
	expect_line(s, "rtlplayground_sfp_info{device=\"lan9\",vendor=\"FS\",model=\"SFP-10GSR-85\",serial=\"S123\"} 1\n");
	expect_line(s, "rtlplayground_sfp_los{device=\"lan9\"} 0\n");
	expect_line(s, "rtlplayground_sfp_voltage_volts{device=\"lan9\"} 3.3000\n");
	expect_line(s, "rtlplayground_sfp_tx_bias_amperes{device=\"lan9\"} 0.006000\n");
	expect_line(s, "rtlplayground_sfp_tx_power_watts{device=\"lan9\"} 0.0005000\n");
	expect_line(s, "rtlplayground_sfp_rx_power_watts{device=\"lan9\"} 0.0004000\n");
	expect_line(s, "rtlplayground_sfp_external_calibration{device=\"lan9\"} 0\n");
	assert(!strstr(scrape, "node_ethtool_"));
	f = fopen("metrics_golden.txt", "rb");
	if (getenv("UPDATE_GOLDEN") || !f) {
		assert(getenv("UPDATE_GOLDEN"));
		f = fopen("metrics_golden.txt", "wb");
		fputs(s, f);
		fclose(f);
	} else {
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		rewind(f);
		want = calloc(n + 1, 1);
		assert(fread(want, 1, n, f) == (size_t)n);
		fclose(f);
		assert(!strcmp(s, want));
		free(want);
	}
	puts("PASS golden scrape: OpenWrt names and labels, values, units, valid contiguous families");
}

static void test_no_ntp(void)
{
	fixture();
	ntp_state.synced = 0;
	const char *s = render(false, NULL);
	assert(!strstr(s, "node_boot_time_seconds ") && !strstr(s, "node_time_seconds "));
	puts("PASS no wall-clock time families without NTP sync");
}

static void test_read_failure(void)
{
	fixture();
	fail_port = 1;
	fail_sel = 0;
	const char *s = render(false, NULL);
	expect_valid(s);
	assert(!strstr(s, "node_network_receive_bytes_total{device=\"lan2\"}"));
	expect_line(s, "node_network_receive_bytes_total{device=\"lan3\"} 3000\n");
	puts("PASS a failed hardware read omits only that sample");
}

static void test_escaping(void)
{
	fixture();
	for (int i = 0; i < PORT_NAME_SIZE; i++)
		port_names[1][i] = i & 1 ? '\\' : '"';
	const char *s = render(false, NULL);
	expect_valid(s);
	expect_line(s, "ifalias=\"\\\"\\\\\\\"\\\\");
	puts("PASS a 32-character port name of quotes and backslashes is escaped and fits");
}

static void test_mib(void)
{
	fixture();
	const char *s = render(true, NULL);
	expect_valid(s);
	expect_line(s, "# TYPE node_ethtool_transmitted_crcalignerrors untyped\n");
	expect_line(s, "node_ethtool_received_ether_crc_align_errors{device=\"lan1\"} 25\n");
	expect_line(s, "node_ethtool_received_packets_total{device=\"lan1\"} 3009\n");
	puts("PASS ?mib=1 adds the ethtool families");
}

static void pull_module(void)
{
	sfp_pins_last |= 0x01;
}

static void test_sfp_external_calibration(void)
{
	fixture();
	sfp_options[0] = 0x70;	/* DDM implemented, internally and externally calibrated flags */
	const char *s = render(false, NULL);
	expect_valid(s);
	expect_line(s, "rtlplayground_sfp_external_calibration{device=\"lan9\"} 1\n");
	expect_line(s, "rtlplayground_sfp_info{device=\"lan9\"");
	assert(!strstr(s, "sfp_lan9") && !strstr(s, "rtlplayground_sfp_voltage_volts{"));
	puts("PASS an externally calibrated module reports the flag and no diagnostic values");
}

static void test_sfp_read_failure(void)
{
	fixture();
	fail_a2 = 224 + 2;	/* the voltage word */
	const char *s = render(false, NULL);
	expect_valid(s);
	assert(!strstr(s, "rtlplayground_sfp_voltage_volts{"));
	expect_line(s, "rtlplayground_sfp_tx_bias_amperes{device=\"lan9\"} 0.006000\n");
	fail_a2 = 224;	/* the temperature word */
	s = render(false, NULL);
	expect_valid(s);
	assert(!strstr(s, "node_hwmon_temp_celsius{chip=\"sfp_lan9\""));
	puts("PASS a failed SFP diagnostic read omits only that sample");
}

static void test_sfp_removed(void)
{
	fixture();
	const char *s = render(false, pull_module);
	expect_valid(s);
	assert(!strstr(s, "rtlplayground_sfp_info{device=\"lan9\""));
	puts("PASS a module pulled mid-scrape drops its later samples, output stays valid");
}

/* CLI edges metrics_cli.c uses */
uint8_t cmd_buffer[CMD_BUF_SIZE];
uint8_t cmd_words_len, cmd_words_b[15], err_status;
static char console[256];
void print_string(const char *s) { strncat(console, s, sizeof(console) - strlen(console) - 1); }
uint8_t cmd_compare(uint8_t start, const uint8_t *word)
{
	const char *w = (const char *)cmd_buffer + cmd_words_b[start];
	size_t n = strlen((const char *)word);
	return !strncmp(w, (const char *)word, n) && (w[n] == ' ' || !w[n]);
}

static void run_cmd(const char *s)
{
	strcpy((char *)cmd_buffer, s);
	cmd_words_len = 0;
	for (unsigned i = 0; cmd_buffer[i]; i++)
		if (cmd_buffer[i] != ' ' && (i == 0 || cmd_buffer[i - 1] == ' '))
			cmd_words_b[cmd_words_len++] = i;
	console[0] = 0;
	err_status = ERR_OK;
	prometheus_command();
}

static void expect_out(const char *want)
{
	outbuf[slen] = 0;
	if (strcmp((char *)outbuf, want)) {
		fprintf(stderr, "got \"%s\", want \"%s\"\n", outbuf, want);
		assert(0);
	}
}

static void expect_u64(const uint8_t b[8], const char *want)
{
	struct snmp_counter c;
	memcpy(c.bytes, b, 8);
	slen = 0;
	put_u64(&c);
	expect_out(want);
}

static void expect_fixed(int32_t v, uint8_t decimals, const char *want)
{
	slen = 0;
	put_fixed(v, decimals);
	expect_out(want);
}

static void test_put_u64(void)
{
	expect_u64((uint8_t[8]){0}, "0");
	expect_u64((uint8_t[8]){0, 0, 0, 1, 0, 0, 0, 0}, "4294967296");
	expect_u64((uint8_t[8]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, "18446744073709551615");
	puts("PASS put_u64: 0, 2^32 and 2^64-1");
}

static void test_put_fixed(void)
{
	expect_fixed(-1250, 3, "-1.250");
	expect_fixed(5, 4, "0.0005");
	expect_fixed(33000, 4, "3.3000");
	expect_fixed(46, 0, "46");
	expect_fixed(INT32_MIN, 0, "-2147483648");
	puts("PASS put_fixed: sign, leading zeros, no decimals, INT32_MIN");
}

static void test_command(void)
{
	run_cmd("prometheus on");
	assert(prometheus_enabled == 1 && err_status == ERR_OK);
	run_cmd("prometheus show");
	assert(strstr(console, "Prometheus exporter: enabled"));
	run_cmd("prometheus off");
	assert(prometheus_enabled == 0 && err_status == ERR_OK);
	run_cmd("prometheus show");
	assert(strstr(console, "Prometheus exporter: disabled"));
	run_cmd("prometheus");
	assert(strstr(console, "Usage: prometheus [on|off|show]") && err_status == ERR_INVALID_ARGUMENT);
	run_cmd("prometheus on now");
	assert(prometheus_enabled == 0 && err_status == ERR_INVALID_ARGUMENT);
	puts("PASS prometheus on|off|show and usage errors");
}

int main(void)
{
	test_put_u64();
	test_put_fixed();
	test_command();
	test_golden();
	test_no_ntp();
	test_read_failure();
	test_escaping();
	test_mib();
	test_sfp_removed();
	test_sfp_external_calibration();
	test_sfp_read_failure();
	return 0;
}
