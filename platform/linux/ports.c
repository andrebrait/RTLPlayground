/*
 * Ports of the board as the kernel sees them: logical port N is the netdev board_ifnames[N].
 * Link state and settings come from sysfs and ethtool (SIOCETHTOOL, ETHTOOL_G/SLINKSETTINGS), the
 * MIB counters from the netdev statistics (RTM_GETLINK, IFLA_STATS64) and, for the
 * broadcast/multicast counts the kernel does not keep, from packet sockets (see "Counters").
 * Every kernel call is checked: a failed change prints an error through cmd_error() and leaves
 * the previous state; a failed read reads as zero or down.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <glob.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* the 8051 storage-class shim defines __data, which linux/socket.h uses as a member name */
#pragma push_macro("__data")
#undef __data
#include <libmnl/libmnl.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#pragma pop_macro("__data")

#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"
#include "linkcode.h"

#define NPORTS 9

static const char *ifname_of(uint8_t port)
{
	return port < NPORTS ? board_ifnames[port] : NULL;
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
 * 64..16383): the longest frame the MAC accepts, counted on the wire, i.e. destination and source
 * address, one VLAN tag, EtherType, payload and FCS. The default the project shows for a fresh port
 * is 1522 (tools/httpd_sim.c, test/platform_mock.c) = 1500 + 14 + 4 + 4, the 802.3ac maximum.
 * [INFERENCE] The repository holds no register documentation: the 22 comes from that default.
 * A Linux netdev MTU is the payload only, so MTU = V - 22 and V = MTU + 22; `mtu show` and
 * mtu.json report exactly what `mtu` set. A V below 90 (netdev MTU under the kernel's 68) or one the
 * driver refuses is an error. A bridge over the ports follows by itself: the kernel sets the MTU
 * of a bridge whose MTU nobody set to the minimum of its ports, on every port MTU change
 * (br_mtu_auto_adjust); this daemon never sets a bridge MTU.
 */
#define MTU_FRAME_OVERHEAD 22

uint16_t port_mtu_get(uint8_t port) __banked
{
	struct ifreq ifr = ifr_for(port);

	if (ioctl(ctl_fd(), SIOCGIFMTU, &ifr) < 0)
		return 0;
	return ifr.ifr_mtu + MTU_FRAME_OVERHEAD;
}

void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked
{
	struct ifreq ifr = ifr_for(port);

	if (!ifname_of(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	ifr.ifr_mtu = mtu - MTU_FRAME_OVERHEAD;
	if (ioctl(ctl_fd(), SIOCSIFMTU, &ifr) < 0)
		port_error(port, "cannot set the MTU");
}

/* ---------------------------------------------------------------- link settings */

/* A link mode of ethtool and what the shell shows of it. adv: its bit in port_phy_adv_get()
 * (bits 5..0 = 2500F, 1000F, 100F, 100H, 10F, 10H; 0 = not among the six). */
struct mode {
	int bit;
	uint8_t adv;
	uint8_t speed;	/* PHY_SPEED_* for the speed commands, 0 = never selected by one */
	bool full;
	const char *name;
};

static const struct mode modes[] = {
	{ ETHTOOL_LINK_MODE_10baseT_Half_BIT,    0x01, PHY_SPEED_10M,  false, "10Base-Half" },
	{ ETHTOOL_LINK_MODE_10baseT_Full_BIT,    0x02, PHY_SPEED_10M,  true,  "10Base-Full" },
	{ ETHTOOL_LINK_MODE_100baseT_Half_BIT,   0x04, PHY_SPEED_100M, false, "100Base-Half" },
	{ ETHTOOL_LINK_MODE_100baseT_Full_BIT,   0x08, PHY_SPEED_100M, true,  "100Base-Full" },
	{ ETHTOOL_LINK_MODE_1000baseT_Half_BIT,  0x00, 0,              false, "1000Base-Half" },
	{ ETHTOOL_LINK_MODE_1000baseT_Full_BIT,  0x10, PHY_SPEED_1G,   true,  "1000Base-Full" },
	{ ETHTOOL_LINK_MODE_2500baseT_Full_BIT,  0x20, PHY_SPEED_2G5,  true,  "2500Base-Full" },
	{ ETHTOOL_LINK_MODE_5000baseT_Full_BIT,  0x00, PHY_SPEED_5G,   true,  "5000Base-Full" },
	{ ETHTOOL_LINK_MODE_10000baseT_Full_BIT, 0x00, PHY_SPEED_10G,  true,  "10GBase-Full" },
};
#define NMODES (sizeof modes / sizeof modes[0])

/* ETHTOOL_GLINKSETTINGS answer: the fixed part, then supported, advertised and partner-advertised
 * link mode masks of base.link_mode_masks_nwords 32-bit words each. */
#define MASK_WORDS_MAX 127
struct elink {
	struct ethtool_link_settings base;
	uint32_t masks[3 * MASK_WORDS_MAX];
};

static bool mode_in(const struct elink *e, int which, int bit)	/* which: 0 supported, 1 advertising, 2 partner */
{
	int n = e->base.link_mode_masks_nwords;

	if (bit >= 32 * n)
		return false;
	return e->masks[which * n + bit / 32] >> (bit % 32) & 1;
}

static void mode_set(struct elink *e, int which, int bit, bool on)
{
	int n = e->base.link_mode_masks_nwords;

	if (bit >= 32 * n)
		return;
	if (on)
		e->masks[which * n + bit / 32] |= (uint32_t)1 << (bit % 32);
	else
		e->masks[which * n + bit / 32] &= ~((uint32_t)1 << (bit % 32));
}

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
	uint8_t r = 0;

	if (elink_get(port, &e) < 0)
		return 0;
	for (unsigned i = 0; i < NMODES; i++)
		if (mode_in(&e, 1, modes[i].bit))
			r |= modes[i].adv;
	return r;
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
 * that speed; auto advertises everything the port supports; off takes the port down. Nothing
 * changes when the driver refuses (veth does).
 * shortcut: auto on a netdev that supports no speed modes (veth) only brings it up: there is
 * nothing to advertise, and refusing would leave `port N on` unable to undo `port N off`.
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
		e.base.link_mode_masks_nwords = 0;	/* nothing to advertise */
	}
	for (unsigned i = 0; i < NMODES; i++) {
		bool want;

		if (phy_settings.speed == PHY_SPEED_AUTO)
			want = mode_in(&e, 0, modes[i].bit);
		else
			want = modes[i].speed == phy_settings.speed &&
			       (phy_settings.duplex == PHY_DUPLEX_BOTH || (phy_settings.duplex == PHY_DUPLEX_FULL) == modes[i].full);
		any |= want;
		mode_set(&e, 1, modes[i].bit, want);
	}
	if (phy_settings.speed != PHY_SPEED_AUTO && !any) {
		cmd_error("no such speed and duplex\n");
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

/* `port <n> duplex full|half`: of the speeds advertised now, keep the modes of that duplex. */
void phy_set_duplex(void) __banked
{
	uint8_t port = phy_settings.port;
	bool full = phy_settings.duplex == PHY_DUPLEX_FULL, any = false;
	struct elink e;

	if (!ifname_of(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	if (elink_get(port, &e) < 0) {
		port_error(port, "cannot read the link settings");
		return;
	}
	for (unsigned i = 0; i < NMODES; i++) {
		if (modes[i].full != full)
			mode_set(&e, 1, modes[i].bit, false);
		any |= mode_in(&e, 1, modes[i].bit);
	}
	if (!any) {
		cmd_error("no advertised speed has that duplex\n");
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
	static const char *const names[] = { "Down", "10M", "100M", "1000M", "500M", "10G", "2500M", "5G" };

	print_string("\nLink speed: ");
	print_string(names[code]);
	if (elink_get(port, &e) < 0) {
		write_char('\n');
		return;
	}
	if (code && e.base.duplex != DUPLEX_UNKNOWN)
		print_string(e.base.duplex == DUPLEX_FULL ? " full duplex" : " half duplex");
	if (e.base.autoneg == AUTONEG_ENABLE) {
		print_string("\nAN enabled, advertising:");
		print_modes(&e, 1);
		print_string("\nLink Partner advertises:");
		print_modes(&e, 2);
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
 * Counter index c of port_counter_get() is a word of 64 bits in port_buf[0..7], big-endian. The
 * words follow the MIB list of html/app.js (the same ids as the 8051's STAT_COUNTER_*): a word is
 * either one 64-bit counter (WIDE) or two 32-bit ones, the first name in the upper half. The
 * kernel's statistics (struct rtnl_link_stats64) fill what they have a counterpart for; the
 * receive/transmit broadcast and multicast frame counts come from packet sockets (below); the
 * rest has none and is 0:
 *   - the frame size histograms, flexible octet sets, per-queue counters, pause/control frame
 *     counts, undersize/oversize/fragment/jabber counters and the MAC-internal discards: neither
 *     the netdev statistics nor ethtool's generic ones carry them for every driver, and a veth
 *     keeps none;
 *   - the "PHY" side good/error counters (words 49..51): a netdev has no PHY-side counters.
 * Words past the end of the list (the page asks for 0..54) are 0.
 */
enum src {
	S_NONE,
	S_RX_BYTES, S_TX_BYTES, S_RX_PKTS, S_TX_PKTS, S_RX_ERRORS, S_TX_ERRORS, S_RX_DROPPED, S_TX_DROPPED,
	S_COLLISIONS, S_RX_LENGTH, S_RX_CRC, S_RX_FRAME, S_RX_MISSED, S_TX_ABORTED, S_TX_WINDOW,
	S_RX_UCAST, S_TX_UCAST,	/* packets minus broadcast and multicast */
	S_RX_MCAST, S_RX_BCAST, S_TX_MCAST, S_TX_BCAST,	/* the packet sockets, in this order */
	S_COUNT
};
#define NPKT 4	/* packet socket kinds: S_RX_MCAST .. S_TX_BCAST */

struct mib {
	uint8_t hi, lo;	/* source of the upper / lower 32 bits */
	bool wide;	/* hi is the 64-bit source of the whole word */
};
#define WIDE(a)	 { a, S_NONE, true }
#define PAIR(a, b) { a, b, false }
#define NOTHING	 { S_NONE, S_NONE, false }

static const struct mib mib_map[] = {
	/*  0 */ WIDE(S_RX_BYTES),				/* Interface in Octets */
	/*  1 */ WIDE(S_TX_BYTES),				/* Interface out Octets */
	/*  2 */ WIDE(S_RX_UCAST),				/* in Unicast Pkts */
	/*  3 */ WIDE(S_RX_MCAST),				/* in Multicast Pkts */
	/*  4 */ WIDE(S_RX_BCAST),				/* in Broadcast Pkts */
	/*  5 */ WIDE(S_TX_UCAST),				/* out Unicast Pkts */
	/*  6 */ WIDE(S_TX_MCAST),				/* out Multicast Pkts */
	/*  7 */ WIDE(S_TX_BCAST),				/* out Broadcast Pkts */
	/*  8 */ PAIR(S_TX_DROPPED, S_RX_DROPPED),		/* out discards, 802.1d Tp Port in discards */
	/*  9 */ NOTHING,					/* single, multiple collision frames: only a total exists */
	/* 10 */ PAIR(S_NONE, S_TX_WINDOW),			/* deferred transmissions, late collisions */
	/* 11 */ PAIR(S_TX_ABORTED, S_NONE),			/* excessive collisions, symbol errors */
	/* 12 */ NOTHING,					/* unknown opcodes, in pause frames */
	/* 13 */ PAIR(S_NONE, S_RX_MISSED),			/* out pause frames, drop events */
	/* 14 */ PAIR(S_TX_BCAST, S_TX_MCAST),			/* TX Ether Broadcast, Multicast Pkts */
	/* 15 */ PAIR(S_NONE, S_RX_CRC),			/* TX, RX CRC Align errors */
	/* 16..19 */ NOTHING, NOTHING, NOTHING, NOTHING,	/* undersized, oversized, fragments, jabbers */
	/* 20 */ PAIR(S_COLLISIONS, S_NONE),			/* TX Ether Collisions, TX 64 Octets */
	/* 21..32 */ NOTHING, NOTHING, NOTHING, NOTHING, NOTHING, NOTHING,	/* size histograms, */
	NOTHING, NOTHING, NOTHING, NOTHING, NOTHING, NOTHING,	/* flexible octets */
	/* 33 */ PAIR(S_NONE, S_RX_LENGTH),			/* RX flexible CRC set 0, Length Field Errors */
	/* 34 */ NOTHING,					/* false carriers, undersized octets */
	/* 35 */ PAIR(S_RX_FRAME, S_NONE),			/* Framing Errors */
	/* 36..45 */ NOTHING, NOTHING, NOTHING, NOTHING, NOTHING,	/* MAC discards, learning, */
	NOTHING, NOTHING, NOTHING, NOTHING, NOTHING,		/* egress queues */
	/* 46 */ WIDE(S_TX_PKTS),				/* TX Good Counter (STAT_COUNTER_TX_PKTS) */
	/* 47 */ WIDE(S_RX_PKTS),				/* RX Good Counter (STAT_COUNTER_RX_PKTS) */
	/* 48 */ PAIR(S_RX_ERRORS, S_TX_ERRORS),		/* RX, TX Error Counter (STAT_COUNTER_ERR_PKTS) */
	/* 49..51 */ NOTHING, NOTHING, NOTHING,		/* the PHY side counters */
};
#define NMIB (sizeof mib_map / sizeof mib_map[0])

/* ------- packet socket counts: broadcast and multicast frames, received and transmitted
 *
 * Neither rtnl_link_stats64 nor sysfs counts broadcast frames, and `multicast` (received) is 0 on
 * veth, so each port gets four packet sockets bound to its netdev with a classic BPF filter that
 * passes only the frames of one kind (received/sent x broadcast/multicast-but-not-broadcast,
 * decided on the destination MAC). The kernel counts each frame the filter passes
 * (PACKET_STATISTICS: delivered plus dropped); the frames themselves are cut to one byte and the
 * receive buffer is the minimum, so what piles up unread is bounded. Reading the statistics
 * resets them, so they are summed here. Counting starts when the daemon opens the sockets
 * (ports_init); a netdev that appears later is picked up on the next read.
 */
static int pkt_fd[NPORTS][NPKT];
static uint64_t pkt_sum[NPORTS][NPKT];

static void pkt_filter(unsigned kind, struct sock_filter f[11])
{
	bool tx = kind >= S_TX_MCAST - S_RX_MCAST, bcast = kind & 1;
	/* drop = 10, not-broadcast verdict = 8, broadcast verdict = 9 */
	struct sock_filter prog[11] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SKF_AD_OFF + SKF_AD_PKTTYPE),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PACKET_OUTGOING, tx ? 0 : 8, tx ? 8 : 0),
		BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 1, 0, 6),	/* group bit of the destination */
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffff, 0, 2),
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffff, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, bcast ? 0 : 1),	/* not the broadcast address */
		BPF_STMT(BPF_RET | BPF_K, bcast ? 1 : 0),	/* the broadcast address */
		BPF_STMT(BPF_RET | BPF_K, 0),
	};

	memcpy(f, prog, sizeof prog);
}

static void pkt_open(uint8_t port, unsigned kind)
{
	static bool warned[NPORTS], inited;
	struct sock_filter code[11];
	struct sock_fprog fprog = { .len = 11, .filter = code };
	struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL) };
	int fd, rcvbuf = 1;
	const char *step;

	if (!inited) {	/* fd 0 is a valid descriptor, so zero cannot mean "not open" */
		memset(pkt_fd, -1, sizeof pkt_fd);
		inited = true;
	}
	if (pkt_fd[port][kind] >= 0 || !ifname_of(port))
		return;
	sll.sll_ifindex = if_nametoindex(ifname_of(port));
	pkt_filter(kind, code);
	/* protocol 0 until bound, so that nothing is queued before the filter is in place */
	fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	step = "socket";
	if (fd >= 0 && (step = "filter", setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof fprog) == 0) &&
	    (step = "buffer", setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) == 0) &&
	    (step = "bind", sll.sll_ifindex && bind(fd, (struct sockaddr *)&sll, sizeof sll) == 0)) {
		pkt_fd[port][kind] = fd;
		return;
	}
	if (!warned[port]) {
		warned[port] = true;
		fprintf(stderr, "rtlplayground: %s: no frame counter (%s: %s)\n", ifname_of(port), step,
			sll.sll_ifindex ? strerror(errno) : "no such netdev");
	}
	if (fd >= 0)
		close(fd);
}

