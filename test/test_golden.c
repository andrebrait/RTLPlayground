/*
 * test_golden.c - characterization of what the firmware prints: every JSON
 * page of httpd/page_impl.c and a set of CLI commands of cmd_parser.c, run on
 * one fixed register fixture, byte-compared with test/golden/<name>.txt.
 *
 * The goldens were taken from the code as it stood before the platform seam,
 * so a refactor that moves hardware access must leave every byte unchanged.
 *   build/test_golden                  compare, fail on any difference
 *   GOLDEN_UPDATE=1 build/test_golden  rewrite the goldens (review the diff!)
 * Subsystems not linked here are stubs that print "[stub:<name>]" (golden_env.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_lacp.h"
#include "machine.h"
#include "page_impl.h"
#include "phy.h"
#include "uip.h"
#include "syslog.h"
#include "cmd_parser.h"
#include "support.h"
#include "hw_mock.h"

extern uint8_t  outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;
extern uint8_t  vlan_names[VLAN_NAMES_SIZE];
extern uint16_t vlan_ptr;
extern uint16_t management_vlan;
extern uint8_t  gpio_last_value[8];
extern uint16_t session_timeout;
extern char     hostname[24];
extern char     port_names[9][PORT_NAME_SIZE];
extern bool     stp_enabled, lldp_enabled;
extern uint16_t lldp_logical_port_status;
extern uint8_t  lacpEnabled;
extern uint16_t lacp_lag_ports[];
extern uint8_t  lacp_agg_valid[];
extern uint8_t  lacp_port_lag[];
extern uint8_t  lacp_actor_state[];
extern struct syslog_state syslog_state;
extern const char *flash_config;
extern uint64_t gpio_pins_low;
void phy_mock_reset(void);
void phy_mock_set(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t val);

static int update;
static int failed;

/* ---- golden compare ---------------------------------------------------- */

static void golden(const char *name, const char *buf, size_t len)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s.txt", GOLDEN_DIR, name);
	tests_run++;

	if (update) {
		FILE *f = fopen(path, "wb");
		if (!f || fwrite(buf, 1, len, f) != len) {
			printf("  FAIL: %s: cannot write %s\n", name, path);
			failed++;
			return;
		}
		fclose(f);
		printf("  wrote %-14s %5zu bytes\n", name, len);
		return;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("  FAIL: %s: missing %s (run with GOLDEN_UPDATE=1)\n", name, path);
		failed++;
		return;
	}
	static char want[1 << 16];
	size_t n = fread(want, 1, sizeof want, f);
	fclose(f);
	size_t i = 0;
	while (i < n && i < len && want[i] == buf[i])
		i++;
	if (i == n && i == len) {
		printf("  ok:   %s (%zu bytes)\n", name, len);
		return;
	}
	printf("  FAIL: %s: first difference at offset %zu (golden %zu bytes, got %zu bytes)\n", name, i, n, len);
	printf("        golden: \"%.40s\"\n        got:    \"%.40s\"\n", i < n ? want + i : "<end>", i < len ? buf + i : "<end>");
	failed++;
}

/* ---- the fixture ------------------------------------------------------- */

/* Type a line, then do what the firmware main loop does with it. */
static void cli(const char *line)
{
	feed(line);
	feed("\n");
	if (cmd_available) {
		cmd_available = 0;
		cmd_tokenize();
		if (err_status == ERR_OK)
			cmd_parser();
		print_cmd_prompt();
	}
}

static void put(uint16_t idx, uint8_t last, uint16_t vid, uint16_t port, bool st)
{
	uint8_t m[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, last };
	hw_l2_put(idx, m, vid, port, st);
}

/* Two ports up (0: 2.5G, 1: 1G), the SFP port 8 without a module, MIB counters
 * on those ports, VLANs 1 and 10 (named), three L2 entries (one static),
 * mirroring, a LAG with a hash, EEE, bandwidth and storm limits on port 1,
 * MTU 9000 on port 1, PVID and isolation, chip temperature raw 0x1a80. */
