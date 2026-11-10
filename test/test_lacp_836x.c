/*
 * test_lacp_836x.c - LACP on the RTL8370N's two fixed groups. rtl837x_lacp.c,
 * rtl836x_port.c and rtl837x_trap.c are compiled unmodified for the RTL836x; only
 * the register controller (a 16-bit register file), the NIC's TX edge and the
 * console are modelled. A simulated 802.3ad partner speaks to the real state
 * machines, and what a group holds is read back from the chip's own registers:
 * the trunk membership nibbles in PORT_TRUNK_GROUP_MASK and the action of the
 * slow-protocols address in RMA_CTRL02.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "machine.h"
#include "uip.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl836x_mib.h"
#include "rtl837x_port.h"
#include "rtl837x_lacp.h"
#include "rtl837x_trap.h"

#if SOC_FAMILY != RTL836X
#error "this test is for the RTL836x build"
#endif
_Static_assert(PORT_LAG_COUNT == 2, "the RTL8370N has two fixed groups");

/* ---------- platform mocks ---------- */

uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
struct uip_eth_addr uip_ethaddr = { .addr = {0x02,0x11,0x22,0x33,0x44,0x55} };
const struct machine machine = {
	.phys_to_log_port = {0, 1, 2, 3, 4, 5, 6, 7, 0xff},
	.log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 0}
};
struct machine_runtime machine_detected;
uint8_t lacpEnabled;
bool lldp_enabled;
uint16_t management_vlan = 1;	/* the slow-protocol path zeroes it per frame */
volatile uint32_t ticks;
uint8_t sfr_data[4];
volatile uint8_t SFR_DATA_0, SFR_DATA_8, SFR_DATA_16, SFR_DATA_24;
volatile uint16_t SFR_DATA_U16;
uint16_t vlan_ptr;
uint8_t vlan_names[VLAN_NAMES_SIZE];

void print_string(const char *s) { (void)s; }
void print_byte(uint8_t b) { (void)b; }
void print_short(uint16_t v) { (void)v; }
void write_char(char c) { (void)c; }
void print_phys_port(uint8_t port) { (void)port; }
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg) { (void)phy_id; (void)dev_id; (void)reg; }
bool rtl836x_mib_summary(uint8_t port, uint8_t metric, uint8_t *value) { (void)port; (void)metric; (void)value; return false; }
void vlan_name_remove(uint16_t vlan) { (void)vlan; }

/* The register controller: 16-bit registers, the value in a6:a7 on reads and a8:a9 order on writes as the RTL8370N takes it */
static uint16_t regfile[0x4000];
static unsigned reg_writes;

void reg_read(uint16_t addr)
{
	SFR_DATA_U16 = regfile[addr];
	SFR_DATA_8 = regfile[addr] >> 8;
	SFR_DATA_0 = regfile[addr];
}

void reg_write(uint16_t addr)
{
	regfile[addr] = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
	reg_writes++;
}

void reg_bit_set(uint16_t addr, char bit) { regfile[addr] |= 1u << bit; reg_writes++; }
void reg_bit_clear(uint16_t addr, char bit) { regfile[addr] &= ~(1u << bit); reg_writes++; }
uint8_t reg_bit_test(uint16_t addr, char bit) { return (regfile[addr] >> bit) & 1; }

#define GROUP_MASK	regfile[RTL836X_REG_PORT_TRUNK_GROUP_MASK]
#define TRUNK_CTRL	regfile[RTL836X_REG_PORT_TRUNK_CTRL]
#define RMA_LACP_ACT	((regfile[RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP] & RMA_OPERATION_MASK) >> 7)

/* ---------- on-wire mirrors (independent re-statement of the layout) ---------- */

struct wire_info {
	uint16_t sys_prio;
	uint8_t  sys[MAC_ADDR_LEN];
	uint16_t key;
	uint16_t port_prio;
	uint16_t port;
	uint8_t  state;
	uint8_t  reserved[3];
};

struct wire_pdu_out {		/* what lacp_send() hands to tcpip_output(), from uip_buf + 12 */
	uint8_t  dst[MAC_ADDR_LEN];
	uint8_t  src[MAC_ADDR_LEN];
	struct rtl_tag rtl_tag;
	uint16_t ethertype;
	uint8_t  subtype;
	uint8_t  version;
	uint8_t  tlv_actor, actor_len;
	struct wire_info actor;
	uint8_t  tlv_partner, partner_len;
	struct wire_info partner;
	uint8_t  tlv_collector, collector_len;
	uint16_t collector_max_delay;
	uint8_t  collector_reserved[12];
	uint8_t  tlv_terminator, terminator_len;
	uint8_t  terminator_reserved[50];
};

