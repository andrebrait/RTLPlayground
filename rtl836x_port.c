/*
 * The port interface of rtl837x_port.c for the RTL836x. Port status, MIB
 * counters, 802.1Q VLANs, isolation, mirroring, frame length, static LAGs and
 * the flush of one port's dynamic L2 entries are implemented; L2 learning stays
 * at hardware defaults, which forward between all ports and to the CPU port.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"
#include "rtl837x_port.h"
#include "machine.h"

#pragma codeseg BANK1
#pragma constseg BANK1

extern __xdata uint8_t sfr_data[4];
extern __xdata uint16_t vlan_ptr;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];


uint16_t port_link_mask(void) __banked
{
	uint16_t links = 0;

	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		reg_read(RTL836X_REG_PORT0_STATUS + i);
		if (SFR_DATA_0 & PORT_STATUS_LINK)
			links |= PORT_BIT(i);
	}
	return links;
}


void port_stats_print(void) __banked
{
	__xdata uint8_t value[8];
	print_string("\nPort\tState\tLink\tTX U/M/B\tTX excess\tRX U/M/B\tRX FCS\n");
	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		print_phys_port(i); write_char('\t');
		phy_read(i, 0, 0);
		print_string(SFR_DATA_8 & 0x08 ? "Off\t" : "On\t");	/* BMCR power down */
		reg_read(RTL836X_REG_PORT0_STATUS + i);
		if (!(SFR_DATA_0 & PORT_STATUS_LINK))
			print_string("Down\t");
		else if ((SFR_DATA_0 & PORT_STATUS_SPEED_MASK) == 2)
			print_string("1000M\t");
		else if (SFR_DATA_0 & PORT_STATUS_SPEED_MASK)
			print_string("100M\t");
		else
			print_string("10M\t");
		for (uint8_t metric = 0; metric < 4; metric++) {
			if (rtl836x_mib_summary(i, metric, value)) {
				print_string("0x");
				for (uint8_t b = 0; b < 8; b++)
					print_byte(value[b]);
			} else {
				print_string("n/a");
			}
			write_char(metric == 3 ? '\n' : '\t');
		}
	}
}


/*
 * VLANs as the RTL8367C SDK drives them; files are in the SDK, ports are chip ports:
 * - VLAN_CTRL 0x07a8 bit 0 turns VLANs and the egress filter on (rtl8367c_setAsicVlanFilter,
 *   rtl8367c_asicdrv_vlan.c:879, also _rtk_vlan_egrFilterEnable_set, vlan.c:372).
 * - 4K table: TABLE_ACCESS_ADDR 0x0501 takes the VID, TABLE_ACCESS_CTRL 0x0500 the command
 *   TB_TARGET_CVLAN (3) | TB_OP_WRITE (1) << 3 (rtl8367c_asicdrv.h:84) with the entry in
 *   TABLE_WRITE_DATA0-2 0x0510, or TB_OP_READ (0) and the entry in TABLE_READ_DATA0-2 0x0520
 *   once TABLE_LUT_ADDR 0x0502 bit 13 reads 0 (rtl8367c_setAsicVlan4kEntry and
 *   rtl8367c_getAsicVlan4kEntry, rtl8367c_asicdrv_vlan.c:215 and :289). Word 0: members 7:0,
 *   untagged set 15:8; word 1: FID 3:0 and IVL 14, here 0 like rtk_vlan_init's VLAN 1 (FID 0,
 *   shared learning); word 2: members 10:8 in 2:0, untagged set 10:8 in 5:3
 *   (_rtl8367c_Vlan4kStUser2Smi, rtl8367c_asicdrv_vlan.c:52). No members means no VLAN.
 * - PVID: VLAN_PVID_CTRL0 0x0700 + port / 2, bits 4:0 for even and 12:8 for odd ports, selects
 *   one of 32 member configurations at 0x0728 + 4 * index: word 0 members, word 1 FID, word 3
 *   the VID (rtl8367c_setAsicVlanPortBasedVID and _rtl8367c_VlanMCStUser2Smi,
 *   rtl8367c_asicdrv_vlan.c:544 and :27). rtk_vlan_portPvid_set (vlan.c:509) takes the one with
 *   that VID or fills a free one from the 4K entry (rtk_vlan_checkAndCreateMbr, vlan.c:2185),
 *   and rtk_vlan_set keeps their members in step with the 4K entry (vlan.c:193).
 * - Ingress filter: VLAN_INGRESS 0x07a9, a bit per port (rtl8367c_asicdrv_vlan.c:438).
 * - Accepted frames: VLAN_ACCEPT_FRAME_TYPE_CTRL0 0x07aa + port / 8, 2 bits per port: 0 all,
 *   1 tagged, 2 untagged, as vlan_ingress_mode_e (rtl8367c_asicdrv_vlan.c:381).
 * - Egress tag mode: PORT0_MISC_CFG 0x000e + 32 * port, bits 5:4, 0 follows the untagged set
 *   (rtl8367c_setAsicVlanEgressTagMode, rtl8367c_asicdrv_vlan.c:485).
 * The NIC is chip port 10. VLAN 1 starts untagged on the CPU port; configured
 * VLANs tag CPU traffic so the RX adapter can retain the ingress VID.
 */