static void fixture_load(void)
{
	hw_reset();
	phy_mock_reset();
	t_memset(vlan_names, 0, sizeof vlan_names);
	vlan_ptr = 0;
	management_vlan = 1;
	t_memset(gpio_last_value, 0, sizeof gpio_last_value);
	t_memset(port_names, 0, sizeof port_names);
	strcpy(port_names[0], "uplink");
	strcpy(port_names[1], "nas");
	session_timeout = 200;
	strcpy(hostname, "hosttest");
	/* the pages print the low byte of each word first, as on the 8051 */
	uip_hostaddr[0] = 192 | 168 << 8; uip_hostaddr[1] = 1 | 2 << 8;
	uip_draddr[0]   = 192 | 168 << 8; uip_draddr[1]   = 1 | 1 << 8;
	uip_netmask[0]  = 255 | 255 << 8; uip_netmask[1]  = 255 | 0 << 8;
	t_memset(&syslog_state, 0, sizeof syslog_state);
	syslog_state.server_ip[0] = 192; syslog_state.server_ip[1] = 168;
	syslog_state.server_ip[2] = 1;   syslog_state.server_ip[3] = 9;
	syslog_state.server_port = 514;
	flash_config = "hostname hosttest\nmtu 1 9000\nvlan 10 lab 1 2t\n";
	stp_enabled = 1;
	lldp_enabled = 1;
	lldp_logical_port_status = 0x0003;
	lacpEnabled = 1;
	lacp_lag_ports[0] = 0x0003;
	lacp_agg_valid[0] = 1;
	lacp_port_lag[0] = lacp_port_lag[1] = 1;
	lacp_actor_state[0] = lacp_actor_state[1] = 0x3d;

	/* link state and speed, temperature, EEE */
	hw_reg_set(RTL837X_REG_LINKS_STS, 0x00030000);
	hw_reg_set(RTL837X_REG_LINKS, 0x00000025);
	hw_reg_set(RTL837X_TM_RESULT, 0x00001a80);	/* 53.0 C */
	gpio_pins_low = 0;				/* every SFP slot empty */
	hw_reg_set(RTL8373_PHY_EEE_ABLTY, 0x00000003);
	for (uint8_t p = 0; p < 2; p++) {
		phy_mock_set(p, PHY_MMD31, 0xa610, 0x2000);
		phy_mock_set(p, PHY_MMD_AN, PHY_ANEG_MGBASE_CTRL, p ? 0x0000 : 0x0080);
		phy_mock_set(p, PHY_MMD31, PHY_MMD31_GBCR, 0x0200);
		phy_mock_set(p, PHY_MMD_AN, PHY_ANEG_ADV, 0x01e1);
		phy_mock_set(p, PHY_MMD_AN, PHY_EEE_ADV2, p ? 0 : PHY_EEE_BIT_2G5);
		phy_mock_set(p, PHY_MMD_AN, PHY_EEE_ADV, PHY_EEE_BIT_1G | PHY_EEE_BIT_100M);
		phy_mock_set(p, PHY_MMD_AN, PHY_EEE_LP_ABILITY2, p ? 0 : PHY_EEE_BIT_2G5);
		phy_mock_set(p, PHY_MMD_AN, PHY_EEE_LP_ABILITY, PHY_EEE_BIT_1G);
	}
	hw_reg_set(0x0abc, 0x00c0ffee);
	hw_reg_set(RTL837X_REG_SDS_MODES, 0x00000a05);
	hw_reg_set(RTL837X_RAND_NUM0, 0x89abcdef);
	hw_reg_set(RTL837X_RAND_NUM1, 0x00001234);
	hw_reg_set(RTL837X_REG_GPIO_00_31_INPUT, 0x80000001);
	hw_reg_set(RTL837X_REG_GPIO_00_31_INPUT + 4, 0x0000f00d);
	hw_reg_set(RTL837X_L2_LRN_PORT_CONSTRT_CNT + (0 << 2), 0x2);
	hw_reg_set(RTL837X_L2_LRN_PORT_CONSTRT_CNT + (1 << 2), 0x1);

	/* MIB counters; one of them past 32 bits */
	for (uint8_t c = 0; c < 0x37; c++) {
		hw_counter_set(0, c, 0x1000 + c * 7);
		hw_counter_set(1, c, 0x20 + c);
	}
	/* 64-bit packet counters: each high-word byte is set on its own somewhere,
	 * so a reader that drops one byte of the high word changes an output */
	hw_counter_set(0, STAT_COUNTER_TX_PKTS, 0x123456789ULL);
	hw_counter_set(0, STAT_COUNTER_RX_PKTS, 0x01000000deadbeefULL);
	hw_counter_set(1, STAT_COUNTER_TX_PKTS, 0x00000100000004d2ULL);
	hw_counter_set(1, STAT_COUNTER_RX_PKTS, 0x000100000000004fULL);
	hw_counter_set(1, STAT_COUNTER_ERR_PKTS, 0x0000000500000003ULL);

	/* L2: two learned, one static */
	put(0x010, 0x01, 1, 0, false);
	put(0x020, 0x02, 10, 1, false);
	put(0x8f0, 0x03, 1, CPU_PORT, true);

	/* what the CLI configures lands in the same register file */
	env_reset();
	cli("vlan 1 1 2 3 4 5 6 7 8 9");
	cli("vlan 10 lab 1 2t");
	cli("mirror 9 1 2");
	cli("lag 1 1 2");
	cli("laghash 1 smac dmac");
	cli("mtu 1 9000");
	cli("pvid 1 10");
	cli("pvid 3 10");
	cli("isolate 1 2 3");
	cli("bw in 1 0100");
	cli("bw out 1 0200");
	cli("storm 1 bcast 100 pps");
	env_reset();	/* clears the echo and the command history */
}