/* What rtl836x_nic.c's nic_rx_packet() hands the dispatcher: the ingress port in the low byte of the tag's
 * pmask and a VID 0 802.1Q tag behind it, the same layout the RTL837x gives a forwarded frame */
struct wire_pdu_in {
	uint8_t  dst[MAC_ADDR_LEN];
	uint8_t  src[MAC_ADDR_LEN];
	struct rtl_tag rtl_tag;
	struct vlan_tag vlan_tag;
	uint16_t ethertype;
	uint8_t  subtype;
	uint8_t  version;
	uint8_t  tlv_actor, actor_len;
	struct wire_info actor;
	uint8_t  tlv_partner, partner_len;
	struct wire_info partner;
};

_Static_assert(sizeof(struct wire_pdu_out) == 132, "TX pdu must be 132");

/* ---------- TX capture ---------- */

static struct wire_pdu_out last_tx[10];
static int tx_count[10];
static uint16_t mgmt_vlan_at_tx = 0xffff;

void tcpip_output(void)
{
	struct wire_pdu_out *p = (struct wire_pdu_out *)&uip_buf[RTL_FRAME_DESC_SIZE];
	uint16_t pmask = HTONS(p->rtl_tag.pmask);

	mgmt_vlan_at_tx = management_vlan;
	for (int port = 0; port < 10; port++) {
		if (pmask & (1u << port)) {
			last_tx[port] = *p;
			tx_count[port]++;
		}
	}
	uip_len = 0;
}

/* ---------- simulated partner ---------- */

static void partner_frame(uint8_t port, const uint8_t sys[MAC_ADDR_LEN], uint8_t pstate, int echo)
{
	struct wire_pdu_in in;

	memset(&in, 0, sizeof(in));
	in.dst[0] = 0x01; in.dst[1] = 0x80; in.dst[2] = 0xc2; in.dst[5] = 0x02;
	memcpy(in.src, sys, MAC_ADDR_LEN);
	in.rtl_tag.tag = HTONS(RTL_FRAME_TAG_ID);
	in.rtl_tag.version = RTL_FRAME_TAG_VERSION;
	in.rtl_tag.pmask = HTONS(port);
	in.vlan_tag.svlan = HTONS(0x8100);
	in.ethertype = HTONS(0x8809);
	in.subtype = 0x01;
	in.version = 0x01;

	in.tlv_actor = 0x01; in.actor_len = 0x14;
	in.actor.sys_prio = HTONS(0x8000);
	memcpy(in.actor.sys, sys, MAC_ADDR_LEN);
	in.actor.key = HTONS(0x0011);
	in.actor.port_prio = HTONS(0x00ff);
	in.actor.port = HTONS((uint16_t)port + 101);
	in.actor.state = pstate;

	in.tlv_partner = 0x02; in.partner_len = 0x14;
	if (echo) {			/* a fresh view of us, from our last TX */
		in.partner.sys_prio = last_tx[port].actor.sys_prio;
		memcpy(in.partner.sys, last_tx[port].actor.sys, MAC_ADDR_LEN);
		in.partner.key = last_tx[port].actor.key;
		in.partner.port = last_tx[port].actor.port;
		in.partner.state = last_tx[port].actor.state;
	}

	memcpy(uip_buf, &in, sizeof(in));
	uip_len = sizeof(in);
	lacp_in();
}

/* Advance time by n system ticks; the LACP timers step every fourth one */
static void step(int n) { while (n--) { ticks++; lacp_timers(); } }

/* ---------- scenario runner ---------- */

static int failures;
#define CHECK(cond, name) do { \
	if (cond) printf("PASS  %s\n", name); \
	else { printf("FAIL  %s\n", name); failures++; } \
} while (0)

#define P_ACT   LACP_STATE_ACTIVITY
#define P_AGG   LACP_STATE_AGGREGATION
#define P_SYNC  LACP_STATE_SYNC
#define P_COL   LACP_STATE_COLLECTING
#define P_DIST  LACP_STATE_DISTRIBUTING
#define P_TO    LACP_STATE_TIMEOUT

