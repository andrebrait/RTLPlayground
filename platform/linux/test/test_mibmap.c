/* mib_word(): the counters table of mibmap.h on canned driver names and values, as the Realtek
 * Otto DSA driver (rtl83xx/stats.c, RTL930x list) and e1000e report them, and on a veth (none). */
#include <stdio.h>

#include "platform.h"
#include "mibmap.h"

static int fails;

static void check(const char *what, uint64_t want, uint64_t got)
{
	if (want != got) {
		printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", what, (unsigned long long)want, (unsigned long long)got);
		fails++;
	} else {
		printf("ok:   %s\n", what);
	}
}

/* ETH_GSTRING_LEN bytes per name, as ETHTOOL_GSTRINGS returns them */
#define MAXN 64
static char names[MAXN * ETH_GSTRING_LEN];
static uint64_t vals[MAXN];
static struct mib_snap snap;

static void add(const char *name, uint64_t v)
{
	snprintf(names + (size_t)snap.n * ETH_GSTRING_LEN, ETH_GSTRING_LEN, "%s", name);
	vals[snap.n++] = v;
}

static void reset(void)
{
	memset(names, 0, sizeof names);
	memset(vals, 0, sizeof vals);
	memset(&snap, 0, sizeof snap);
	snap.names = names;
	snap.vals = vals;
}

