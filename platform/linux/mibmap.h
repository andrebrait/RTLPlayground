#ifndef _MIBMAP_H_
#define _MIBMAP_H_

/*
 * Counter index c of port_counter_get() is a word of 64 bits in port_buf[0..7], big-endian. The
 * words follow the MIB list of html/app.js (the same ids as the 8051's STAT_COUNTER_*): a word is
 * either one 64-bit counter (wide) or two 32-bit ones, the first name of the list in the upper half.
 *
 * Sources, per counter, in order:
 *   1. a driver counter by name (ETHTOOL_GSTRINGS/GSTATS of ETH_SS_STATS). The names are those the
 *      Realtek Otto DSA driver of the target kernel reports (openwrt main,
 *      target/linux/realtek/files-6.18/drivers/net/dsa/rtl83xx/stats.c, the otto_stats_*_mib_list
 *      tables of RTL838x/839x/930x/931x; e.g. "ifOutDiscards", "tx_BroadcastPkts", "Collisions",
 *      "rx_MacDiscards"), and, for other NICs, the names of e1000e ("rx_broadcast"...);
 *   2. the netdev statistics (RTM_GETLINK IFLA_STATS64, struct rtnl_link_stats64).
 * A counter with neither reads 0. Words with no source at all: the unicast counts (2, 5: the
 * kernel has no unicast counter, and packets minus multicast would count broadcasts as unicast),
 * the broadcast counts (4, 7) on a driver without a "rx_broadcast"/"tx_broadcast"-like name (veth),
 * the pause and control frame counts, the RMON size histograms (21..26, 28), the per-queue counters
 * (38..45: the Otto driver has none) and the PHY side counters (49..51: a netdev has no PHY side);
 * words past 51 are 0. The Otto driver exposes its standard counters (frame sizes, pause frames)
 * only through the standard ethtool stats groups, not as ETH_SS_STATS strings, so they are not read.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* the 8051 storage-class shim defines __data, which linux/socket.h (via if_link.h) uses as a member name */
#pragma push_macro("__data")
#undef __data
#include <linux/ethtool.h>
#include <linux/if_link.h>
#pragma pop_macro("__data")

/* netdev statistics a counter may fall back to */
enum st { ST_NONE, ST_RX_BYTES, ST_TX_BYTES, ST_RX_PKTS, ST_TX_PKTS, ST_RX_ERRORS, ST_TX_ERRORS,
	  ST_RX_DROPPED, ST_TX_DROPPED, ST_MULTICAST, ST_COLLISIONS, ST_RX_LENGTH, ST_RX_CRC,
	  ST_RX_FRAME, ST_RX_MISSED, ST_TX_ABORTED, ST_TX_WINDOW };

/* One counter: the driver names tried in order, then the netdev statistic. */
struct mibsrc {
	uint8_t st;
	const char *name[3];
};
struct mibword {
	bool wide;
	struct mibsrc hi, lo;
};
#define S(st, ...)	{ st, { __VA_ARGS__ } }
#define WIDE(a)		{ true, a, S(ST_NONE) }
#define PAIR(a, b)	{ false, a, b }
#define NOWORD		{ false, S(ST_NONE), S(ST_NONE) }