static const uint8_t SYS_A[MAC_ADDR_LEN] = {0x02,0xaa,0xaa,0xaa,0xaa,0x01};
static const uint8_t SYS_B[MAC_ADDR_LEN] = {0x02,0xbb,0xbb,0xbb,0xbb,0x02};
#define IN_SYNC	(P_ACT | P_AGG | P_TO | P_SYNC)

/* Everything LACP and the chip hold about the two groups, to show a refused request touched none of it */
struct snapshot {
	uint16_t lag_ports[PORT_LAG_COUNT], members_last[PORT_LAG_COUNT];
	uint8_t agg_valid[PORT_LAG_COUNT], port_lag[10], actor[10], rx_state[10];
	uint16_t group_mask, trunk_ctrl;
	uint8_t rma_act, enabled;
	unsigned writes;
};

static void take(struct snapshot *s)
{
	memset(s, 0, sizeof(*s));	/* padding takes part in the comparison */
	memcpy(s->lag_ports, lacp_lag_ports, sizeof(s->lag_ports));
	memcpy(s->members_last, lacp_members_last, sizeof(s->members_last));
	memcpy(s->agg_valid, lacp_agg_valid, sizeof(s->agg_valid));
	memcpy(s->port_lag, lacp_port_lag, sizeof(s->port_lag));
	memcpy(s->actor, lacp_actor_state, sizeof(s->actor));
	memcpy(s->rx_state, lacp_rx_state, sizeof(s->rx_state));
	s->group_mask = GROUP_MASK;
	s->trunk_ctrl = TRUNK_CTRL;
	s->rma_act = RMA_LACP_ACT;
	s->enabled = lacpEnabled;
	s->writes = reg_writes;
}

static int same(const struct snapshot *a)
{
	struct snapshot b;

	take(&b);
	return !memcmp(a, &b, sizeof(b));
}

/* Both partners answer, twice, so the echoes carry our fresh state */
static void converge(uint8_t first, uint8_t last, const uint8_t sys[MAC_ADDR_LEN])
{
	for (int round = 0; round < 3; round++) {
		for (uint8_t p = first; p <= last; p++)
			partner_frame(p, sys, IN_SYNC, 1);
		step(8);
	}
}