int main(void)
{
	/* ---- a veth: no driver statistics, the netdev statistics only ---- */
	reset();
	snap.st = (struct rtnl_link_stats64){ .rx_bytes = 1000, .tx_bytes = 2000, .rx_packets = 10, .tx_packets = 20,
		.rx_errors = 3, .tx_errors = 4, .rx_dropped = 5, .tx_dropped = 6, .multicast = 7, .collisions = 8,
		.rx_length_errors = 9, .rx_crc_errors = 11, .rx_frame_errors = 12, .rx_missed_errors = 13,
		.tx_aborted_errors = 14, .tx_window_errors = 15 };
	check("veth word 0 in octets", 1000, mib_word(&snap, 0));
	check("veth word 1 out octets", 2000, mib_word(&snap, 1));
	check("veth word 3 in multicast (netdev statistic)", 7, mib_word(&snap, 3));
	check("veth word 4 in broadcast has no source", 0, mib_word(&snap, 4));
	check("veth word 2 and 5 unicast have no source", 0, mib_word(&snap, 2) | mib_word(&snap, 5));
	check("veth word 8: tx_dropped | rx_dropped halves", 6ULL << 32 | 5, mib_word(&snap, 8));
	check("veth word 10: late collisions in the lower half", 15, mib_word(&snap, 10));
	check("veth word 11: excessive collisions in the upper half", 14ULL << 32, mib_word(&snap, 11));
	check("veth word 13: rx_missed as drop events (lower)", 13, mib_word(&snap, 13));
	check("veth word 15: rx_crc (lower)", 11, mib_word(&snap, 15));
	check("veth word 20: collisions (upper)", 8ULL << 32, mib_word(&snap, 20));
	check("veth word 33: length errors (lower)", 9, mib_word(&snap, 33));
	check("veth word 35: framing errors (upper)", 12ULL << 32, mib_word(&snap, 35));
	check("word 46 TX Good", 20, mib_word(&snap, 46));
	check("word 47 RX Good", 10, mib_word(&snap, 47));
	check("word 48: rx errors | tx errors", 3ULL << 32 | 4, mib_word(&snap, 48));
	for (unsigned w = 21; w <= 26; w++)
		check("histogram word is 0", 0, mib_word(&snap, w));
	check("queue word 40 is 0", 0, mib_word(&snap, 40));
	check("PHY side word 49 is 0", 0, mib_word(&snap, 49));
	check("past the list: word 52", 0, mib_word(&snap, 52));
	check("past the list: word 200", 0, mib_word(&snap, 200));

	/* ---- the Otto driver (RTL930x list of stats.c): names win over the netdev statistics ---- */
	reset();
	snap.st = (struct rtnl_link_stats64){ .rx_packets = 100, .tx_packets = 200, .tx_dropped = 77, .rx_dropped = 66,
		.rx_length_errors = 55, .collisions = 44, .rx_missed_errors = 33 };
	add("dot1dTpPortInDiscards", 9);
	add("ifOutDiscards", 7);
	add("DropEvents", 0x1234);
	add("tx_BroadcastPkts", 3);
	add("tx_MulticastPkts", 4);
	add("tx_CRCAlignErrors", 5);
	add("tx_UndersizePkts", 6);
	add("tx_OversizePkts", 8);
	add("tx_Fragments", 10);
	add("tx_Jabbers", 11);
	add("tx_Collisions", 12);
	add("rx_UndersizeDropPkts", 13);
	add("tx_PktsFlexibleOctetsSet1", 14);
	add("rx_PktsFlexibleOctetsSet1", 15);
	add("tx_PktsFlexibleOctetsCRCSet1", 16);
	add("rx_PktsFlexibleOctetsCRCSet1", 17);
	add("tx_PktsFlexibleOctetsSet0", 18);
	add("rx_PktsFlexibleOctetsSet0", 19);
	add("tx_PktsFlexibleOctetsCRCSet0", 20);
	add("rx_PktsFlexibleOctetsCRCSet0", 21);
	add("LengthFieldError", 22);
	add("FalseCarrierTimes", 23);
	add("UndersizeOctets", 24);
	add("FramingErrors", 25);
	add("ParserErrors", 26);
	add("rx_MacDiscards", 27);
	add("rx_MacIPGShortDrop", 28);
	check("otto word 8: ifOutDiscards upper, dot1dTpPortInDiscards lower (not the netdev's 77/66)", 7ULL << 32 | 9, mib_word(&snap, 8));
	check("otto word 13: DropEvents beats rx_missed_errors", 0x1234, mib_word(&snap, 13));
	check("otto word 14: tx broadcast | multicast", 3ULL << 32 | 4, mib_word(&snap, 14));
	check("otto word 7: out broadcast", 3, mib_word(&snap, 7));
	check("otto word 6: out multicast", 4, mib_word(&snap, 6));
	check("otto word 15: tx crc upper", 5ULL << 32, mib_word(&snap, 15));
	check("otto word 16..19", 6ULL << 32, mib_word(&snap, 16));
	check("otto word 17", 8ULL << 32, mib_word(&snap, 17));
	check("otto word 18", 10ULL << 32, mib_word(&snap, 18));
	check("otto word 19", 11ULL << 32, mib_word(&snap, 19));
	check("otto word 20: Collisions beats the netdev's 44", 12ULL << 32, mib_word(&snap, 20));
	check("otto word 27: rx undersize drop", 13ULL << 32, mib_word(&snap, 27));
	check("otto word 29 lower: tx flexible set 1", 14, mib_word(&snap, 29));
	check("otto word 30", 15ULL << 32 | 16, mib_word(&snap, 30));
	check("otto word 31", 17ULL << 32 | 18, mib_word(&snap, 31));
	check("otto word 32", 19ULL << 32 | 20, mib_word(&snap, 32));
	check("otto word 33: flexible CRC set 0 | LengthFieldError beats the netdev's 55", 21ULL << 32 | 22, mib_word(&snap, 33));
	check("otto word 34: false carriers | undersize octets", 23ULL << 32 | 24, mib_word(&snap, 34));
	check("otto word 35: framing errors", 25ULL << 32, mib_word(&snap, 35));
	check("otto word 36: mac discards | ipg short drop", 27ULL << 32 | 28, mib_word(&snap, 36));
	check("otto word 46 still from the netdev", 200, mib_word(&snap, 46));
	check("otto word 47 still from the netdev", 100, mib_word(&snap, 47));
	check("otto ParserErrors maps to no word", 0, mib_word(&snap, 37));
	/* a 33-bit value does not leak into the other half */
	vals[0] = 0x1ffffffffULL;	/* dot1dTpPortInDiscards */
	vals[1] = 8;			/* ifOutDiscards: bit 32 of the word is clear */
	check("32-bit halves are truncated", 8ULL << 32 | 0xffffffff, mib_word(&snap, 8));

	/* ---- e1000e names: broadcast and multicast counts ---- */
	reset();
	snap.st = (struct rtnl_link_stats64){ .multicast = 4, .rx_packets = 50 };
	add("rx_packets", 50);
	add("rx_broadcast", 12);
	add("tx_broadcast", 13);
	add("rx_multicast", 14);
	add("tx_multicast", 15);
	check("e1000e word 4: rx_broadcast", 12, mib_word(&snap, 4));
	check("e1000e word 7: tx_broadcast", 13, mib_word(&snap, 7));
	check("e1000e word 3: rx_multicast beats the netdev's 4", 14, mib_word(&snap, 3));
	check("e1000e word 6: tx_multicast", 15, mib_word(&snap, 6));
	check("e1000e word 14", 13ULL << 32 | 15, mib_word(&snap, 14));

	/* a name cut at ETH_GSTRING_LEN must not match a shorter one */
	reset();
	add("tx_BroadcastPkts_extra", 99);
	check("a longer name is not the name", 0, mib_word(&snap, 14));
	return fails != 0;
}