#define CHIP_PORT(p)	((p) == CPU_PORT ? RTL836X_CPU_PORT : (p))
#define CHIP_MASK(m)	(((m) & 0xff) | (((m) & PMASK_CPU) << 1))
#define LOG_MASK(m)	(((m) & 0xff) | (((m) >> 1) & PMASK_CPU))
#define VLAN1_MEMBERS	((PORT_BIT(MACHINE_MAX_PORT + 1) - PORT_BIT(MACHINE_MIN_PORT)) | PMASK_CPU)
#define MC_REG(i, word)	(RTL836X_REG_VLAN_MEMBER_CONFIGURATION0 + 4 * (i) + (word))

static __xdata uint16_t vlan4k_members, vlan4k_untag;	/* chip port masks of the last 4K entry read */


void reg_bits(uint16_t reg, __xdata uint16_t mask, __xdata uint16_t val) __banked
{
	uint16_t m = mask;

	while (!(m & 1)) {
		m >>= 1;
		val <<= 1;
	}
	reg_read(reg);
	val = (SFR_DATA_U16 & ~mask) | (val & mask);
	REG_SET(reg, val);
}


static void table_wait(void)
{
	__xdata uint8_t guard = 0;

	do {
		reg_read(RTL836X_REG_TABLE_LUT_ADDR);
	} while ((SFR_DATA_U16 & TABLE_LUT_ADDR_BUSY) && ++guard);
}


static void vlan4k_read(__xdata uint16_t vid)
{
	table_wait();
	REG_SET(RTL836X_REG_TABLE_ACCESS_ADDR, vid);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, TABLE_ACCESS_CVLAN_READ);
	table_wait();
	reg_read(RTL836X_REG_TABLE_READ_DATA0 + 2);
	vlan4k_members = (SFR_DATA_U16 & 0x07) << 8;
	vlan4k_untag = (SFR_DATA_U16 & 0x38) << 5;
	reg_read(RTL836X_REG_TABLE_READ_DATA0);
	vlan4k_members |= SFR_DATA_U16 & 0xff;
	vlan4k_untag |= SFR_DATA_U16 >> 8;
}


static uint16_t mc_vid(__xdata uint8_t i)
{
	reg_read(MC_REG(i, 3));
	return SFR_DATA_U16 & 0x1fff;
}


