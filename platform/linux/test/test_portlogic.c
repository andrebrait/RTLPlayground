/* portlogic.h on canned link-mode masks: the mode table, the six advertisement bits of
 * port_phy_adv_get(), `port N <speed>` and `port N duplex`, and the temperature scaling. */
#include <stdio.h>

#include "platform.h"
#include "portlogic.h"

static int fails;

static void check(const char *what, long want, long got)
{
	if (want != got) {
		printf("FAIL: %s: expected 0x%lx, got 0x%lx\n", what, want, got);
		fails++;
	} else {
		printf("ok:   %s\n", what);
	}
}

/* masks: 2 words per mask, as a 64-bit-mode-space driver reports them */
static void blank(struct elink *e)
{
	memset(e, 0, sizeof *e);
	e->base.link_mode_masks_nwords = 2;
}

/* the modes (ethtool bit numbers) set in mask `which`, as a list */
static void setmodes(struct elink *e, int which, const int *bits)
{
	for (; *bits >= 0; bits++)
		mode_set(e, which, *bits, true);
}

/* the advertised mask of e as the bit numbers it holds, for comparing: bit i of the result is mode bit i (bits < 64 only) */
static uint64_t advmask(const struct elink *e)
{
	uint64_t m = 0;

	for (int b = 0; b < 64; b++)
		if (mode_in(e, MASK_ADVERTISING, b))
			m |= 1ULL << b;
	return m;
}

#define B(x) (1ULL << ETHTOOL_LINK_MODE_##x##_BIT)
#define L(...) ((const int[]){ __VA_ARGS__, -1 })
#define H10 ETHTOOL_LINK_MODE_10baseT_Half_BIT
#define F10 ETHTOOL_LINK_MODE_10baseT_Full_BIT
#define H100 ETHTOOL_LINK_MODE_100baseT_Half_BIT
#define F100 ETHTOOL_LINK_MODE_100baseT_Full_BIT
#define H1G ETHTOOL_LINK_MODE_1000baseT_Half_BIT
#define F1G ETHTOOL_LINK_MODE_1000baseT_Full_BIT
#define F2G5 ETHTOOL_LINK_MODE_2500baseT_Full_BIT
#define F5G ETHTOOL_LINK_MODE_5000baseT_Full_BIT
#define F10G ETHTOOL_LINK_MODE_10000baseT_Full_BIT
#define X1G ETHTOOL_LINK_MODE_1000baseX_Full_BIT
#define X2G5 ETHTOOL_LINK_MODE_2500baseX_Full_BIT
#define SR10 ETHTOOL_LINK_MODE_10000baseSR_Full_BIT
#define LR10 ETHTOOL_LINK_MODE_10000baseLR_Full_BIT
#define CR10 ETHTOOL_LINK_MODE_10000baseCR_Full_BIT
#define KR10 ETHTOOL_LINK_MODE_10000baseKR_Full_BIT
#define FIBRE ETHTOOL_LINK_MODE_FIBRE_BIT
#define AN ETHTOOL_LINK_MODE_Autoneg_BIT
#define TP ETHTOOL_LINK_MODE_TP_BIT

