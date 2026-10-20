/*
 * Ports of the board as the kernel sees them: logical port N is the netdev board_ifnames[N].
 * Link state and settings come from sysfs and ethtool (SIOCETHTOOL, ETHTOOL_G/SLINKSETTINGS), the
 * MIB counters from the driver's named ethtool statistics and the netdev statistics (RTM_GETLINK,
 * IFLA_STATS64; see mibmap.h). Every kernel call is checked: a failed change prints an error
 * through cmd_error() and leaves the previous state; a failed read reads as zero or down.
 */
#include <errno.h>
#include <glob.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nl.h"
/* the 8051 storage-class shim defines __data, which linux/socket.h uses as a member name */
#pragma push_macro("__data")
#undef __data
#include <linux/ethtool.h>
#include <linux/sockios.h>
#pragma pop_macro("__data")

#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"
#include "linkcode.h"
#include "mibmap.h"
#include "portlogic.h"

static const char *ifname_of(uint8_t port)
{
	return port < BOARD_PORTS ? board_ifnames[port] : NULL;
}

/* One integer from /sys/class/net/<ifname of port>/<attr>; 0 for a port the board lacks or an
 * attribute that cannot be read (carrier of a downed netdev is one: EINVAL). */
static unsigned sysfs_uint(uint8_t port, const char *attr)
{
	char path[96];
	unsigned v = 0;
	FILE *f;

	if (!ifname_of(port))
		return 0;
	snprintf(path, sizeof path, "/sys/class/net/%s/%s", ifname_of(port), attr);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%u", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

/* "Error: <ifname>: <what>: <errno text>" (what = NULL: errno text alone). */
static void port_error(uint8_t port, const char *what)
{
	static char msg[96];

	snprintf(msg, sizeof msg, "%s: %s%s%s\n", ifname_of(port) ? ifname_of(port) : "port", what ? what : "",
		 what ? ": " : "", strerror(errno));
	cmd_error(msg);
}

/* A socket for the interface ioctls, opened on first use. */
static int ctl_fd(void)
{
	static int fd = -1;

	if (fd < 0)
		fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	return fd;
}

/* The request block for the port's netdev; the name stays empty for a port the board lacks, so
 * the kernel answers ENODEV. */
static struct ifreq ifr_for(uint8_t port)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof ifr);
	if (ifname_of(port))
		snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname_of(port));
	return ifr;
}

/* ---------------------------------------------------------------- MTU
 *
 * The 8051 value is what `mtu <port> <V>` writes to RTL8373_REG_MAC_L2_PORT_MAX_LEN (14 bits,
 * 64..16383): the longest frame the MAC accepts, counted on the wire. A Linux netdev MTU is the
 * L2 payload only, so MTU = V - 26 and V = MTU + 26. 26 = ETH_HLEN + 2 * VLAN_HLEN + ETH_FCS_LEN
 * (Ethernet header, up to two stacked VLAN tags, FCS), the very conversion of the target kernel's
 * Realtek Otto DSA driver: openwrt main, target/linux/realtek/files-6.18/drivers/net/dsa/rtl83xx/
 * dsa.c, RTLDSA_FRAME_OVERHEAD and rtldsa_port_change_mtu() (frame_size = new_mtu +
 * RTLDSA_FRAME_OVERHEAD, written to the MAC max-length register of the port). `mtu show` and
 * mtu.json report exactly what `mtu` set, clamped to the register's 14 bits like the 8051's
 * port_mtu_get() (a netdev MTU of 65535 reads 16383). A V whose MTU lies outside what the netdev
 * accepts (min_mtu/max_mtu of sysfs) is an error and changes nothing. A bridge over the ports
 * follows by itself: the kernel sets the MTU of a bridge whose MTU nobody set to the minimum of its
 * ports on every port MTU change (br_mtu_auto_adjust); this daemon never sets a bridge MTU.
 */
#define MTU_FRAME_OVERHEAD 26
#define MTU_REG_MAX 0x3fff

