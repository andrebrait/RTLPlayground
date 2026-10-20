#ifndef _PORTLOGIC_H_
#define _PORTLOGIC_H_

/*
 * The parts of ports.c that are plain computation on ethtool link-mode masks, so that
 * test/test_portlogic.c can run them on canned masks without a NIC. Include platform.h first
 * (PHY_SPEED_*, PHY_DUPLEX_*).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <linux/ethtool.h>

/* A link mode of ethtool and what the shell shows of it. adv: its bit in port_phy_adv_get()
 * (bits 5..0 = 2500F, 1000F, 100F, 100H, 10F, 10H; 0 = not among the six). speed: PHY_SPEED_*
 * of the `port <n> <speed>` command that selects it (0: no command selects it; the 8051 has no
 * 1000 half either). */
struct mode {
	int bit;
	uint8_t adv;
	uint8_t speed;
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

/* ETHTOOL_GLINKSETTINGS answer: the fixed part, then the supported, advertised and
 * partner-advertised link mode masks of base.link_mode_masks_nwords 32-bit words each. */
#define MASK_WORDS_MAX 127
enum { MASK_SUPPORTED, MASK_ADVERTISING, MASK_PARTNER };
struct elink {
	struct ethtool_link_settings base;
	uint32_t masks[3 * MASK_WORDS_MAX];
};

static inline bool mode_in(const struct elink *e, int which, int bit)
{
	int n = e->base.link_mode_masks_nwords;

	if (bit >= 32 * n)
		return false;
	return e->masks[which * n + bit / 32] >> (bit % 32) & 1;
}

static inline void mode_set(struct elink *e, int which, int bit, bool on)
{
	int n = e->base.link_mode_masks_nwords;

	if (bit >= 32 * n)
		return;
	if (on)
		e->masks[which * n + bit / 32] |= (uint32_t)1 << (bit % 32);
	else
		e->masks[which * n + bit / 32] &= ~((uint32_t)1 << (bit % 32));
}

/* port_phy_adv_get(): the advertised modes among the six of platform.h. */
static inline uint8_t adv_bits(const struct elink *e)
{
	uint8_t r = 0;

	for (unsigned i = 0; i < NMODES; i++)
		if (mode_in(e, MASK_ADVERTISING, modes[i].bit))
			r |= modes[i].adv;
	return r;
}

/*
 * `port <n> <speed> [duplex]`: sets the advertised modes of e to those of the speed (duplex is
 * PHY_DUPLEX_*), or for PHY_SPEED_AUTO to every table mode the port supports. Modes outside the
 * table are left as they are. Returns false, e untouched, for a speed and duplex no table mode
 * has (`port 1 2g5 half`), and for a mode a port that reports its modes does not support (a driver
 * such as e1000e would accept the request and advertise nothing); *any = false when nothing was selected (auto on a netdev that
 * supports none of the table modes, such as veth): the caller must not write the settings back.
 */
static inline bool speed_masks(struct elink *e, uint8_t speed, uint8_t duplex, bool *any)
{
	bool want[NMODES], found = false, reports = false;

	for (unsigned i = 0; i < NMODES; i++)
		reports |= mode_in(e, MASK_SUPPORTED, modes[i].bit);
	for (unsigned i = 0; i < NMODES; i++) {
		if (speed == PHY_SPEED_AUTO)
			want[i] = mode_in(e, MASK_SUPPORTED, modes[i].bit);
		else
			want[i] = modes[i].speed == speed &&
				  (duplex == PHY_DUPLEX_BOTH || (duplex == PHY_DUPLEX_FULL) == modes[i].full) &&
				  (!reports || mode_in(e, MASK_SUPPORTED, modes[i].bit));
		found |= want[i];
	}
	*any = found;
	if (speed != PHY_SPEED_AUTO && !found)
		return false;
	for (unsigned i = 0; i < NMODES; i++)
		mode_set(e, MASK_ADVERTISING, modes[i].bit, want[i]);
	return true;
}

/*
 * `port <n> duplex full|half`, as the 8051 does it (rtl837x_phy.c phy_set_duplex): of the 10M
 * modes and of the 100M modes, each family that is advertised at all is advertised at the
 * requested duplex only; the modes of 1G and up (other registers on the 8051) stay. False, e
 * untouched, when no 10M or 100M mode is advertised.
 */
static inline bool duplex_masks(struct elink *e, bool full)
{
	static const struct { int half, full; } fam[2] = {
		{ ETHTOOL_LINK_MODE_10baseT_Half_BIT, ETHTOOL_LINK_MODE_10baseT_Full_BIT },
		{ ETHTOOL_LINK_MODE_100baseT_Half_BIT, ETHTOOL_LINK_MODE_100baseT_Full_BIT },
	};
	bool on[2];

	for (int i = 0; i < 2; i++)
		on[i] = mode_in(e, MASK_ADVERTISING, fam[i].half) || mode_in(e, MASK_ADVERTISING, fam[i].full);
	if (!on[0] && !on[1])
		return false;
	for (int i = 0; i < 2; i++) {
		if (!on[i])
			continue;
		mode_set(e, MASK_ADVERTISING, fam[i].half, !full);
		mode_set(e, MASK_ADVERTISING, fam[i].full, full);
	}
	return true;
}

/* A thermal zone's temperature in millidegrees C as chip_temp_get() reports it, 1/128 C. */
static inline int16_t thermal_raw(int64_t millideg)
{
	return (int16_t)(millideg * 128 / 1000);
}

#endif
