/* Ports of the board as the kernel sees them: logical port N is the netdev board_ifnames[N]. */
#include <stdio.h>

#include "platform.h"
#include "linux.h"

/* One integer from /sys/class/net/<ifname of port>/<attr>; 0 for a port the board lacks or an
 * attribute that cannot be read. */
static unsigned sysfs_uint(uint8_t port, const char *attr)
{
	char path[96];
	unsigned v = 0;
	FILE *f;

	if (port >= 9 || !board_ifnames[port])
		return 0;
	snprintf(path, sizeof path, "/sys/class/net/%s/%s", board_ifnames[port], attr);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%u", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

uint16_t port_mtu_get(uint8_t port) __banked
{
	return sysfs_uint(port, "mtu");
}
