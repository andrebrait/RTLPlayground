/* Prometheus exporter: number printers and the "prometheus" command.
 * metrics.c is included so its static printers are reachable; metrics_cli.c
 * is linked unmodified. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "rtl837x_common.h"
#include "snmp_hw.h"
#include "metrics.h"

__xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
__xdata uint16_t slen;

#include "../metrics.c"

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
	return 0;
}