/* ---- one output each --------------------------------------------------- */

static void page(const char *name, void (*fn)(void))
{
	fixture_load();
	slen = 0;
	fn();
	golden(name, (const char *)outbuf, slen < TCP_OUTBUF_SIZE ? slen : TCP_OUTBUF_SIZE);
}

static void p_counters(void) { send_counters(1); }
static void p_basic_info_frac(void)
{
	hw_reg_set(RTL837X_TM_RESULT, 0x00001ac0);	/* 53.5 C: the fraction digit */
	send_basic_info();
}
static void p_vlan(void)     { send_vlan(10); }
static void p_l2(void)       { send_l2(0); }

static void p_l2_delete(void)
{
	l2_delete(0x020);
	/* the page says nothing; what it did is the table that is left */
	char tbl[512];
	int n = 0;
	for (int i = 0; i < hw_l2_count(); i++) {
		const struct hw_l2 *e = hw_l2_at(i);
		n += snprintf(tbl + n, sizeof tbl - n, "idx %04x a %08x b %08x c %08x\n", e->idx, e->a, e->b, e->c);
	}
	memcpy(outbuf + slen, tbl, n);
	slen += n;
}

static void cmd_with(const char *name, const char *line, void (*pre)(void))
{
	fixture_load();
	if (pre)
		pre();
	cli(line);
	if (out_len >= sizeof(out_buf) - 1) {
		printf("  FAIL: %s: output exceeds the capture buffer\n", name);
		failed++;
		return;
	}
	golden(name, out_buf, out_len);
}

static void sfp_inserted(void) { gpio_pins_low = 1; }	/* pin_detect of both slots is 0 */

static void cmd(const char *name, const char *line) { cmd_with(name, line, NULL); }

int main(void)
{
	update = getenv("GOLDEN_UPDATE") != NULL;
	setvbuf(stdout, NULL, _IONBF, 0);
	watchdog_arm(30, "golden run");
	printf("== golden output: JSON pages and CLI ==\n");

	page("send_basic_info", send_basic_info);
	page("send_basic_info_frac", p_basic_info_frac);
	page("send_status", send_status);
	page("send_counters", p_counters);
	page("send_vlan", p_vlan);
	page("send_vlanlist", send_vlanlist);
	page("send_l2", p_l2);
	page("l2_delete", p_l2_delete);
	page("send_mirror", send_mirror);
	page("send_lag", send_lag);
	page("send_stp", send_stp);
	page("send_lacp", send_lacp);
	page("send_lldp", send_lldp);
	page("send_eee", send_eee);
	page("send_bandwidth", send_bandwidth);
	page("send_storm", send_storm);
	page("send_isolation", send_isolation);
	page("send_mtu", send_mtu);
	page("send_config", send_config);

	cmd("cli_mtu_show", "mtu show");
	cmd("cli_mirror_status", "mirror status");
	cmd("cli_lag_show", "lag show");
	cmd("cli_bw_show", "bw show");
	cmd("cli_bw_status", "bw status 1");
	cmd("cli_storm_show", "storm show");
	cmd("cli_storm", "storm");
	cmd("cli_eee_show", "eee show");
	cmd("cli_eee_status", "eee status");
	cmd("cli_vlan_show", "vlan show");
	cmd("cli_l2", "l2");
	cmd("cli_stat", "stat");
	cmd_with("cli_stat_sfp_in", "stat", sfp_inserted);
	cmd("cli_regget", "regget 0abc");
	cmd("cli_sds", "sds");
	cmd("cli_gpio", "gpio");
	cmd("cli_rnd", "rnd");
	cmd("cli_unknown", "foo");

	printf("\n%d outputs, %d failed%s\n", tests_run, failed, update ? " (GOLDEN_UPDATE: goldens rewritten)" : "");
	return failed ? 1 : 0;
}
