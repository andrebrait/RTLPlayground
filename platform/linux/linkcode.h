#ifndef _LINKCODE_H_
#define _LINKCODE_H_

#include <stdbool.h>
#include <stdint.h>

/* The names of the codes, as LINKS of html/app.js:735 has them. */
static const char *const link_names[8] = { "Down", "10M", "100M", "1000M", "500M", "10G", "2.5G", "5G" };

/* What port_link_get() returns, from the carrier and the ethtool speed in Mb/s (0xffffffff =
 * SPEED_UNKNOWN): 0 = down, else the index into LINKS of html/app.js:735
 * ["Down","10M","100M","1000M","500M","10G","2.5G","5G"]. A speed the UI has no name for (an
 * unreadable one, or 14G and up) shows as 1000M: the port is up, and that is the safest label. */
static inline uint8_t link_code(bool carrier, uint32_t speed_mbps)
{
	if (!carrier)
		return 0;
	switch (speed_mbps) {
	case 10:	return 1;
	case 100:	return 2;
	case 1000:	return 3;
	case 2500:	return 6;
	case 5000:	return 7;
	case 10000:	return 5;
	default:	return 3;
	}
}

#endif