int main(void)
{
	struct snapshot before;

	/* Boot order of the firmware: trap_init() resets the actions, then the saved config replays */
	regfile[RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP] = 0x7465;	/* unrelated bits that must survive */
	GROUP_MASK = 0x0300;		/* chip ports 8 and 9 are not ours */
	TRUNK_CTRL = 0x0100;		/* reset: dumb mode on, no hash field */
	for (uint8_t p = MACHINE_MIN_PORT; p <= MACHINE_MAX_PORT; p++)
		regfile[RTL836X_REG_PORT0_STATUS + p] = PORT_STATUS_LINK;	/* carrier on every port */
	CHECK(port_link_mask() == 0x00ff, "T0a the link mask follows the PORTn_STATUS link bits");
	trap_init();
	lacp_init();

	CHECK(RMA_LACP_ACT == TRAP_ACT_FORWARD && regfile[RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP] == 0x7465
	      && !lacpEnabled, "T0 boot: the slow-protocols address is forwarded and LACP is off");

	/* T1: a fixed-bank candidate set starts negotiation, the candidates are not members yet */
	CHECK(port_lag_members_valid(0, 0x0003) && port_lag_members_valid(1, 0x00c0)
	      && port_lag_members_valid(0, 0x000f) && port_lag_members_valid(1, 0x00f0)
	      && port_lag_members_valid(0, 0) && port_lag_members_valid(1, 0),
	      "T1a each group accepts ports of its own bank, or none");
	lacp_lag_set(0, 0x0007);			/* ports 1-3: one of them gets a different partner later */
	CHECK(lacpEnabled && RMA_LACP_ACT == TRAP_ACT_TRAP
	      && regfile[RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP] == 0x74e5,
	      "T1b the first group turns on the trap of 01:80:c2:00:00:02 and leaves the other action bits");
	step(8);
	CHECK(GROUP_MASK == 0x0300 && port_lag_members_get(0) == 0 && port_lag_of(0) == PORT_LAG_NONE,
	      "T1c candidates are not trunk members while they negotiate");
	int announced = tx_count[0] && tx_count[1] && tx_count[2] && !tx_count[3];
	int well_formed = last_tx[0].ethertype == HTONS(0x8809) && last_tx[0].subtype == 1
			  && last_tx[0].dst[5] == 0x02 && HTONS(last_tx[0].rtl_tag.pmask) == PORT_BIT(0)
			  && HTONS(last_tx[1].rtl_tag.pmask) == PORT_BIT(1)
			  && last_tx[0].rtl_tag.flags == HTONS(RTL_TAG_LEARN_DIS | RTL_TAG_KEEP)
			  && HTONS(last_tx[0].actor.port) == 1 && HTONS(last_tx[0].actor.key) == 1
			  && (last_tx[0].actor.state & (P_ACT | P_AGG | P_TO)) == (P_ACT | P_AGG | P_TO);
	CHECK(announced && well_formed && mgmt_vlan_at_tx == 0 && management_vlan == 1,
	      "T1d LACPDUs go out on candidate ports only, CPU-tagged for one port, untagged, management VLAN restored");

	/* T2: a partner that is not in sync yet gets our SYNC only, no members */
	partner_frame(0, SYS_A, P_ACT | P_AGG | P_TO, 1);
	partner_frame(1, SYS_A, P_ACT | P_AGG | P_TO, 1);
	step(8);
	CHECK((lacp_actor_state[0] & (P_SYNC | P_COL | P_DIST)) == P_SYNC && GROUP_MASK == 0x0300,
	      "T2 partner not in sync: actor SYNC only, chip membership empty");

	/* T3: a synchronized partner on both ports: the chip's group 1 takes ports 1 and 2 */
	converge(0, 1, SYS_A);
	CHECK((lacp_actor_state[0] & P_COL) && (lacp_actor_state[1] & P_DIST)
	      && GROUP_MASK == 0x0303 && port_lag_members_get(0) == 0x0003 && port_lag_members_get(1) == 0
	      && port_lag_of(0) == 0 && port_lag_of(1) == 0 && port_lag_of(2) == PORT_LAG_NONE,
	      "T3 negotiated: PORT_TRUNK_GROUP_MASK low nibble holds exactly ports 1 and 2");
	CHECK((TRUNK_CTRL & PORT_TRUNK_CTRL_HASH) == PORT_TRUNK_CTRL_HASH && (TRUNK_CTRL & 0x0180) == 0x0100,
	      "T3b the first members seed all seven hash fields on the zero selector and keep the mode bits");

	/* T4: the second group negotiates with another partner, independently */
	lacp_lag_set(1, 0x00c0);
	step(8);
	CHECK(GROUP_MASK == 0x0303 && RMA_LACP_ACT == TRAP_ACT_TRAP,
	      "T4a group 2 candidates change neither group 1's members nor the trap");
	converge(6, 7, SYS_B);
	CHECK(GROUP_MASK == 0x03c3 && port_lag_members_get(1) == 0x00c0 && port_lag_of(7) == 1
	      && lacp_agg_valid[0] && lacp_agg_valid[1] && !memcmp(lacp_agg_sys[1], SYS_B, MAC_ADDR_LEN),
	      "T4b group 2 aggregates its own partner into the high nibble");

	/* T5: LACPDUs on a port in no LACP group, or from a mis-cabled system, change nothing */
	take(&before);
	int rx3 = lacp_rx_count[3];
	partner_frame(3, SYS_A, IN_SYNC, 1);
	partner_frame(4, SYS_A, IN_SYNC, 1);
	CHECK(lacp_rx_count[3] == rx3 && same(&before), "T5a a port in no LACP group ignores LACPDUs");
	partner_frame(2, SYS_B, IN_SYNC, 1);
	step(8);
	CHECK(GROUP_MASK == 0x03c3 && !(lacp_actor_state[2] & P_SYNC) && !memcmp(lacp_agg_sys[0], SYS_A, MAC_ADDR_LEN),
	      "T5b a candidate that hears another partner system stays out of the group");

	/* T6: refused candidates never touch a working group */
	take(&before);
	lacp_lag_set(0, 0x0030);			/* group 1 with ports of group 2 */
	lacp_lag_set(0, 0x0013);			/* one foreign port among its own */
	lacp_lag_set(1, 0x0003);			/* group 2 with ports of group 1 */
	lacp_lag_set(0, PORT_BIT(CPU_PORT));		/* the CPU port */
	lacp_lag_set(0, 0x0100);			/* chip port 8 */
	lacp_lag_set(2, 0x0001);			/* a third group */
	lacp_lag_set(0xff, 0x0001);
	CHECK(!port_lag_members_valid(0, 0x0030) && !port_lag_members_valid(0, 0x0013)
	      && !port_lag_members_valid(1, 0x0003) && !port_lag_members_valid(0, PORT_BIT(CPU_PORT))
	      && !port_lag_members_valid(0, 0x0100) && !port_lag_members_valid(2, 1)
	      && !port_lag_members_valid(0xff, 1) && same(&before),
	      "T6 cross-bank, CPU, chip-port-8 and third-group candidates: refused, no LACP state or register changed");

	/* T7: applying the same candidates again does not tear the group down */
	take(&before);
	lacp_lag_set(0, 0x0007);
	lacp_lag_set(1, 0x00c0);
	CHECK(same(&before), "T7 the same candidate set again: members and counters untouched");

	/* T8: group 1's partner goes quiet. Its members leave the chip after the short timeout; group 2 stays */
	for (int t = 0; t < 25; t++) {
		partner_frame(6, SYS_B, IN_SYNC, 1);
		partner_frame(7, SYS_B, IN_SYNC, 1);
		step(40);
	}
	CHECK(GROUP_MASK == 0x03c0 && port_lag_members_get(0) == 0 && port_lag_members_get(1) == 0x00c0
	      && !lacp_agg_valid[0] && lacp_agg_valid[1] && lacp_lag_ports[0] == 0x0007,
	      "T8 timeout: group 1's chip nibble is cleared, its candidates stay, group 2 is unaffected");

	/* T9: static members and LACP turn into one another in the chip */
	CHECK(port_lag_members_set(0, 0x000c) && GROUP_MASK == 0x03cc,
	      "T9a a static group is written to the nibble");
	lacp_lag_set(0, 0);			/* 'lag 1 lacp off' on a group that is not under LACP any more */
	CHECK(GROUP_MASK == 0x03cc && lacp_lag_ports[0] == 0 && lacp_port_lag[2] == LACP_LAG_NONE,
	      "T9b leaving LACP releases the candidates and keeps the static members");
	lacp_lag_set(0, 0x000c);		/* the page's Static -> LACP: the static trunk goes */
	CHECK(GROUP_MASK == 0x03c0 && lacp_lag_ports[0] == 0x000c,
	      "T9c putting a static group under LACP clears its trunk while it negotiates");

	/* T10: taking the groups out of LACP drains the chip and, with the last one, drops the address */
	lacp_lag_set(1, 0);
	CHECK(GROUP_MASK == 0x0300 && lacpEnabled && RMA_LACP_ACT == TRAP_ACT_TRAP,
	      "T10a group 2 leaves: its nibble is cleared, group 1 keeps the engine running");
	lacp_lag_set(0, 0);
	CHECK(GROUP_MASK == 0x0300 && !lacpEnabled && RMA_LACP_ACT == TRAP_ACT_DROP
	      && (regfile[RTL836X_REG_RMA_CTRL00 + TRAP_RMA_LACP] & ~RMA_OPERATION_MASK) == (0x7465 & ~RMA_OPERATION_MASK),
	      "T10b the last group leaves: the address is dropped again, other action bits untouched");

	/* T11: 'lacp off' clears every group at once */
	lacp_lag_set(0, 0x0003);
	lacp_lag_set(1, 0x00c0);
	step(8);
	converge(0, 1, SYS_A);
	converge(6, 7, SYS_B);
	int both = GROUP_MASK == 0x03c3;
	lacp_off();
	CHECK(both && GROUP_MASK == 0x0300 && !lacpEnabled && lacp_lag_ports[0] == 0 && lacp_lag_ports[1] == 0,
	      "T11 lacp off: both groups leave LACP and the chip holds no members");

	/* T12: the hash chosen for the switch is kept when a group is rebuilt */
	port_lag_hash_set(0, LAG_HASH_L2_SMAC | LAG_HASH_L4_DPORT);
	lacp_lag_set(0, 0x0003);
	step(8);
	converge(0, 1, SYS_A);
	CHECK(GROUP_MASK == 0x0303 && port_lag_hash_get(1) == (LAG_HASH_L2_SMAC | LAG_HASH_L4_DPORT),
	      "T12 negotiation keeps the hash selector already set");
	lacp_off();

	printf("\n%s (%d failure%s)\n", failures ? "SANDBOX: FAILURES" : "SANDBOX: ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