/* Logical port masks; the member configurations of vid follow the new member set */
static void vlan_write(__xdata uint16_t vid, __xdata uint16_t members, __xdata uint16_t untag)
{
	__xdata uint16_t word = (members & 0xff) | (untag << 8);
	__xdata uint8_t i;

	table_wait();
	REG_SET(RTL836X_REG_TABLE_WRITE_DATA0, word);
	REG_SET(RTL836X_REG_TABLE_WRITE_DATA0 + 1, 0);
	word = ((members & PMASK_CPU) >> 7) | ((untag & PMASK_CPU) >> 4);	/* chip port 10 */
	REG_SET(RTL836X_REG_TABLE_WRITE_DATA0 + 2, word);
	REG_SET(RTL836X_REG_TABLE_ACCESS_ADDR, vid);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, TABLE_ACCESS_CVLAN_WRITE);
	word = CHIP_MASK(members);
	for (i = 0; i < RTL836X_VLAN_MC_ENTRIES; i++) {
		if (mc_vid(i) == vid)
			REG_SET(MC_REG(i, 0), word);
	}
}


/* Whether a port selects member configuration idx; CTRL5 has port 10 alone */
static bool mc_selected(__xdata uint8_t idx)
{
	__xdata uint8_t r;

	for (r = 0; r <= RTL836X_CPU_PORT / 2; r++) {
		reg_read(RTL836X_REG_VLAN_PVID_CTRL0 + r);
		if ((SFR_DATA_0 & 0x1f) == idx || (r < RTL836X_CPU_PORT / 2 && (SFR_DATA_8 & 0x1f) == idx))
			return true;
	}
	return false;
}


/*
 * The member configuration for vid. Unlike the SDK, which tracks the ones it filled,
 * a configuration is free when no port selects it.
 */
static uint8_t mc_index(__xdata uint16_t vid)
{
	__xdata uint8_t i;

	for (i = 0; i < RTL836X_VLAN_MC_ENTRIES; i++) {
		if (mc_vid(i) == vid)
			return i;
	}
	/* 11 ports select at most 11 of the 32 configurations */
	for (i = 0; mc_selected(i); i++)
		;
	vlan4k_read(vid);
	REG_SET(MC_REG(i, 0), vlan4k_members);
	REG_SET(MC_REG(i, 1), 0);
	REG_SET(MC_REG(i, 2), 0);
	REG_SET(MC_REG(i, 3), vid);
	return i;
}


/* A port in a LAG gives its PVID to every member, as on the RTL837x */
void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked
{
	/* simple steps in xdata: wider expressions spill to internal RAM, which is full */
	__xdata uint8_t i, chip, lag = port_lag_of(port), idx = mc_index(pvid);
	__xdata uint16_t members = PORT_BIT(port);

	if (lag != PORT_LAG_NONE)
		members = port_lag_members_get(lag);
	for (i = 0; i <= CPU_PORT; i++, members >>= 1) {
		if (!(members & 1))
			continue;
		chip = CHIP_PORT(i);
		reg_bits(RTL836X_REG_VLAN_PVID_CTRL0 + (chip >> 1), chip & 1 ? 0x1f00 : 0x001f, idx);
	}
}


uint16_t port_pvid_get(uint8_t port) __banked
{
	__xdata uint8_t chip = CHIP_PORT(port);

	reg_read(RTL836X_REG_VLAN_PVID_CTRL0 + (chip >> 1));
	return mc_vid((chip & 1 ? SFR_DATA_8 : SFR_DATA_0) & 0x1f);
}


/* sfr_data gets the entry as the RTL837x has it: bit 25 valid, members in 9:0 and the
 * untagged set in 19:10, the CPU port as bit 9. Valid here means having members. */
int8_t vlan_get(uint16_t vlan) __banked
{
	__xdata uint16_t members, untag;

	if (vlan >= 0xfff)
		return -1;
	vlan4k_read(vlan);
	members = LOG_MASK(vlan4k_members);
	untag = LOG_MASK(vlan4k_untag);
	sfr_data[0] = members ? 0x02 : 0;
	sfr_data[1] = untag >> 6;
	sfr_data[2] = (untag << 2) | (members >> 8);
	sfr_data[3] = members;
	return 0;
}