static const struct mibword mib_map[] = {
	/*  0 */ WIDE(S(ST_RX_BYTES)),					/* Interface in Octets */
	/*  1 */ WIDE(S(ST_TX_BYTES)),					/* Interface out Octets */
	/*  2 */ NOWORD,						/* in Unicast Pkts: no source */
	/*  3 */ WIDE(S(ST_MULTICAST, "rx_multicast")),			/* in Multicast Pkts */
	/*  4 */ WIDE(S(ST_NONE, "rx_broadcast")),			/* in Broadcast Pkts */
	/*  5 */ NOWORD,						/* out Unicast Pkts: no source */
	/*  6 */ WIDE(S(ST_NONE, "tx_multicast", "tx_MulticastPkts")),	/* out Multicast Pkts */
	/*  7 */ WIDE(S(ST_NONE, "tx_broadcast", "tx_BroadcastPkts")),	/* out Broadcast Pkts */
	/*  8 */ PAIR(S(ST_TX_DROPPED, "ifOutDiscards"),		/* out discards, */
		      S(ST_RX_DROPPED, "dot1dTpPortInDiscards")),	/* 802.1d Tp Port in discards */
	/*  9 */ PAIR(S(ST_NONE, "tx_single_coll_ok"), S(ST_NONE, "tx_multi_coll_ok")),
	/* 10 */ PAIR(S(ST_NONE, "tx_deferred_ok"), S(ST_TX_WINDOW)),	/* deferred transmissions, late collisions */
	/* 11 */ PAIR(S(ST_TX_ABORTED), S(ST_NONE)),			/* excessive collisions, symbol errors */
	/* 12 */ NOWORD,						/* unknown opcodes, in pause frames */
	/* 13 */ PAIR(S(ST_NONE), S(ST_RX_MISSED, "DropEvents")),	/* out pause frames, drop events */
	/* 14 */ PAIR(S(ST_NONE, "tx_broadcast", "tx_BroadcastPkts"),	/* TX Ether Broadcast, */
		      S(ST_NONE, "tx_multicast", "tx_MulticastPkts")),	/* Multicast Pkts */
	/* 15 */ PAIR(S(ST_NONE, "tx_CRCAlignErrors"), S(ST_RX_CRC)),	/* TX, RX CRC Align errors */
	/* 16 */ PAIR(S(ST_NONE, "tx_UndersizePkts"), S(ST_NONE)),	/* undersized */
	/* 17 */ PAIR(S(ST_NONE, "tx_OversizePkts"), S(ST_NONE)),	/* oversized */
	/* 18 */ PAIR(S(ST_NONE, "tx_Fragments"), S(ST_NONE)),		/* fragments */
	/* 19 */ PAIR(S(ST_NONE, "tx_Jabbers"), S(ST_NONE)),		/* jabbers */
	/* 20 */ PAIR(S(ST_COLLISIONS, "tx_Collisions", "Collisions"), S(ST_NONE)),	/* TX Collisions, TX 64 Octets */
	/* 21..26 */ NOWORD, NOWORD, NOWORD, NOWORD, NOWORD, NOWORD,	/* size histograms */
	/* 27 */ PAIR(S(ST_NONE, "rx_UndersizeDropPkts"), S(ST_NONE)),	/* RX undersized drop, TX >1518 */
	/* 28 */ NOWORD,						/* RX >1518, TX too large */
	/* 29 */ PAIR(S(ST_NONE), S(ST_NONE, "tx_PktsFlexibleOctetsSet1")),
	/* 30 */ PAIR(S(ST_NONE, "rx_PktsFlexibleOctetsSet1"), S(ST_NONE, "tx_PktsFlexibleOctetsCRCSet1")),
	/* 31 */ PAIR(S(ST_NONE, "rx_PktsFlexibleOctetsCRCSet1"), S(ST_NONE, "tx_PktsFlexibleOctetsSet0")),
	/* 32 */ PAIR(S(ST_NONE, "rx_PktsFlexibleOctetsSet0"), S(ST_NONE, "tx_PktsFlexibleOctetsCRCSet0")),
	/* 33 */ PAIR(S(ST_NONE, "rx_PktsFlexibleOctetsCRCSet0"),	/* ..., Length Field Errors */
		      S(ST_RX_LENGTH, "LengthFieldError", "rx_LengthFieldError")),
	/* 34 */ PAIR(S(ST_NONE, "FalseCarrierTimes", "rx_FalseCarrierTimes"),	/* False Carriers, */
		      S(ST_NONE, "UndersizeOctets", "rx_UnderSizeOctets")),	/* Undersized Octets */
	/* 35 */ PAIR(S(ST_RX_FRAME, "FramingErrors", "rx_FramingErrors"), S(ST_NONE)),
	/* 36 */ PAIR(S(ST_NONE, "rx_MacDiscards"), S(ST_NONE, "rx_MacIPGShortDrop")),
	/* 37 */ NOWORD,						/* learned entry discards */
	/* 38..45 */ NOWORD, NOWORD, NOWORD, NOWORD, NOWORD, NOWORD, NOWORD, NOWORD,	/* egress queues */
	/* 46 */ WIDE(S(ST_TX_PKTS)),					/* TX Good (STAT_COUNTER_TX_PKTS) */
	/* 47 */ WIDE(S(ST_RX_PKTS)),					/* RX Good (STAT_COUNTER_RX_PKTS) */
	/* 48 */ PAIR(S(ST_RX_ERRORS), S(ST_TX_ERRORS)),		/* RX, TX Error (STAT_COUNTER_ERR_PKTS) */
	/* 49..51 */ NOWORD, NOWORD, NOWORD,				/* the PHY side counters */
};
#define NMIB (sizeof mib_map / sizeof mib_map[0])

/* Everything one read of a port's counters returns: the driver's names (n strings of
 * ETH_GSTRING_LEN bytes) with their values, and the netdev statistics. */
struct mib_snap {
	struct rtnl_link_stats64 st;
	unsigned n;
	const char *names;
	const uint64_t *vals;
};

static inline uint64_t mib_st(const struct rtnl_link_stats64 *st, uint8_t s)
{
	switch (s) {
	case ST_RX_BYTES: return st->rx_bytes;
	case ST_TX_BYTES: return st->tx_bytes;
	case ST_RX_PKTS: return st->rx_packets;
	case ST_TX_PKTS: return st->tx_packets;
	case ST_RX_ERRORS: return st->rx_errors;
	case ST_TX_ERRORS: return st->tx_errors;
	case ST_RX_DROPPED: return st->rx_dropped;
	case ST_TX_DROPPED: return st->tx_dropped;
	case ST_MULTICAST: return st->multicast;
	case ST_COLLISIONS: return st->collisions;
	case ST_RX_LENGTH: return st->rx_length_errors;
	case ST_RX_CRC: return st->rx_crc_errors;
	case ST_RX_FRAME: return st->rx_frame_errors;
	case ST_RX_MISSED: return st->rx_missed_errors;
	case ST_TX_ABORTED: return st->tx_aborted_errors;
	case ST_TX_WINDOW: return st->tx_window_errors;
	default: return 0;
	}
}

static inline uint64_t mib_src(const struct mib_snap *s, const struct mibsrc *src)
{
	for (int k = 0; k < 3 && src->name[k]; k++)
		for (unsigned i = 0; i < s->n; i++)
			if (!strncmp(s->names + (size_t)i * ETH_GSTRING_LEN, src->name[k], ETH_GSTRING_LEN))
				return s->vals[i];
	return mib_st(&s->st, src->st);
}

/* The 64-bit word `word` of the counters list; 0 for a word with no source or past the list. */
static inline uint64_t mib_word(const struct mib_snap *s, unsigned word)
{
	const struct mibword *m;

	if (word >= NMIB)
		return 0;
	m = &mib_map[word];
	if (m->wide)
		return mib_src(s, &m->hi);
	return (mib_src(s, &m->hi) & 0xffffffff) << 32 | (mib_src(s, &m->lo) & 0xffffffff);
}

#endif
