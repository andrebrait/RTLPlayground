/* div_small.c against C's own division: every 16-bit value with every
 * divisor from 2 to 16, the decimal digits of every 16-bit value, and
 * 64-bit long division over a spread of values. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"

static int failures;

static void check(int ok, const char *what, unsigned long long v, unsigned d)
{
	if (!ok && failures++ < 10)
		printf("FAIL %s: %llu / %u\n", what, v, d);
}

int main(void)
{
	uint32_t v;
	uint8_t d;

	for (d = 2; d <= 16; d++)
		for (v = 0; v <= 0xffff; v++) {
			uint16_t q = div16_small(v, d);
			check(q == v / d && div_rem == v % d, "div16_small", v, d);
		}
	puts("PASS div16_small: every 16-bit value, divisors 2 to 16");

	for (v = 0; v <= 0xffff; v++) {
		char want[6];
		uint8_t i = u16_to_dec(v);

		snprintf(want, sizeof want, "%u", (unsigned)v);
		check(sizeof(dec_digits) - i == strlen(want)
		      && !memcmp(dec_digits + i, want, strlen(want)), "u16_to_dec", v, 10);
	}
	puts("PASS u16_to_dec: every 16-bit value");

	/* 64-bit values: powers of two, their neighbours and an LCG spread */
	uint64_t x = 1, s = 12345;
	for (int n = 0; n < 20000; n++) {
		uint64_t val = n < 64 ? x << n : n < 128 ? (x << (n - 64)) - 1 : (s = s * 6364136223846793005ull + 1442695040888963407ull);
		for (d = 2; d <= 16; d++) {
			uint8_t b[8];
			uint64_t q = 0;

			for (int i = 0; i < 8; i++)
				b[i] = val >> (56 - 8 * i);
			uint8_t r = div_small(b, 8, d);
			for (int i = 0; i < 8; i++)
				q = q << 8 | b[i];
			check(q == val / d && r == val % d, "div_small", val, d);
		}
	}
	puts("PASS div_small: 64-bit values, divisors 2 to 16");

	return failures != 0;
}
