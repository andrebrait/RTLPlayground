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
#include "rtl837x_flash.h"
#include "rtl837x_port.h"
#include "rtl837x_lacp.h"
#include "rtl837x_stp.h"
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
extern uint8_t  stp_ent_of[];
extern uint16_t stp_lag_mask[];
extern uint16_t lacp_lag_ports[];
extern uint8_t  lacp_agg_valid[];
extern uint8_t  lacp_port_lag[];
extern uint8_t  lacp_actor_state[];
extern struct syslog_state syslog_state;
extern const char *flash_config;
extern uint16_t cont_len;
extern uint32_t cont_addr;
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

/* MSTP states 0..3 in a mix across the ports, with ports 1 and 2 forming STP lag 1
 * (their entities fold into the lag, which reports its first member's state). */
static void p_stp_states(void)
{
	hw_reg_set(RTL837X_MSTP_STATES, 0x2a1b4c93);
	stp_ent_of[1] = stp_ent_of[2] = STP_LAG_BASE;
	stp_lag_mask[0] = 0x0006;
	send_stp();
	stp_ent_of[1] = 1;
	stp_ent_of[2] = 2;
	stp_lag_mask[0] = 0;
}

/* Mirroring with RX mask 0003 and TX mask 0001, and storm control on port 1
 * with bcast enabled in pps and ucast in kbps, so RX != TX and enabled != pps. */
static void mirror_asym(void) { cli("mirror 9 1 2r"); env_reset(); }
static void storm_mixed(void) { cli("storm 1 ucast 200 kbps"); env_reset(); }
static void p_mirror_asym(void) { mirror_asym(); send_mirror(); }
static void p_storm_mixed(void) { storm_mixed(); send_storm(); }
static void p_l2(void)       { send_l2(0); }

static void append_table(void)
{
	char tbl[512];
	int n = 0;
	for (int i = 0; i < hw_l2_count(); i++) {
		const struct hw_l2 *e = hw_l2_at(i);
		n += snprintf(tbl + n, sizeof tbl - n, "idx %04x a %08x b %08x c %08x\n", e->idx, e->a, e->b, e->c);
	}
	memcpy(outbuf + slen, tbl, n);
	slen += n;
}

static void p_l2_delete(void)
{
	l2_delete(0x020);
	/* the page says nothing; what it did is the table that is left */
	append_table();
}

/* The static entry sits at slot 0x8f0 with port 9 and the static flag, so its delete
 * clears flag, port and age bits that the learned entry has not set; the engine
 * registers show which slot and which table operation the page asked for. The
 * entry at 0x400 is what a search with a mangled index 0x0f0 would hit instead. */
static void p_l2_delete_static(void)
{
	put(0x400, 0xfe, 0xabc, 14, true);	/* a slot between the search index and the entry's own bits */
	l2_delete(0x8f0);
	append_table();
	char regs[160];
	int n = snprintf(regs, sizeof regs, "regs ctrl %08x d0 %08x ina %08x inb %08x inc %08x\n",
			 hw_reg_get(RTL837X_TBL_CTRL), hw_reg_get(RTL837x_TBL_DATA_0), hw_reg_get(RTL837x_TBL_DATA_IN_A),
			 hw_reg_get(RTL837x_TBL_DATA_IN_B), hw_reg_get(RTL837x_TBL_DATA_IN_C));
	memcpy(outbuf + slen, regs, n);
	slen += n;
}

static void p_l2_delete_empty(void)
{
	hw_reset();
	l2_delete(5);
}

/* An entry in VLAN 0xabc on port 14 (both port fields set), and a scan that starts mid-table and wraps. */
static void p_l2_high(void)
{
	put(0x400, 0xfe, 0xabc, 14, true);
	send_l2(0x011);
}

