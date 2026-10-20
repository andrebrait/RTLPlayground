/* link_code(): carrier and ethtool speed -> the link code app.js reads (LINKS, app.js:735):
 * 0 Down, 1 10M, 2 100M, 3 1000M, 4 500M, 5 10G, 6 2.5G, 7 5G; a speed it has no name for is 3. */
#include <stdio.h>

#include <linux/ethtool.h>

#include "linkcode.h"

static int fails;

static void check(const char *what, uint8_t want, uint8_t got)
{
	if (want != got) {
		printf("FAIL: %s: expected %u, got %u\n", what, want, got);
		fails++;
	} else {
		printf("ok:   %s\n", what);
	}
}

#define SPEED(name, code) do { \
	check(#name " with carrier", code, link_code(true, name)); \
	check(#name " without carrier", 0, link_code(false, name)); \
} while (0)

int main(void)
{
	SPEED(SPEED_10, 1);
	SPEED(SPEED_100, 2);
	SPEED(SPEED_1000, 3);
	SPEED(SPEED_2500, 6);
	SPEED(SPEED_5000, 7);
	SPEED(SPEED_10000, 5);
	/* the rest of the speeds ethtool names: up, but not one the UI has a label for */
	SPEED(SPEED_14000, 3);
	SPEED(SPEED_20000, 3);
	SPEED(SPEED_25000, 3);
	SPEED(SPEED_40000, 3);
	SPEED(SPEED_50000, 3);
	SPEED(SPEED_56000, 3);
	SPEED(SPEED_100000, 3);
	SPEED(SPEED_200000, 3);
	SPEED(SPEED_400000, 3);
	SPEED(SPEED_800000, 3);
	/* the driver reports no speed (SPEED_UNKNOWN is -1; the field is unsigned) */
	check("SPEED_UNKNOWN with carrier", 3, link_code(true, (uint32_t)SPEED_UNKNOWN));
	check("SPEED_UNKNOWN without carrier", 0, link_code(false, (uint32_t)SPEED_UNKNOWN));
	check("speed 0 with carrier", 3, link_code(true, 0));
	return fails != 0;
}