int main(void)
{
	struct elink e;
	bool any;

	/* ---- mode_in / mode_set across both mask words ---- */
	blank(&e);
	mode_set(&e, MASK_ADVERTISING, F2G5, true);	/* bit 47: second word */
	check("bit 47 lands in word 1 of the advertising mask", 1u << 15, e.masks[2 + 1]);
	check("...and nowhere else", 0, e.masks[0] | e.masks[1] | e.masks[2] | e.masks[4] | e.masks[5]);
	check("mode_in sees it", 1, mode_in(&e, MASK_ADVERTISING, F2G5));
	check("mode_in: not in supported", 0, mode_in(&e, MASK_SUPPORTED, F2G5));
	mode_set(&e, MASK_ADVERTISING, F2G5, false);
	check("mode_set off clears it", 0, mode_in(&e, MASK_ADVERTISING, F2G5));
	check("a bit past the mask is never in", 0, mode_in(&e, MASK_ADVERTISING, 64));
	mode_set(&e, MASK_ADVERTISING, 64, true);
	check("a bit past the mask is not stored", 0, e.masks[0] | e.masks[1] | e.masks[2] | e.masks[3] | e.masks[4] | e.masks[5]);

	/* ---- adv_bits: the six bits of platform.h (2500F, 1000F, 100F, 100H, 10F, 10H) ---- */
	blank(&e);
	check("nothing advertised: 0", 0, adv_bits(&e));
	setmodes(&e, MASK_ADVERTISING, L(H10, F10, H100, F100, H1G, F1G, F2G5, F5G, F10G, AN, TP));
	check("everything advertised: 0x3f (1000 half, 5G and 10G have no bit)", 0x3f, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(H10));
	check("10 half: bit 0", 0x01, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F10));
	check("10 full: bit 1", 0x02, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(H100));
	check("100 half: bit 2", 0x04, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F100));
	check("100 full: bit 3", 0x08, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F1G));
	check("1000 full: bit 4", 0x10, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F2G5));
	check("2500 full: bit 5", 0x20, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(H1G, F5G, F10G));
	check("1000 half, 5G, 10G: no bit", 0, adv_bits(&e));
	blank(&e); setmodes(&e, MASK_SUPPORTED, L(F10, F100, F1G, F2G5));
	check("supported modes are not advertised ones", 0, adv_bits(&e));

	/* ---- speed_masks: the success path of `port N <speed>` ---- */
	blank(&e); setmodes(&e, MASK_SUPPORTED, L(H10, F10, H100, F100, F1G, F2G5, AN, TP));
	setmodes(&e, MASK_ADVERTISING, L(H10, F10, H100, F100, F1G, F2G5, AN, TP));
	check("100m full: ok", 1, speed_masks(&e, PHY_SPEED_100M, PHY_DUPLEX_FULL, &any));
	check("100m full: selects something", 1, any);
	check("100m full: advertises 100F only (Autoneg, TP kept)", B(100baseT_Full) | B(Autoneg) | B(TP), advmask(&e));
	check("100m full: bits", 0x08, adv_bits(&e));
	check("100m full: the supported mask is untouched", 1, mode_in(&e, MASK_SUPPORTED, F2G5));
	check("100m both duplexes: ok", 1, speed_masks(&e, PHY_SPEED_100M, PHY_DUPLEX_BOTH, &any));
	check("100m both: 100H and 100F", B(100baseT_Half) | B(100baseT_Full) | B(Autoneg) | B(TP), advmask(&e));
	check("10m half: ok", 1, speed_masks(&e, PHY_SPEED_10M, PHY_DUPLEX_HALF, &any));
	check("10m half: 10H only", B(10baseT_Half) | B(Autoneg) | B(TP), advmask(&e));
	check("1g (duplex both): 1000 full only, not 1000 half", 1, speed_masks(&e, PHY_SPEED_1G, PHY_DUPLEX_BOTH, &any));
	check("1g: bits", 0x10, adv_bits(&e));
	check("1g: mask", B(1000baseT_Full) | B(Autoneg) | B(TP), advmask(&e));
	check("2g5: ok", 1, speed_masks(&e, PHY_SPEED_2G5, PHY_DUPLEX_BOTH, &any));
	check("2g5: bits", 0x20, adv_bits(&e));
	check("auto: ok", 1, speed_masks(&e, PHY_SPEED_AUTO, PHY_DUPLEX_BOTH, &any));
	check("auto: every supported table mode", B(10baseT_Half) | B(10baseT_Full) | B(100baseT_Half) | B(100baseT_Full) |
	      B(1000baseT_Full) | B(2500baseT_Full) | B(Autoneg) | B(TP), advmask(&e));
	check("auto: selects something", 1, any);
	{
		uint64_t before = advmask(&e);

		check("10g on a port that reports its modes without 10G: refused", 0, speed_masks(&e, PHY_SPEED_10G, PHY_DUPLEX_BOTH, &any));
		check("10g refused: nothing changed", before, advmask(&e));
		check("1g half: no 1000 half mode: refused", 0, speed_masks(&e, PHY_SPEED_1G, PHY_DUPLEX_HALF, &any));
		check("5g, 2g5 half refused", 0, speed_masks(&e, PHY_SPEED_5G, PHY_DUPLEX_BOTH, &any) | speed_masks(&e, PHY_SPEED_2G5, PHY_DUPLEX_HALF, &any));
		/* 100m on a port that supports only 10M: refused */
		blank(&e); setmodes(&e, MASK_SUPPORTED, L(H10, F10));
		check("100m on a 10M-only port: refused", 0, speed_masks(&e, PHY_SPEED_100M, PHY_DUPLEX_BOTH, &any));
		/* a driver that reports no modes (veth): the request goes through to the kernel */
		blank(&e);
		check("1g where no modes are reported: passed to the driver", 1, speed_masks(&e, PHY_SPEED_1G, PHY_DUPLEX_BOTH, &any));
		check("...advertising 1000 full", B(1000baseT_Full), advmask(&e));
	}
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F1G, AN));
	check("2g5 half: no such mode, refused", 0, speed_masks(&e, PHY_SPEED_2G5, PHY_DUPLEX_HALF, &any));
	check("2g5 half: nothing changed", B(1000baseT_Full) | B(Autoneg), advmask(&e));
	blank(&e);	/* veth: no supported modes at all */
	check("veth auto: ok", 1, speed_masks(&e, PHY_SPEED_AUTO, PHY_DUPLEX_BOTH, &any));
	check("veth auto: nothing to select", 0, any);
	check("veth auto: nothing advertised", 0, advmask(&e));
	memset(&e, 0, sizeof e);	/* no mask words at all (settings unreadable) */
	check("no mask words, auto: ok, nothing", 1, speed_masks(&e, PHY_SPEED_AUTO, PHY_DUPLEX_BOTH, &any) && !any);

	/* ---- an SFP+ cage (the ZX510S has two): 1000Base-X and the 10G optical/DAC modes, no copper modes ---- */
	blank(&e);
	setmodes(&e, MASK_SUPPORTED, L(X1G, SR10, LR10, CR10, FIBRE));
	setmodes(&e, MASK_ADVERTISING, L(X1G, SR10, LR10, CR10, FIBRE));
	check("SFP+ advertising 1000X and 10G: the 1000 full bit", 0x10, adv_bits(&e));
	check("SFP+ 10g: ok, selects something", 1, speed_masks(&e, PHY_SPEED_10G, PHY_DUPLEX_BOTH, &any) && any);
	check("SFP+ 10g: the 10G modes of the port, 1000Base-X cleared, FIBRE kept", B(10000baseSR_Full) | B(10000baseLR_Full) | B(10000baseCR_Full) | B(FIBRE), advmask(&e));
	check("SFP+ 10g: no bit of the six", 0, adv_bits(&e));
	check("SFP+ 1g: ok", 1, speed_masks(&e, PHY_SPEED_1G, PHY_DUPLEX_BOTH, &any) && any);
	check("SFP+ 1g: 1000Base-X only, the 10G modes cleared", B(1000baseX_Full) | B(FIBRE), advmask(&e));
	check("SFP+ 1g: bit 4", 0x10, adv_bits(&e));
	check("SFP+ auto: everything the port supports", B(1000baseX_Full) | B(10000baseSR_Full) | B(10000baseLR_Full) | B(10000baseCR_Full) | B(FIBRE), (speed_masks(&e, PHY_SPEED_AUTO, PHY_DUPLEX_BOTH, &any), advmask(&e)));
	check("SFP+ 10g half: no such mode, refused", 0, speed_masks(&e, PHY_SPEED_10G, PHY_DUPLEX_HALF, &any));
	check("SFP+ 2g5: not supported, refused", 0, speed_masks(&e, PHY_SPEED_2G5, PHY_DUPLEX_BOTH, &any));
	check("SFP+ 100m: not supported, refused", 0, speed_masks(&e, PHY_SPEED_100M, PHY_DUPLEX_BOTH, &any));
	check("...nothing changed by the refusals", B(1000baseX_Full) | B(10000baseSR_Full) | B(10000baseLR_Full) | B(10000baseCR_Full) | B(FIBRE), advmask(&e));
	blank(&e); setmodes(&e, MASK_SUPPORTED, L(X2G5, X1G, KR10, FIBRE)); setmodes(&e, MASK_ADVERTISING, L(X2G5, X1G, KR10));
	check("2500Base-X port 2g5: ok", 1, speed_masks(&e, PHY_SPEED_2G5, PHY_DUPLEX_BOTH, &any) && any);
	check("...2500Base-X only", B(2500baseX_Full), advmask(&e));
	check("...bit 5", 0x20, adv_bits(&e));
	check("backplane KR port 10g", 1, speed_masks(&e, PHY_SPEED_10G, PHY_DUPLEX_BOTH, &any) && any);
	check("...10GBase-KR only", B(10000baseKR_Full), advmask(&e));

	/* ---- duplex_masks: the 8051's phy_set_duplex ---- */
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(H10, F10, H100, F100, F1G, F2G5, AN));
	check("duplex half: ok", 1, duplex_masks(&e, false));
	check("duplex half: 10H, 100H; 1G and 2.5G stay", B(10baseT_Half) | B(100baseT_Half) | B(1000baseT_Full) | B(2500baseT_Full) | B(Autoneg), advmask(&e));
	check("duplex full: ok", 1, duplex_masks(&e, true));
	check("duplex full: 10F, 100F; 1G and 2.5G stay", B(10baseT_Full) | B(100baseT_Full) | B(1000baseT_Full) | B(2500baseT_Full) | B(Autoneg), advmask(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F100, F1G));
	check("only 100F advertised, duplex half: 100H replaces it, no 10M mode appears", 1, duplex_masks(&e, false));
	check("...mask", B(100baseT_Half) | B(1000baseT_Full), advmask(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(H10));
	check("only 10H advertised, duplex full: 10F", 1, duplex_masks(&e, true));
	check("...mask", B(10baseT_Full), advmask(&e));
	blank(&e); setmodes(&e, MASK_ADVERTISING, L(F1G, F2G5, AN));
	check("no 10/100 mode advertised: refused", 0, duplex_masks(&e, false));
	check("...nothing changed", B(1000baseT_Full) | B(2500baseT_Full) | B(Autoneg), advmask(&e));
	blank(&e);
	check("veth (nothing advertised): refused", 0, duplex_masks(&e, true));

	/* ---- thermal scaling: millidegrees to 1/128 C ---- */
	check("0 mC", 0, thermal_raw(0));
	check("1000 mC = 1 C = 128", 128, thermal_raw(1000));
	check("45000 mC = 45 C = 5760", 5760, thermal_raw(45000));
	check("45500 mC = 5824", 5824, thermal_raw(45500));
	check("-5000 mC = -640", -640, thermal_raw(-5000));
	check("12345 mC = 1580 (truncated)", 1580, thermal_raw(12345));
	return fails != 0;
}