/* The switch's own MAC must survive l2_delete: the page says 0 and the table keeps the entry. */
static void l2_delete_keeps_own_mac(void)
{
	fixture_load();
	hw_l2_put(0x030, uip_ethaddr.addr, 1, CPU_PORT, true);
	const struct hw_l2 *e = hw_l2_find(uip_ethaddr.addr, 1);
	struct hw_l2 before = *e;
	slen = 0;
	l2_delete(before.idx);
	outbuf[slen] = 0;
	e = hw_l2_find(uip_ethaddr.addr, 1);
	tests_run++;
	if (!strstr((const char *)outbuf, "{\"result\":0}") || !e ||
	    e->a != before.a || e->b != before.b || e->c != before.c) {
		printf("  FAIL: l2_delete_keeps_own_mac\n");
		failed++;
		return;
	}
	printf("  ok:   l2_delete_keeps_own_mac\n");
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

/* Both temperature registers set, current one below zero in the _neg variants (the sign branch). */
static void temp_set(void) { hw_reg_set(RTL837X_TM_RESULT, 0x00001a80); hw_reg_set(RTL837X_TM_RESULT_POWERON, 0x00000a40); }
static void temp_neg(void) { hw_reg_set(RTL837X_TM_RESULT, 0x0000f5c0); hw_reg_set(RTL837X_TM_RESULT_POWERON, 0x0000f600); }
static void p_basic_info_neg(void) { temp_neg(); send_basic_info(); }

/* The status page with a module in the slot, with and without receive loss, and with the slot empty
 * but no loss (sfp_pins_last bit 0 = no module, bit 1 = loss; the page reads this debounced snapshot). */
static void p_status_sfp(uint8_t pins)
{
	uint8_t saved = sfp_pins_last;
	sfp_pins_last = pins;
	send_status();
	sfp_pins_last = saved;
}
static void p_status_sfp_ok(void)  { p_status_sfp(0x00); }
static void p_status_sfp_los(void) { p_status_sfp(0x02); }
static void p_status_sfp_gone(void) { p_status_sfp(0x01); }

/* A config of `nonnul` non-NUL bytes (lines of "igmp show", padded with blank lines), NUL after it.
 * send_config must send exactly those bytes, streaming what does not fit the TCP buffer through
 * cont_addr/cont_len; execute_config must run one command per "igmp show" line. The two
 * configs sit on the edges of the NUL scan: the NUL is the first byte of the second 256-byte
 * read, and there is no NUL at all. */
static char cfg_text[CONFIG_LEN + 1];
static int count_stubs(const char *s)
{
	int n = 0;
	for (; (s = strstr(s, "[stub:igmp_show]")); s++)
		n++;
	return n;
}

static void config_case(const char *name, size_t nonnul)
{
	size_t n = 0;
	while (n + 10 <= nonnul) {
		memcpy(cfg_text + n, "igmp show\n", 10);
		n += 10;
	}
	while (n < nonnul)
		cfg_text[n++] = '\n';
	cfg_text[n] = 0;

	fixture_load();
	flash_config = "";
	slen = 0;
	send_config();
	uint16_t hdr = slen;

	flash_config = cfg_text;
	cont_len = 0;
	cont_addr = 0;
	slen = 0;
	send_config();
	size_t room = TCP_OUTBUF_SIZE - hdr;
	size_t sent = nonnul < room ? nonnul : room;
	bool ok = slen == hdr + sent && !memcmp(outbuf + hdr, cfg_text, sent)
		&& cont_len == nonnul - sent && (!cont_len || cont_addr == CONFIG_START + sent);

	out_reset();
	execute_config();
	int cmds = count_stubs(out_buf);
	tests_run++;
	if (!ok || cmds != (int)(nonnul / 10)) {
		printf("  FAIL: %s: slen %u cont_len %u cont_addr %lx, %d commands (want %zu, cont %zu)\n", name,
		       slen, cont_len, (unsigned long)cont_addr, cmds, hdr + sent, nonnul - sent);
		failed++;
		return;
	}
	printf("  ok:   %s (%u bytes sent, %u streamed, %d commands)\n", name, slen, cont_len, cmds);
}

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
	page("send_basic_info_neg", p_basic_info_neg);
	page("send_status_sfp_ok", p_status_sfp_ok);
	page("send_status_sfp_los", p_status_sfp_los);
	page("send_status_sfp_gone", p_status_sfp_gone);
	page("send_counters", p_counters);
	page("send_vlan", p_vlan);
	page("send_vlanlist", send_vlanlist);
	page("send_l2", p_l2);
	page("l2_delete", p_l2_delete);
	l2_delete_keeps_own_mac();
	page("send_l2_high", p_l2_high);
	page("l2_delete_static", p_l2_delete_static);
	page("l2_delete_empty", p_l2_delete_empty);
	page("send_mirror", send_mirror);
	page("send_mirror_asym", p_mirror_asym);
	page("send_lag", send_lag);
	page("send_stp", send_stp);
	page("send_stp_states", p_stp_states);
	page("send_lacp", send_lacp);
	page("send_lldp", send_lldp);
	page("send_eee", send_eee);
	page("send_bandwidth", send_bandwidth);
	page("send_storm", send_storm);
	page("send_storm_mixed", p_storm_mixed);
	page("send_isolation", send_isolation);
	page("send_mtu", send_mtu);
	page("send_config", send_config);
	config_case("config_nul_on_boundary", 256);
	config_case("config_full_no_nul", CONFIG_LEN);

	cmd("cli_mtu_show", "mtu show");
	cmd("cli_mirror_status", "mirror status");
	cmd_with("cli_mirror_status_asym", "mirror status", mirror_asym);
	cmd("cli_lag_show", "lag show");
	cmd("cli_bw_show", "bw show");
	cmd("cli_bw_status", "bw status 1");
	cmd("cli_storm_show", "storm show");
	cmd("cli_storm", "storm");
	cmd_with("cli_storm_mixed", "storm", storm_mixed);
	cmd("cli_eee_show", "eee show");
	cmd("cli_eee_status", "eee status");
	cmd("cli_vlan_show", "vlan show");
	cmd("cli_l2", "l2");
	cmd("cli_stat", "stat");
	cmd_with("cli_stat_sfp_in", "stat", sfp_inserted);
	cmd_with("cli_temp", "temp", temp_set);
	cmd_with("cli_temp_neg", "temp", temp_neg);
	cmd("cli_sfp", "sfp");
	cmd_with("cli_sfp_in", "sfp", sfp_inserted);
	cmd("cli_regget", "regget 0abc");
	cmd("cli_sds", "sds");
	cmd("cli_gpio", "gpio");
	cmd("cli_rnd", "rnd");
	cmd("cli_unknown", "foo");

	printf("\n%d outputs, %d failed%s\n", tests_run, failed, update ? " (GOLDEN_UPDATE: goldens rewritten)" : "");
	return failed ? 1 : 0;
}
