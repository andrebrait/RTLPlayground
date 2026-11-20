/* ntp.c's calendar and offset printing against the C library: every day of
 * the 32-bit Unix range through ntp_split(), and every UTC offset through
 * ntp_print_offset(). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../ntp.c"

/* What ntp.c needs from the rest of the firmware */
__xdata struct dns_state dns_state;
void *uip_appdata;
__xdata uint8_t uip_flags;
__xdata uint16_t uip_len;
void dns_lookup(void) {}
void print_ip(__xdata uint8_t *ip) { (void)ip; }
void read_reg_timer(__xdata uint32_t *t) { *t = 0; }
void timer_set(__xdata struct timer *t, clock_time_t interval) { (void)t; (void)interval; }
int timer_expired(__xdata struct timer *t) { (void)t; return 0; }
void uip_send(const void *data, uint16_t len) { (void)data; (void)len; }
struct uip_udp_conn *uip_udp_new(uip_ipaddr_t *ripaddr, u16_t rport) { (void)ripaddr; (void)rport; return 0; }

static char out[32];
static size_t out_len;
void write_char(char c) { if (out_len < sizeof out - 1) out[out_len++] = c; out[out_len] = 0; }
void print_string(__code const char *p) { while (*p) write_char(*p++); }
void print_string_x(__xdata char *p) { while (*p) write_char(*p++); }
void itoa_short(uint16_t v) { char s[6]; snprintf(s, sizeof s, "%u", v); print_string(s); }

static int failures;

int main(void)
{
	for (uint32_t day = 0; day <= 0xffffffffu / 86400; day++) {
		time_t t = (time_t)day * 86400;
		struct tm *tm = gmtime(&t);

		ntp_split((uint32_t)t);
		if ((ntp_year != tm->tm_year + 1900 || ntp_mon != tm->tm_mon + 1 ||
		     ntp_mday != tm->tm_mday || ntp_wday != tm->tm_wday) && failures++ < 10)
			printf("FAIL ntp_split day %u: %u-%u-%u wday %u\n", day, ntp_year, ntp_mon, ntp_mday, ntp_wday);
	}
	puts("PASS ntp_split: every day from 1970 to 2106, leap years and weekday");

	for (int off = -840; off <= 840; off++) {
		char want[16];

		snprintf(want, sizeof want, "%c%02d:%02d", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
		out_len = 0;
		ntp_print_offset(off);
		if (strcmp(out, want) && failures++ < 10)
			printf("FAIL ntp_print_offset %d: %s, want %s\n", off, out, want);
	}
	puts("PASS ntp_print_offset: every offset from -14:00 to +14:00");

	return failures != 0;
}