uint16_t port_mtu_get(uint8_t port) __banked
{
	struct ifreq ifr = ifr_for(port);
	unsigned v;

	if (ioctl(ctl_fd(), SIOCGIFMTU, &ifr) < 0)
		return 0;
	v = ifr.ifr_mtu + MTU_FRAME_OVERHEAD;
	return v > MTU_REG_MAX ? MTU_REG_MAX : v;
}

void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked
{
	struct ifreq ifr = ifr_for(port);
	unsigned min, max;

	if (!ifname_of(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	min = sysfs_uint(port, "min_mtu");
	max = sysfs_uint(port, "max_mtu");
	if (mtu < MTU_FRAME_OVERHEAD + (min ? min : 68) || (max && mtu > MTU_FRAME_OVERHEAD + max)) {
		static char msg[96];

		snprintf(msg, sizeof msg, "%s takes frame lengths of %u..%u\n", ifname_of(port),
			 MTU_FRAME_OVERHEAD + (min ? min : 68), max && MTU_FRAME_OVERHEAD + max < MTU_REG_MAX ? MTU_FRAME_OVERHEAD + max : MTU_REG_MAX);
		cmd_error(msg);
		return;
	}
	ifr.ifr_mtu = mtu - MTU_FRAME_OVERHEAD;
	if (ioctl(ctl_fd(), SIOCSIFMTU, &ifr) < 0)
		port_error(port, "cannot set the MTU");
}

/* ---------------------------------------------------------------- link settings */

static int ethtool_cmd(uint8_t port, void *data)
{
	struct ifreq ifr = ifr_for(port);

	ifr.ifr_data = data;
	return ioctl(ctl_fd(), SIOCETHTOOL, &ifr);
}

/* The link modes handshake of ETHTOOL_GLINKSETTINGS: the first call answers with the number of
 * mask words (negated), the second fills them. */
static int elink_get(uint8_t port, struct elink *e)
{
	memset(e, 0, sizeof *e);
	e->base.cmd = ETHTOOL_GLINKSETTINGS;
	if (ethtool_cmd(port, e) < 0)
		return -1;
	if (e->base.cmd != ETHTOOL_GLINKSETTINGS || e->base.link_mode_masks_nwords >= 0 ||
	    -e->base.link_mode_masks_nwords > MASK_WORDS_MAX) {
		errno = EOPNOTSUPP;
		return -1;
	}
	e->base.link_mode_masks_nwords = -e->base.link_mode_masks_nwords;
	if (ethtool_cmd(port, e) < 0)
		return -1;
	if (e->base.link_mode_masks_nwords <= 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	return 0;
}

static int elink_set(uint8_t port, struct elink *e)
{
	e->base.cmd = ETHTOOL_SLINKSETTINGS;
	return ethtool_cmd(port, e);
}

uint8_t port_link_get(uint8_t port) __banked
{
	struct elink e;
	uint32_t speed = SPEED_UNKNOWN;

	if (!sysfs_uint(port, "carrier"))
		return 0;
	if (elink_get(port, &e) == 0)
		speed = e.base.speed;
	return link_code(true, speed);
}

bool port_phy_enabled(uint8_t port) __banked
{
	struct ifreq ifr = ifr_for(port);

	return ioctl(ctl_fd(), SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP);
}

uint8_t port_phy_adv_get(uint8_t port) __banked
{
	struct elink e;

	if (elink_get(port, &e) < 0)
		return 0;
	return adv_bits(&e);
}

/* Brings the port up (on) or down; false with the error printed. */
static bool port_set_up(uint8_t port, bool on)
{
	struct ifreq ifr = ifr_for(port);

	if (ioctl(ctl_fd(), SIOCGIFFLAGS, &ifr) < 0) {
		port_error(port, "cannot read the port state");
		return false;
	}
	if (!!(ifr.ifr_flags & IFF_UP) == on)
		return true;
	ifr.ifr_flags = on ? ifr.ifr_flags | IFF_UP : ifr.ifr_flags & ~IFF_UP;
	if (ioctl(ctl_fd(), SIOCSIFFLAGS, &ifr) < 0) {
		port_error(port, on ? "cannot enable the port" : "cannot disable the port");
		return false;
	}
	return true;
}

static const char *speed_name(uint8_t speed)
{
	switch (speed) {
	case PHY_SPEED_10M:  return "10M";
	case PHY_SPEED_100M: return "100M";
	case PHY_SPEED_1G:   return "1G";
	case PHY_SPEED_2G5:  return "2.5G";
	case PHY_SPEED_5G:   return "5G";
	case PHY_SPEED_10G:  return "10G";
	default:             return "auto";
	}
}

/*
 * `port <n> 10m|100m|1g|2g5|5g|10g|auto|on|off [half|full]`: like the 8051 (which leaves
 * autonegotiation on and advertises only the chosen mode) the port advertises only the modes of
 * that speed (speed_masks); auto advertises everything the port supports; off takes the port
 * down. Nothing changes when the driver refuses (veth does).
 * shortcut: auto/on on a netdev that supports none of the speed modes (veth) only brings it up
 * and says so: there is nothing to advertise, and refusing would leave `port N on` unable to
 * undo `port N off`.
 */
void phy_set_speed(void) __banked
{
	uint8_t port = phy_settings.port;
	struct elink e;
	bool any = false;

	if (!ifname_of(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	if (phy_settings.speed == PHY_OFF) {
		if (port_set_up(port, false)) {
			print_string("Setting port "); print_phys_port(port); print_string(" to disabled\n");
		}
		return;
	}
	if (elink_get(port, &e) < 0) {
		if (phy_settings.speed != PHY_SPEED_AUTO) {
			port_error(port, "cannot read the link settings");
			return;
		}
		e.base.link_mode_masks_nwords = 0;	/* no modes to select */
	}
	if (!speed_masks(&e, phy_settings.speed, phy_settings.duplex, &any)) {
		cmd_error("the port does not have that speed and duplex\n");
		return;
	}
	if (any) {
		e.base.autoneg = AUTONEG_ENABLE;
		if (elink_set(port, &e) < 0) {
			port_error(port, "cannot set the speed");
			return;
		}
	}
	if (!port_set_up(port, true))
		return;
	if (!any) {
		print_string("Port "); print_phys_port(port);
		print_string(" is on; it has no selectable link modes and advertises its native modes\n");
		return;
	}
	print_string("Setting port "); print_phys_port(port); print_string(" to speed ");
	print_string(speed_name(phy_settings.speed));
	if (phy_settings.speed == PHY_SPEED_10M || phy_settings.speed == PHY_SPEED_100M) {
		if (phy_settings.duplex == PHY_DUPLEX_FULL)
			print_string(" full duplex");
		else if (phy_settings.duplex == PHY_DUPLEX_HALF)
			print_string(" half duplex");
	}
	write_char('\n');
}

/* `port <n> duplex full|half`: see duplex_masks (the 8051's behaviour). */
void phy_set_duplex(void) __banked
{
	uint8_t port = phy_settings.port;
	bool full = phy_settings.duplex == PHY_DUPLEX_FULL;
	struct elink e;

	if (!ifname_of(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	if (elink_get(port, &e) < 0) {
		port_error(port, "cannot read the link settings");
		return;
	}
	if (!duplex_masks(&e, full)) {
		cmd_error("no 10M or 100M mode is advertised\n");
		return;
	}
	if (elink_set(port, &e) < 0) {
		port_error(port, "cannot set the duplex");
		return;
	}
	print_string("Setting port "); print_phys_port(port);
	print_string(full ? " to full duplex\n" : " to half duplex\n");
}

static void print_modes(const struct elink *e, int which)
{
	for (unsigned i = 0; i < NMODES; i++)
		if (mode_in(e, which, modes[i].bit)) {
			write_char(' ');
			print_string(modes[i].name);
		}
}

void phy_show(uint8_t port) __banked
{
	struct elink e;
	uint8_t code = port_link_get(port);

	print_string("\nLink speed: ");
	print_string(link_names[code]);
	if (elink_get(port, &e) < 0) {
		write_char('\n');
		return;
	}
	if (code && e.base.duplex != DUPLEX_UNKNOWN)
		print_string(e.base.duplex == DUPLEX_FULL ? " full duplex" : " half duplex");
	if (e.base.autoneg == AUTONEG_ENABLE) {
		print_string("\nAN enabled, advertising:");
		print_modes(&e, MASK_ADVERTISING);
		print_string("\nLink Partner advertises:");
		print_modes(&e, MASK_PARTNER);
	} else {
		print_string("\nAN disabled, forced");
	}
	write_char('\n');
}

void port_media_show(uint8_t port) __banked
{
	struct elink e;
	struct ethtool_pauseparam pause = { .cmd = ETHTOOL_GPAUSEPARAM };

	if (sysfs_uint(port, "carrier") && elink_get(port, &e) == 0 &&
	    (e.base.master_slave_state == MASTER_SLAVE_STATE_MASTER || e.base.master_slave_state == MASTER_SLAVE_STATE_SLAVE)) {
		print_string("Link role: ");
		print_string(e.base.master_slave_state == MASTER_SLAVE_STATE_MASTER ? "master\n" : "slave\n");
	}
	print_string("Pause negotiated:");
	if (ethtool_cmd(port, &pause) < 0) {
		print_string(" unknown\n");	/* the driver has no pause parameters (veth) */
		return;
	}
	if (pause.tx_pause)
		print_string(" TX");
	if (pause.rx_pause)
		print_string(" RX");
	if (!pause.tx_pause && !pause.rx_pause)
		print_string(" none");
	write_char('\n');
}

/* ---------------------------------------------------------------- counters
 *
 * The words and their sources are the table of mibmap.h. A snapshot of a port (the driver's named
 * statistics and the netdev statistics) is taken when a read starts a new sweep: of a different
 * port, of a counter at or below the previous one (the web page reads 0, 1, 2, ... in turn), or
 * more than a second after the last one. A sweep of all words thus costs one snapshot.
 */
#define MAX_STATS 256	/* driver statistics read per port; a driver with more reads none */

static int stats_attr(const struct nlattr *attr, void *data)
{
	if (mnl_attr_get_type(attr) == IFLA_STATS64 && mnl_attr_get_payload_len(attr) >= sizeof(struct rtnl_link_stats64))
		memcpy(data, mnl_attr_get_payload(attr), sizeof(struct rtnl_link_stats64));
	return MNL_CB_OK;
}

static int stats_msg(const struct nlmsghdr *nlh, void *data)
{
	mnl_attr_parse(nlh, sizeof(struct ifinfomsg), stats_attr, data);
	return MNL_CB_OK;
}

/* The netdev statistics of the port through one RTM_GETLINK; false (st zeroed) if it fails. */
static bool link_stats(uint8_t port, struct rtnl_link_stats64 *st)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_GETLINK, 0);
	struct ifinfomsg *ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);

	memset(st, 0, sizeof *st);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = if_nametoindex(ifname_of(port));
	return ifi->ifi_index && nl_get(nlh, stats_msg, st) == 0;
}

static struct {
	struct ethtool_gstrings h;
	char data[MAX_STATS * ETH_GSTRING_LEN];
} gstr;
static struct {
	struct ethtool_stats h;
	uint64_t data[MAX_STATS];
} gval;

/* The driver's named statistics (ETH_SS_STATS) of the port into gstr/gval; how many, 0 if the
 * driver has none or any step fails. */
static unsigned driver_stats(uint8_t port)
{
	struct {
		struct ethtool_sset_info h;
		uint32_t n;
	} si = { .h = { .cmd = ETHTOOL_GSSET_INFO, .sset_mask = 1ULL << ETH_SS_STATS } };
	unsigned n;

	if (ethtool_cmd(port, &si) < 0 || !(si.h.sset_mask & (1ULL << ETH_SS_STATS)))
		return 0;
	n = si.n;
	if (n == 0 || n > MAX_STATS)
		return 0;
	memset(&gstr.h, 0, sizeof gstr.h);
	gstr.h.cmd = ETHTOOL_GSTRINGS;
	gstr.h.string_set = ETH_SS_STATS;
	gstr.h.len = n;
	if (ethtool_cmd(port, &gstr) < 0)
		return 0;
	memset(&gval.h, 0, sizeof gval.h);
	gval.h.cmd = ETHTOOL_GSTATS;
	gval.h.n_stats = n;
	if (ethtool_cmd(port, &gval) < 0)
		return 0;
	return n;
}

void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked
{
	static struct mib_snap snap;
	static int snap_port = -1, snap_last;
	static int64_t snap_at;
	static bool snap_ok;
	uint64_t v;

	memset(port_buf, 0, 8);
	if (!ifname_of(port) || counter >= NMIB)
		return;
	if (port != snap_port || counter <= snap_last || now_ms() - snap_at > 1000) {
		snap_ok = link_stats(port, &snap.st);
		snap.n = snap_ok ? driver_stats(port) : 0;
		snap.names = gstr.data;
		snap.vals = gval.data;
		snap_port = port;
		snap_at = now_ms();
	}
	snap_last = counter;
	if (!snap_ok)
		return;
	v = mib_word(&snap, counter);
	for (int i = 0; i < 8; i++)
		port_buf[i] = v >> (56 - 8 * i);
}

void port_stats_print(void) __banked
{

	print_string("\nPort\tState\tLink\tTxGood\t\tTxBad\t\tRxGood\t\tRxBad\t\tMACs\n");
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		print_phys_port(i); write_char('\t');
		print_string(port_phy_enabled(i) ? "On\t" : "Off\t");
		print_string(link_names[port_link_get(i)]); write_char('\t');
		for (int k = 0; k < 4; k++) {
			static const uint8_t ctr[4] = { STAT_COUNTER_TX_PKTS, STAT_COUNTER_ERR_PKTS, STAT_COUNTER_RX_PKTS, STAT_COUNTER_ERR_PKTS };
			static const uint8_t off[4] = { 4, 4, 4, 0 };	/* the low 32 bits of the word; RX errors in the high ones */

			port_counter_get(i, ctr[k]);
			print_long((uint32_t)port_buf[off[k]] << 24 | port_buf[off[k] + 1] << 16 | port_buf[off[k] + 2] << 8 | port_buf[off[k] + 3]);
			write_char('\t');
		}
		port_macs_learned_get(i);
		itoa_short(port_buf[2] << 8 | port_buf[3]);
		write_char('\n');
	}
}

/* ---------------------------------------------------------------- chip temperature */

/* The first thermal zone in 1/128 C, or 0 when the machine has none (a container, a VM): said once. */
static bool thermal_read(int32_t *raw)
{
	glob_t g;
	FILE *f;
	long mdeg;
	bool ok = false;

	if (glob("/sys/class/thermal/thermal_zone*/temp", 0, NULL, &g) != 0)
		return false;
	f = fopen(g.gl_pathv[0], "r");
	if (f) {
		if (fscanf(f, "%ld", &mdeg) == 1) {
			*raw = thermal_raw(mdeg);
			ok = true;
		}
		fclose(f);
	}
	globfree(&g);
	return ok;
}

static int16_t temp_at_start;

/* sensor 0 = now, 1 = at power-on (the first reading of the daemon). */
int16_t chip_temp_get(uint8_t sensor) __banked
{
	static bool said;
	int32_t raw;

	if (sensor)
		return temp_at_start;
	if (thermal_read(&raw))
		return raw;
	if (!said) {
		said = true;
		fprintf(stderr, "rtlplayground: no thermal zone: the chip temperature reads 0\n");
	}
	return 0;
}

/* Takes the power-on chip temperature (sensor 1). Call once at startup. */
void ports_init(void)
{
	temp_at_start = chip_temp_get(0);
}