/* The arguments are in vlan_settings; a member that is not tagged is untagged */
void vlan_create(void) __banked
{
	if (!vlan_settings.vlan || vlan_settings.vlan >= 0xfff) {
		print_string("\nInvalid VLAN: "); print_short(vlan_settings.vlan); write_char('\n');
		return;
	}
	/* Preserve the VLAN identity on the CPU link, independently of front-port tagging. */
	vlan_settings.members |= PMASK_CPU;
	vlan_settings.tagged = (vlan_settings.tagged & vlan_settings.members) | PMASK_CPU;

	print_string("vlan: "); print_short(vlan_settings.vlan);
	print_string(", members: "); print_short(vlan_settings.members);
	print_string(", tagged: "); print_short(vlan_settings.tagged); write_char('\n');

	vlan_write(vlan_settings.vlan, vlan_settings.members, vlan_settings.members & ~vlan_settings.tagged);
}


/* Ports keep the VID as PVID, as on the RTL837x, and drop its untagged frames */
void vlan_delete(uint16_t vlan) __banked
{
	if (!vlan || vlan >= 0xfff)
		return;

	vlan_name_remove(vlan);
	vlan_write(vlan, 0, 0);
}


bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked
{
	if ((!MACHINE_PORT_VALID(port) && port != CPU_PORT) || type >= VLAN_INVALID) {
		print_string("Invalid port or ingress filter type\n");
		return false;
	}
	port = CHIP_PORT(port);
	reg_bits(RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0 + (port >> 3), 3 << ((port & 7) << 1), type);
	return true;
}


vlan_ingress_mode_t port_ingress_filter_get(__xdata uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port) && port != CPU_PORT)
		return VLAN_INVALID;
	port = CHIP_PORT(port);
	reg_read(RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0 + (port >> 3));
	return (SFR_DATA_U16 >> ((port & 7) << 1)) & 0x3;
}


bool port_ingress_vlan_filter_set(uint8_t port, __xdata bool enabled) __banked
{
	if (!MACHINE_PORT_VALID(port) && port != CPU_PORT)
		return false;
	if (enabled)
		reg_bit_set(RTL836X_REG_VLAN_INGRESS, CHIP_PORT(port));
	else
		reg_bit_clear(RTL836X_REG_VLAN_INGRESS, CHIP_PORT(port));
	return true;
}


bool port_ingress_vlan_filter_get(uint8_t port) __banked
{
	if (!MACHINE_PORT_VALID(port) && port != CPU_PORT)
		return false;
	return reg_bit_test(RTL836X_REG_VLAN_INGRESS, CHIP_PORT(port));
}


/*
 * rtk_vlan_init (vlan.c:40): VLAN 1 with every front port and the CPU port untagged, PVID 1,
 * egress tag mode original and ingress filter on everywhere, then VLANs on. All frame
 * types are accepted, as on the RTL837x.
 */
void vlan_setup(void) __banked
{
	__xdata uint8_t i;

	vlan_ptr = 0;
	vlan_names[0] = 0;

	for (i = 0; i < RTL836X_VLAN_MC_ENTRIES * 4; i++)
		REG_SET(RTL836X_REG_VLAN_MEMBER_CONFIGURATION0 + i, 0);
	REG_SET(MC_REG(0, 3), 1);	/* configuration 0 is VLAN 1, vlan_write gives it the members */
	vlan_write(1, VLAN1_MEMBERS, VLAN1_MEMBERS);
	for (i = 0; i <= RTL836X_CPU_PORT / 2; i++)
		REG_SET(RTL836X_REG_VLAN_PVID_CTRL0 + i, 0);
	for (i = MACHINE_MIN_PORT; i <= CPU_PORT; i++) {
		reg_bits(RTL836X_REG_PORT0_MISC_CFG + (CHIP_PORT(i) << 5), PORT_MISC_CFG_VLAN_EGRESS_MODE, 0);
		port_ingress_vlan_filter_set(i, true);
	}
	REG_SET(RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0, 0);
	REG_SET(RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0 + 1, 0);
	reg_bits(RTL836X_REG_VLAN_CTRL, VLAN_CTRL_ENABLE, 1);
}


/* PORT_ISOLATION_PORTn_MASK 0x08a2 + chip port: where a frame entering port may leave,
 * all 11 chip ports at reset (rtl8367c_setAsicPortIsolationPermittedPortmask,
 * rtl8367c_asicdrv_portIsolation.c:44) */