static uint64_t pkt_count(uint8_t port, unsigned kind)
{
	struct tpacket_stats st;
	socklen_t len = sizeof st;

	pkt_open(port, kind);
	if (pkt_fd[port][kind] < 0)
		return 0;
	if (getsockopt(pkt_fd[port][kind], SOL_PACKET, PACKET_STATISTICS, &st, &len) == 0)
		pkt_sum[port][kind] += st.tp_packets;
	return pkt_sum[port][kind];
}

/* ------- netdev statistics: one RTM_GETLINK for the port's netdev */
static struct mnl_socket *nl;

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

static bool link_stats(uint8_t port, struct rtnl_link_stats64 *st)
{
	static char buf[32768];
	unsigned seq = 1;
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ifinfomsg *ifi;
	ssize_t n;

	memset(st, 0, sizeof *st);
	if (!nl && !(nl = mnl_socket_open(NETLINK_ROUTE)))
		return false;
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = seq;
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = if_nametoindex(ifname_of(port));
	if (!ifi->ifi_index || mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		return false;
	n = mnl_socket_recvfrom(nl, buf, sizeof buf);
	if (n < 0)
		return false;
	return mnl_cb_run(buf, n, seq, mnl_socket_get_portid(nl), stats_msg, st) >= 0;
}

/* Counters read per call of port_counter_get(): one snapshot of every source. */
static uint64_t src_value(enum src s, const struct rtnl_link_stats64 *st, uint8_t port)
{
	uint64_t p, b, m;

	switch (s) {
	case S_RX_BYTES: return st->rx_bytes;
	case S_TX_BYTES: return st->tx_bytes;
	case S_RX_PKTS: return st->rx_packets;
	case S_TX_PKTS: return st->tx_packets;
	case S_RX_ERRORS: return st->rx_errors;
	case S_TX_ERRORS: return st->tx_errors;
	case S_RX_DROPPED: return st->rx_dropped;
	case S_TX_DROPPED: return st->tx_dropped;
	case S_COLLISIONS: return st->collisions;
	case S_RX_LENGTH: return st->rx_length_errors;
	case S_RX_CRC: return st->rx_crc_errors;
	case S_RX_FRAME: return st->rx_frame_errors;
	case S_RX_MISSED: return st->rx_missed_errors;
	case S_TX_ABORTED: return st->tx_aborted_errors;
	case S_TX_WINDOW: return st->tx_window_errors;
	case S_RX_MCAST: case S_RX_BCAST: case S_TX_MCAST: case S_TX_BCAST:
		return pkt_count(port, s - S_RX_MCAST);
	case S_RX_UCAST: case S_TX_UCAST:
		p = s == S_RX_UCAST ? st->rx_packets : st->tx_packets;
		m = pkt_count(port, s == S_RX_UCAST ? S_RX_MCAST - S_RX_MCAST : S_TX_MCAST - S_RX_MCAST);
		b = pkt_count(port, s == S_RX_UCAST ? S_RX_BCAST - S_RX_MCAST : S_TX_BCAST - S_RX_MCAST);
		return p > m + b ? p - m - b : 0;
	default: return 0;
	}
}

void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked
{
	struct rtnl_link_stats64 st;
	const struct mib *m;
	uint64_t v;

	memset(port_buf, 0, 8);
	if (!ifname_of(port) || counter >= NMIB)
		return;
	m = &mib_map[counter];
	if (m->hi == S_NONE && m->lo == S_NONE)
		return;
	if (!link_stats(port, &st))
		return;
	if (m->wide)
		v = src_value(m->hi, &st, port);
	else
		v = (src_value(m->hi, &st, port) & 0xffffffff) << 32 | (src_value(m->lo, &st, port) & 0xffffffff);
	for (int i = 0; i < 8; i++)
		port_buf[i] = v >> (56 - 8 * i);
}

/* Learned MACs of the port: the bridge forwarding table is not read before Task 7 (L2 table). */
void port_macs_learned_get(uint8_t port) __banked
{
	memset(port_buf, 0, 4);
}

void port_stats_print(void) __banked
{
	static const char *const names[] = { "Down", "10M", "100M", "1000M", "500M", "10G", "2.5G", "5G" };

	print_string("\nPort\tState\tLink\tTxGood\t\tTxBad\t\tRxGood\t\tRxBad\t\tMACs\n");
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		print_phys_port(i); write_char('\t');
		print_string(port_phy_enabled(i) ? "On\t" : "Off\t");
		print_string(names[port_link_get(i)]); write_char('\t');
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
			*raw = mdeg * 128 / 1000;
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

/* Opens the frame counters of every port of the board and takes the power-on temperature. */
void ports_init(void)
{
	for (uint8_t p = 0; p < NPORTS; p++)
		for (unsigned k = 0; k < NPKT; k++)
			pkt_open(p, k);
	temp_at_start = chip_temp_get(0);
}
