/*
 * Division of unsigned values wider than a byte by a small divisor.
 *
 * SDCC compiles a 16- or 32-bit division into a call to its library routines,
 * which shift and subtract one bit at a time. Dividing nibble by nibble keeps
 * every partial dividend at most (d - 1) * 16 + 15, which is at most 255 for
 * d <= 16, so each step is the 8051's DIV AB. A larger divisor whose factors
 * are all at most 16 divides in steps, because floor(floor(x / a) / b) equals
 * floor(x / (a * b)).
 *
 * Not banked: callers sit in every bank.
 */

#include <stdint.h>
#include "rtl837x_common.h"

__xdata uint8_t div_rem;
__xdata char dec_digits[5];

uint8_t div_small(__xdata uint8_t *n, uint8_t len, uint8_t d)
{
	uint8_t cur = 0, q, i;

	for (i = 0; i < len; i++) {
		cur = (cur << 4) | (n[i] >> 4);
		q = cur / d;
		cur %= d;
		cur = (cur << 4) | (n[i] & 15);
		n[i] = (q << 4) | (cur / d);
		cur %= d;
	}
	return cur;
}

uint16_t div16_small(uint16_t v, uint8_t d)
{
	__xdata uint8_t b[2];

	b[0] = v >> 8;
	b[1] = v;
	div_rem = div_small(b, 2, d);
	return ((uint16_t)b[0] << 8) | b[1];
}

uint8_t u16_to_dec(uint16_t v)
{
	uint8_t i = sizeof(dec_digits);

	do {
		v = div16_small(v, 10);
		dec_digits[--i] = '0' + div_rem;
	} while (v);
	return i;
}
