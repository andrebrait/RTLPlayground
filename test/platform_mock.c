/*
 * platform_mock.c - the platform side of the seam check.
 *
 * Defines, against platform.h only, what cmd_parser.c, cmd_editor.c and
 * httpd/page_impl.c need from the platform, with no register file behind it.
 * Each task that moves hardware access behind platform.h adds its mock here.
 */
#include "platform.h"
#include "rtl837x_flash.h"

const struct machine machine = {
	.machine_name = "SEAMTEST",
	.min_port = 0,
	.max_port = 8,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};

uint8_t  outbuf[TCP_OUTBUF_SIZE];
uint16_t slen;

uint16_t strtox(uint8_t *dst, const char *s)
{
	uint8_t *b = dst;
	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}

__xdata char port_names[9][PORT_NAME_SIZE];
__xdata uint8_t sfp_pins_last;

/* Ports: fixed values the seam test looks for in the pages. */
uint8_t port_link_get(uint8_t port) __banked { return 3; }
bool port_phy_enabled(uint8_t port) __banked { return true; }
uint8_t port_phy_adv_get(uint8_t port) __banked { return 0x3f; }

__xdata uint8_t port_buf[24];

/* Every counter reads 0x0000000100000002. */
void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked
{
	static const uint8_t v[8] = { 0, 0, 0, 1, 0, 0, 0, 2 };
	for (uint8_t i = 0; i < 8; i++)
		port_buf[i] = v[i];
}

void port_macs_learned_get(uint8_t port) __banked
{
	port_buf[0] = port_buf[1] = port_buf[2] = 0;
	port_buf[3] = 5;
}

uint16_t port_mtu_get(uint8_t port) __banked { return 1522; }
void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked { }

uint8_t port_eee_adv_get(uint8_t port) __banked { return 7; }
uint8_t port_eee_lp_get(uint8_t port) __banked { return 5; }
bool port_eee_active(uint8_t port) __banked { return true; }

/* Mirror enabled on logical port 8, RX mask 0x0003, TX mask 0x0001. */
void port_mirror_get(void) __banked
{
	static const uint8_t v[6] = { 1, 8, 0x00, 0x03, 0x00, 0x01 };
	for (uint8_t i = 0; i < 6; i++)
		port_buf[i] = v[i];
}

/* Every LAG hashes 0x00000007. */
void port_lag_hash_get(uint8_t lag) __banked
{
	port_buf[0] = port_buf[1] = port_buf[2] = 0;
	port_buf[3] = 7;
}

/* Every port is limited at 0xabcde; ingress also has flow control. */
void bandwidth_get(uint8_t port, __xdata uint8_t egress) __banked
{
	static const uint8_t v[5] = { 1, 0x0a, 0xbc, 0xde, 0 };
	for (uint8_t i = 0; i < 5; i++)
		port_buf[i] = v[i];
	port_buf[4] = !egress;
}

/* Storm types: enabled 1010, pps 1100, rate 0x000123 + type. */
void storm_get(uint8_t port) __banked
{
	for (uint8_t t = 0; t < 4; t++) {
		port_buf[t] = !(t & 1);
		port_buf[4 + t] = t < 2;
		port_buf[8 + 4 * t] = 0;
		port_buf[9 + 4 * t] = 0x00;
		port_buf[10 + 4 * t] = 0x01;
		port_buf[11 + 4 * t] = 0x23 + t;
	}
}

/* VLAN entry: valid (bit 25), untagged 0x007 (bits 19..10), members 0x015 (bits 9..0):
 * word 0x02001c15. VLAN 20 is not a table entry. */
bool vlan_entry_get(uint16_t vlan) __banked
{
	static const uint8_t v[4] = { 0x02, 0x00, 0x1c, 0x15 };
	for (uint8_t i = 0; i < 4; i++)
		port_buf[i] = v[i];
	return vlan != 20;
}

/* Two entries: slot 0x010 learned on port 5, slot 0x020 static on port 9. */
__xdata struct l2_entry l2_buf;

bool l2_entry_next(void) __banked
{
	static const struct l2_entry t[2] = {
		{ 0x010, 1,  { 0x00, 0x11, 0x22, 0x33, 0x44, 0x01 }, 5, false },
		{ 0x020, 10, { 0x00, 0x11, 0x22, 0x33, 0x44, 0x02 }, 9, true },
	};
	const struct l2_entry *e = l2_buf.idx > 0x010 && l2_buf.idx <= 0x020 ? &t[1] : &t[0];
	l2_buf = *e;
	return true;
}

uint8_t l2_entry_delete(uint16_t idx) __banked { return idx == 0x010; }

/* Protocol state: STP on with every port forwarding, LACP and LLDP off. */
__xdata struct uip_eth_addr uip_ethaddr;
__xdata bool stp_enabled = true;
__xdata bool lldp_enabled;
__xdata uint16_t lldp_logical_port_status;
__xdata uint8_t lacpEnabled;
__xdata uint8_t stp_prio, stp_hello_s, stp_maxage_s, stp_fwddelay_s, stp_rstp, stp_txhold;
__xdata uint8_t stp_pflags[STP_ENTITIES], stp_pprio[STP_ENTITIES], stp_pp2p[STP_ENTITIES];
__xdata uint32_t stp_pcost[STP_ENTITIES], stp_dcost[STP_ENTITIES];
__xdata uint8_t stp_ent_of[STP_PORTS];
__xdata uint16_t stp_lag_mask[STP_LAG_COUNT];
__xdata struct bridge stp_dbridge[STP_ENTITIES];
__xdata uint16_t stp_dpid[STP_ENTITIES], stp_bpdu_age[STP_ENTITIES];
__xdata struct bridge root_bridge;
__xdata uint32_t root_bridge_cost;
__xdata uint8_t stp_root_port = 0xff;
__xdata uint16_t stp_tc_count;
__xdata uint8_t lacp_actor_state[10], lacp_partner_state[10], lacp_rx_state[10], lacp_port_lag[10];
__xdata uint8_t lacp_partner_sys[10][MAC_ADDR_LEN];
__xdata uint16_t lacp_rx_count[10], lacp_lag_ports[LACP_NUM_LAGS], lacp_members_last[LACP_NUM_LAGS];
__xdata uint8_t lacp_agg_sys[LACP_NUM_LAGS][MAC_ADDR_LEN], lacp_agg_valid[LACP_NUM_LAGS];

/* Every port is forwarding (state 3). */
uint8_t stp_port_state_get(uint8_t log_port) __banked { return 3; }

/* Chip temperature reads raw 0x1a80 = 53.0 C. */
int16_t chip_temp_get(uint8_t sensor) __banked { return 0x1a80; }

/* An SFP slot holds a module, with receive loss at the last poll. */
bool sfp_present(uint8_t slot) __banked { return true; }
bool sfp_los(uint8_t slot) __banked { return true; }
bool sfp_present_live(uint8_t slot) { return true; }

/* The configuration area holds one line, zeros after it. */
static const char config_text[] = "hostname seam\n";

uint16_t config_read(uint16_t offset, __xdata uint8_t * __xdata buf, __xdata uint16_t len) __banked
{
	if (offset >= CONFIG_LEN)
		return 0;
	if (len > CONFIG_LEN - offset)
		len = CONFIG_LEN - offset;
	for (uint16_t i = 0; i < len; i++)
		buf[i] = offset + i < sizeof config_text - 1 ? config_text[offset + i] : 0;
	return len;
}

void config_stream_rest(uint16_t offset, __xdata uint16_t len) __banked { }

/* A platform with no console commands of its own: every unknown word stays unknown. */
bool platform_cmd(void) __banked { return false; }
