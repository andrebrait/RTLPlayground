/*
 * Prometheus text exposition of switch metrics for the RTL837x platform
 * This code is in the Public Domain
 */

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "snmp_hw.h"
#include "metrics.h"

#pragma codeseg BANK4
#pragma constseg BANK4

extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

__xdata uint8_t prometheus_enabled;

static void put_char(char c)
{
	outbuf[slen++] = c;
}

/* Decimal of an 8-byte big-endian counter, by long division of a copy. */
static void put_u64(__xdata const struct snmp_counter *__xdata v)
{
	__xdata uint8_t n[8], d[20];
	__xdata uint8_t i, k = 0, nz;
	__xdata uint16_t cur;

	for (i = 0; i < 8; i++)
		n[i] = v->bytes[i];
	do {
		cur = 0;
		nz = 0;
		for (i = 0; i < 8; i++) {
			cur = (cur << 8) | n[i];
			n[i] = cur / 10;
			cur %= 10;
			nz |= n[i];
		}
		d[k++] = '0' + cur;
	} while (nz);
	while (k)
		put_char(d[--k]);
}

/* v / 10^decimals, e.g. (-1250, 3) is "-1.250". */
static void put_fixed(__xdata int32_t v, uint8_t decimals)
{
	__xdata uint8_t d[11], k = 0;
	__xdata uint32_t u = v;

	if (v < 0) {
		put_char('-');
		u = 0 - u;
	}
	do {
		d[k++] = '0' + u % 10;
		u /= 10;
	} while (u || k <= decimals);
	while (k) {
		put_char(d[--k]);
		if (k && k == decimals)
			put_char('.');
	}
}