void port_isolate(uint8_t port, __xdata uint16_t pmask) __banked
{
	if (port <= MACHINE_MAX_PORT)
		REG_SET(RTL836X_REG_PORT_ISOLATION_PORT0_MASK + port, CHIP_MASK(pmask));
}


uint16_t port_isolation_get(uint8_t port) __banked
{
	if (port > MACHINE_MAX_PORT)
		return 0;
	reg_read(RTL836X_REG_PORT_ISOLATION_PORT0_MASK + port);
	return LOG_MASK(SFR_DATA_U16);
}


/*
 * Mirroring (rtk_mirror_portBased_set, mirror.c:46): MIRROR_CTRL 0x121c takes the first source port
 * (3:0), the monitor port (7:4) and the RX (9) and TX (10) enables (rtl8367c_setAsicPortMirror,
 * rtl8367c_asicdrv_mirror.c:34, ...RxFunction :87, ...TxFunction :123); MIRROR_SRC_PMSK 0x12fb holds
 * the one source set both directions share (rtl8367c_setAsicPortMirrorMask, :197). So RX and TX
 * masks must be equal or one of them empty, and the monitor port is no source (mirror.c:70-75).
 */
bool port_mirror_set(__xdata uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked
{
	/* simple steps in xdata: wider expressions spill to internal RAM, which is full */
	__xdata uint16_t src_mask, ctrl;
	__xdata uint8_t first = 0;

	src_mask = rx_pmask | tx_pmask;
	if (!src_mask || port > MACHINE_MAX_PORT || (src_mask & PORT_BIT(port)))
		return false;
	if (rx_pmask && tx_pmask && rx_pmask != tx_pmask)
		return false;
	ctrl = CHIP_MASK(src_mask);
	reg_bits(RTL836X_REG_MIRROR_SRC_PMSK, 0x07ff, ctrl);
	while (!(ctrl & 1)) {
		ctrl >>= 1;
		first++;
	}
	ctrl = port;
	ctrl <<= 4;
	ctrl |= first;
	if (rx_pmask)
		ctrl |= 0x0200;
	if (tx_pmask)
		ctrl |= 0x0400;
	reg_bits(RTL836X_REG_MIRROR_CTRL, 0x00ff | MIRROR_CTRL_RX_TX, ctrl);
	return true;
}


void port_mirror_get(__xdata struct port_mirror * __xdata m) __banked
{
	__xdata uint16_t ctrl, src_mask;

	reg_read(RTL836X_REG_MIRROR_CTRL);
	ctrl = SFR_DATA_U16;
	reg_read(RTL836X_REG_MIRROR_SRC_PMSK);
	src_mask = SFR_DATA_U16 & 0x07ff;
	src_mask = LOG_MASK(src_mask);
	m->port = (ctrl >> 4) & 0xf;
	m->enabled = 0;
	m->rx_pmask = 0;
	m->tx_pmask = 0;
	if (ctrl & MIRROR_CTRL_RX_TX)
		m->enabled = 1;
	if (ctrl & 0x0200)
		m->rx_pmask = src_mask;
	if (ctrl & 0x0400)
		m->tx_pmask = src_mask;
}


void port_mirror_del(void) __banked
{
	reg_bits(RTL836X_REG_MIRROR_CTRL, MIRROR_CTRL_RX_TX, 0);
}


/*
 * Frame length: MAX_LENGTH_CFG 0x0883 bit port picks MAX_LEN_CFG0 or CFG1 (0x088c + cfg) at
 * 10/100M, bit 8 + port at 1G (rtl8367c_setAsicMaxLength, rtl8367c_asicdrv_misc.c:214).
 * A port gets the same configuration at both speeds, so the switch holds two lengths at most.
 */
uint16_t port_mtu_get(uint8_t port) __banked
{
	reg_read(RTL836X_REG_MAX_LENGTH_CFG);
	reg_read(RTL836X_REG_MAX_LEN_CFG0 + ((SFR_DATA_U16 >> port) & 1));
	return SFR_DATA_U16 & 0x3fff;
}


/* Takes a configuration holding len, else one no other port uses; false if both are taken */
bool port_mtu_set(uint8_t port, __xdata uint16_t len) __banked
{
	/* simple 8-bit steps in xdata: wider expressions spill to internal RAM, which is full */
	__xdata uint8_t fe, ge, on0, on1, cfg, mask = PORT_BIT(port);

	reg_read(RTL836X_REG_MAX_LENGTH_CFG);
	fe = SFR_DATA_0;		/* CFG1 select per port at 10/100M */
	ge = SFR_DATA_8;		/* and at 1G */
	for (cfg = 0; cfg < 2; cfg++) {
		reg_read(RTL836X_REG_MAX_LEN_CFG0 + cfg);
		if ((SFR_DATA_U16 & 0x3fff) == len)
			goto select;
	}
	on0 = ~(fe & ge);		/* ports on CFG0 at either speed */
	on0 &= ~mask;
	on1 = fe | ge;
	on1 &= ~mask;
	if (!on0)
		cfg = 0;
	else if (!on1)
		cfg = 1;
	else
		return false;
	REG_SET(RTL836X_REG_MAX_LEN_CFG0 + cfg, len);
select:
	if (cfg) {
		fe |= mask;
		ge |= mask;
	} else {
		fe &= ~mask;
		ge &= ~mask;
	}
	len = ((uint16_t)ge << 8) | fe;
	REG_SET(RTL836X_REG_MAX_LENGTH_CFG, len);
	return true;
}


void port_l2_setup(void) __banked
{
}


void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked
{
	(void)mac;
	(void)vlan;
	(void)remove_entry;
}


/* Busy flags of a flush, polled as table_wait polls. shortcut: 256 polls, not timed on silicon; raise it if a flush reports "did not complete" */
static bool flush_idle(__xdata uint16_t busy_low, __xdata uint16_t busy_ext)
{
	__xdata uint8_t polls = 0;
	__xdata uint8_t busy;

	do {
		reg_read(RTL836X_REG_FORCE_FLUSH);
		busy = 0;
		if (SFR_DATA_U16 & busy_low)
			busy = 1;
		reg_read(RTL836X_REG_FORCE_FLUSH1);
		if (SFR_DATA_U16 & busy_ext)
			busy = 1;
	} while (busy && ++polls);
	return !busy;
}


/*
 * Drops one port's dynamic unicast entries as rtl8365mb_l2_flush (Linux) and rtl8367c_setAsicLutForceFlush
 * (SDK, rtl8367c_asicdrv_lut.c) do: CTRL2 mode 0 (port) and type 0 (dynamic only); CTRL1 0, its VID and FID
 * do not apply in port mode; then the port's bit in FORCE_FLUSH (chip ports 0-7) or FORCE_FLUSH1 (8-10).
 * Nothing is written while any flush is busy, or for a logical port without a chip port.
 */
void port_l2_forget_port(uint8_t port) __banked
{
	__xdata uint8_t chip;
	__xdata uint16_t busy_low = 0, busy_ext = 0;

	if (!MACHINE_PORT_VALID(port) && port != CPU_PORT)
		return;
	chip = CHIP_PORT(port);
	if (!flush_idle(FORCE_FLUSH_BUSY_MASK, FORCE_FLUSH1_BUSY_MASK)) {
		print_string("L2 flush busy, port not flushed\n");
		return;
	}
	reg_bits(RTL836X_REG_L2_FLUSH_CTRL2, LUT_FLUSH_MODE_MASK | LUT_FLUSH_TYPE_MASK, 0);
	REG_SET(RTL836X_REG_L2_FLUSH_CTRL1, 0);
	if (chip < 8) {
		REG_SET(RTL836X_REG_FORCE_FLUSH, PORT_BIT(chip));
		busy_low = PORT_BIT(chip) << 8;
	} else {
		REG_SET(RTL836X_REG_FORCE_FLUSH1, PORT_BIT(chip) >> 8);
		busy_ext = PORT_BIT(chip) >> 5;
	}
	if (!flush_idle(busy_low, busy_ext))
		print_string("L2 flush did not complete\n");
}


/*
 * Static LAG. The RTL8370N has two groups, fixed to chip ports 0-3 and 4-7: GROUP_MASK 0x08d0 holds nibble n
 * for group n with bit k for chip port 4 n + k (rtl8367c_setAsicTrunkingGroup, rtl8367c_asicdrv_trunking.c:119;
 * rtk_trunk_port_set, trunk.c:45, refuses ports outside the group's nibble, :96). So its low byte is the chip
 * port mask and logical front ports are chip ports. The hash fields are one set for the whole switch, TRUNK_CTRL
 * 0x08cf bits 6:0 in the order of LAG_HASH_* (rtl8367c_setAsicTrunkingHashSelect, :203; rtk_trunk_distributionAlgorithm_set,
 * trunk.c:189, takes only the whole system and refuses 128 and up). The 16-entry hash table (0x08d2) shared by
 * both groups stays at its reset value, as in the vendor firmware, which never writes it.
 */
uint16_t port_lag_members_get(uint8_t lag) __banked
{
	__xdata uint16_t mask = 0x000f;

	if (lag >= PORT_LAG_COUNT)
		return 0;
	if (lag)
		mask <<= 4;
	reg_read(RTL836X_REG_PORT_TRUNK_GROUP_MASK);
	return SFR_DATA_U16 & mask;
}


uint8_t port_lag_of(uint8_t port) __banked
{
	if (port > MACHINE_MAX_PORT)
		return PORT_LAG_NONE;
	reg_read(RTL836X_REG_PORT_TRUNK_GROUP_MASK);
	if (!((SFR_DATA_U16 >> port) & 1))
		return PORT_LAG_NONE;
	return port >> 2;
}


/* A group the chip has, taking only ports of its own bank: the CPU port and the other group's ports are not */
bool port_lag_members_valid(__xdata uint8_t lag, __xdata uint16_t members) __banked
{
	__xdata uint16_t mask = 0x000f;

	if (lag >= PORT_LAG_COUNT)
		return false;
	if (lag)
		mask <<= 4;
	return (members & mask) == members;
}


/*
 * Refuses, writing nothing, what port_lag_members_valid() refuses.
 * A hash selector of 0 would send every flow to one member, so a non-empty group on a zero selector gets
 * all seven fields, the vendor firmware's mask (0x7f); a selector already set is kept, as are the mode and flood bits.
 */
bool port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked
{
	__xdata uint16_t mask = 0x000f;
	__xdata uint8_t shift = 0;

	if (!port_lag_members_valid(lag, members))
		return false;
	if (lag) {
		mask <<= 4;
		shift = 4;
	}
	members >>= shift;
	if (members) {
		reg_read(RTL836X_REG_PORT_TRUNK_CTRL);
		if (!(SFR_DATA_0 & PORT_TRUNK_CTRL_HASH))
			reg_bits(RTL836X_REG_PORT_TRUNK_CTRL, PORT_TRUNK_CTRL_HASH, PORT_TRUNK_CTRL_HASH);
	}
	reg_bits(RTL836X_REG_PORT_TRUNK_GROUP_MASK, mask, members);
	return true;
}


/* The group is accepted for the RTL837x commands; the hash is the same for both */
bool port_lag_hash_set(__xdata uint8_t lag, __xdata uint8_t hash) __banked
{
	if (lag >= PORT_LAG_COUNT || hash > PORT_TRUNK_CTRL_HASH)
		return false;
	reg_bits(RTL836X_REG_PORT_TRUNK_CTRL, PORT_TRUNK_CTRL_HASH, hash);
	return true;
}


uint8_t port_lag_hash_get(__xdata uint8_t lag) __banked
{
	(void)lag;
	reg_read(RTL836X_REG_PORT_TRUNK_CTRL);
	return SFR_DATA_0 & PORT_TRUNK_CTRL_HASH;
}
