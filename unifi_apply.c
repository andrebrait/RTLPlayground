/*
 * Applying the controller's settings, included by unifi.c (BANK5), whose
 * reply parser and buffer it shares.
 *
 * A setparam's system_cfg is read into a compact description of what the
 * controller wants (ua_*). The areas selected with "unifi apply" are then
 * turned into the commands config.txt uses: config.txt keeps its lines of
 * every other area, the lines of an applied area are replaced by the
 * controller's, and only what changed runs, one command per tick, before
 * the new config.txt is written. So the settings come back at boot without
 * the controller, as on a UniFi switch, and the controller wins over local
 * edits in the areas it applies.
 *
 * Left alone, so the switch stays reachable: the management VLAN's lines
 * and the native VLAN of the uplink. Also left alone: port names, VLAN
 * names, per-port STP, and EEE and flow control on SFP ports.
 *
 * Everything is static: the 8051's internal RAM holds the stack.
 */

/* The areas, UA_*, and ua_mask are at the top of unifi.c */

static __code const char *const __code ua_names[UA_AREAS] = {
	"vlans", "ports", "lag", "stp", "igmp", "jumbo", "mirror"
};

extern __xdata uint16_t management_vlan;
extern __xdata char save_cmd;

/* Front-panel ports 1..9, VLAN slots 1..UA_NV */
#define UA_NP	10
#define UA_NV	32

#define UA_P_OFF	0x01	/* status=disabled */
#define UA_P_FORCED	0x02	/* autoneg=disabled */
#define UA_P_FULL	0x04	/* duplex=enabled */
#define UA_P_EEE	0x08	/* eee given */
#define UA_P_EEE_ON	0x10
#define UA_P_FC		0x20	/* flowctrl given */
#define UA_P_FC_ON	0x40
#define UA_P_AGG	0x80	/* opmode=aggregate */

/* "unifi apply" lines: config.txt gets the current selection */
#define UA_SELF		0x80

#if SOC_FAMILY == RTL836X
#define UA_LAGS		2	/* fixed groups: ports 0-3 and 4-7 */
#else
#define UA_LAGS		4
#endif

#define UA_V_UNTAGGED	1
#define UA_V_TAGGED	2
#define UA_V_EXCLUDE	3

static __xdata uint8_t ua_seen;		/* the areas system_cfg describes */
static __xdata uint8_t ua_pf[UA_NP];	/* UA_P_* */
static __xdata uint8_t ua_speed[UA_NP];	/* index into ua_speeds, 0 = none */
static __xdata uint8_t ua_lag[UA_NP];	/* the controller's LAG id */
static __xdata uint8_t ua_mir[UA_NP];	/* opmode=mirror: the port it mirrors, 0xff unknown */
static __xdata uint16_t ua_pvid[UA_NP];	/* 0 = not given */
static __xdata uint8_t ua_asic[UA_NP];	/* front port to 1 + ASIC port, 0 = none */
static __xdata uint16_t ua_vid[UA_NV];	/* 0 = no such slot */
static __xdata uint8_t ua_vdef[UA_NV];	/* the slot's mode, UA_V_* */
static __xdata uint8_t ua_vigmp[UA_NV];
static __xdata uint16_t ua_vset[UA_NV], ua_vun[UA_NV], ua_vtag[UA_NV];	/* per-port lines, bit = front port */
static __xdata uint8_t ua_stp, ua_stpver, ua_jumbo;	/* 0 not given, 1 off/stp, 2 on/rstp */
static __xdata uint16_t ua_prio;	/* 0xffff = not given */

static __code const char *const __code ua_speeds[] = {
	"", "10m", "100m", "1g", "2g5", "5g", "10g"
};
static __code const uint16_t ua_mbps[] = { 0, 10, 100, 1000, 2500, 5000, 10000 };
#if SOC_FAMILY == RTL836X
#define UA_SPEEDS	4	/* "port N" takes 10m, 100m and 1g only */
#else
#define UA_SPEEDS	7
#endif

/* The parser: a line of system_cfg is [ua_i, ua_le) */
static __xdata uint16_t ua_i, ua_le, ua_n, ua_vs;
static __xdata uint8_t ua_k, ua_q;
static __xdata uint8_t ua_over;		/* too much to hold: nothing is applied */
static __xdata uint8_t ua_vover;	/* more VLAN slots than UA_NV */

static uint8_t ua_lit(__code const char *s)
{
	for (u_j = 0; s[u_j]; u_j++)
		if (ua_i + u_j >= ua_le || u_p[ua_i + u_j] != s[u_j])
			return 0;
	ua_i += u_j;
	return 1;
}

/* A value that ends the line */
static uint8_t ua_val(__code const char *s)
{
	ua_vs = ua_i;
	if (ua_lit(s) && ua_i == ua_le)
		return 1;
	ua_i = ua_vs;
	return 0;
}

/* Decimal digits into ua_n; 0 when there are none or they pass 65535 */
static uint8_t ua_num(void)
{
	ua_n = 0;
	for (ua_q = 0; ua_i < ua_le && u_p[ua_i] >= '0' && u_p[ua_i] <= '9'; ua_i++) {
		if (ua_n >= 6553)
			return 0;
		ua_q = 1;
		ua_n = ua_n * 10 + u_p[ua_i] - '0';
	}
	if (!ua_q)
		return 0;
	return 1;
}

/* untagged, tagged or exclude ending the line, as UA_V_*, else 0 */
static uint8_t ua_vmode(void)
{
	if (ua_val("untagged"))
		return UA_V_UNTAGGED;
	if (ua_val("tagged"))
		return UA_V_TAGGED;
	if (ua_val("exclude"))
		return UA_V_EXCLUDE;
	return 0;
}

static void ua_port_line(void)
{
	if (!ua_num() || ua_n == 0 || ua_n >= UA_NP || !ua_lit("."))
		return;
	ua_k = ua_n;
	ua_seen |= UA_PORTS | UA_LAG | UA_MIRROR;
	if (ua_val("status=disabled")) {
		ua_pf[ua_k] |= UA_P_OFF;
		return;
	}
	if (ua_val("autoneg=disabled")) {
		ua_pf[ua_k] |= UA_P_FORCED;
		return;
	}
	if (ua_val("duplex=enabled")) {
		ua_pf[ua_k] |= UA_P_FULL;
		return;
	}
	if (ua_lit("eee=")) {
		ua_pf[ua_k] |= UA_P_EEE;
		if (ua_val("enabled"))
			ua_pf[ua_k] |= UA_P_EEE_ON;
		return;
	}
	if (ua_lit("flowctrl=")) {
		ua_pf[ua_k] |= UA_P_FC;
		if (ua_val("enabled"))
			ua_pf[ua_k] |= UA_P_FC_ON;
		return;
	}
	if (ua_lit("speed=")) {
		if (!ua_num() || ua_i != ua_le)
			return;
		for (ua_q = 1; ua_q < UA_SPEEDS; ua_q++)
			if (ua_mbps[ua_q] == ua_n)
				ua_speed[ua_k] = ua_q;
		return;
	}
	if (ua_lit("opmode=")) {
		if (ua_val("aggregate"))
			ua_pf[ua_k] |= UA_P_AGG;
		else if (ua_val("mirror") && !ua_mir[ua_k])
			ua_mir[ua_k] = 0xff;
		return;
	}
	if (ua_lit("lag=")) {
		if (ua_num() && ua_i == ua_le && ua_n && ua_n < 256)
			ua_lag[ua_k] = ua_n;
		return;
	}
	if (ua_lit("mirror_port=")) {
		if (ua_num() && ua_i == ua_le && ua_n && ua_n < UA_NP)
			ua_mir[ua_k] = ua_n;
		return;
	}
	if (ua_lit("pvid=")) {
		if (ua_num() && ua_i == ua_le && ua_n && ua_n < 4095)
			ua_pvid[ua_k] = ua_n;
	}
}

static void ua_vlan_line(void)
{
	if (!ua_num() || !ua_lit("."))
		return;	/* switch.vlan.status and the like */
	if (ua_n == 0 || ua_n > UA_NV) {
		ua_vover = 1;	/* VLANs it cannot hold: the ones it can are not all */
		return;
	}
	ua_k = ua_n - 1;
	ua_seen |= UA_VLANS | UA_IGMP;
	u_ls = ua_i;
	if (ua_lit("id=")) {
		if (ua_num() && ua_i == ua_le && ua_n && ua_n < 4095)
			ua_vid[ua_k] = ua_n;
		return;
	}
	if (ua_lit("mode=")) {
		ua_vdef[ua_k] = ua_vmode();
		return;
	}
	if (ua_lit("igmp_snooping=")) {
		ua_vigmp[ua_k] = ua_val("true");
		return;
	}
	if (ua_lit("port.")) {
		if (!ua_num() || ua_n == 0 || ua_n >= UA_NP || !ua_lit(".mode="))
			return;
		ua_q = ua_vmode();
		if (!ua_q)
			return;
		ua_vset[ua_k] |= (uint16_t)1 << ua_n;
		if (ua_q == UA_V_UNTAGGED)
			ua_vun[ua_k] |= (uint16_t)1 << ua_n;
		else if (ua_q == UA_V_TAGGED)
			ua_vtag[ua_k] |= (uint16_t)1 << ua_n;
	}
}

static void ua_line(void)
{
	if (ua_lit("switch.port.")) {
		ua_port_line();
		return;
	}
	if (ua_lit("switch.vlan.")) {
		ua_vlan_line();
		return;
	}
	if (ua_lit("switch.stp.")) {
		ua_seen |= UA_STP;
		if (ua_lit("status=")) {
			ua_stp = 1 + ua_val("enabled");
		} else if (ua_lit("version=")) {
			if (ua_val("rstp"))
				ua_stpver = 2;
			else if (ua_val("stp"))
				ua_stpver = 1;
		} else if (ua_lit("priority=")) {
			if (ua_num() && ua_i == ua_le && ua_n <= 61440)
				ua_prio = ua_n;
		}
		return;
	}
	if (ua_lit("switch.jumboframes=")) {
		ua_seen |= UA_JUMBO;
		ua_jumbo = 1 + ua_val("enabled");
	}
}

/* system_cfg, a JSON string at [u_ms, u_me): key=value lines separated by
 * an escaped newline. An escaped backslash before an n is not one. */
static void ua_parse(void)
{
	memset(ua_pf, 0, sizeof(ua_pf));
	memset(ua_speed, 0, sizeof(ua_speed));
	memset(ua_lag, 0, sizeof(ua_lag));
	memset(ua_mir, 0, sizeof(ua_mir));
	memset(ua_pvid, 0, sizeof(ua_pvid));
	memset(ua_vid, 0, sizeof(ua_vid));
	memset(ua_vdef, 0, sizeof(ua_vdef));
	memset(ua_vigmp, 0, sizeof(ua_vigmp));
	memset(ua_vset, 0, sizeof(ua_vset));
	memset(ua_vun, 0, sizeof(ua_vun));
	memset(ua_vtag, 0, sizeof(ua_vtag));
	memset(ua_asic, 0, sizeof(ua_asic));
	ua_seen = ua_stp = ua_stpver = ua_jumbo = ua_vover = 0;
	ua_prio = 0xffff;
	for (ua_k = MACHINE_MIN_PORT; ua_k <= MACHINE_MAX_PORT; ua_k++)
		if (machine.log_to_phys_port[ua_k] && machine.log_to_phys_port[ua_k] < UA_NP)
			ua_asic[machine.log_to_phys_port[ua_k]] = ua_k + 1;
	for (u_b = u_ms; u_b < u_me; u_b = ua_le + 2) {
		for (ua_le = u_b; ua_le < u_me; ua_le++) {
			if (u_p[ua_le] != '\\')
				continue;
			if (ua_le + 1 < u_me && u_p[ua_le + 1] == 'n')
				break;
			ua_le++;	/* an escape: skip what it escapes */
		}
		if (ua_le > u_me)
			ua_le = u_me;
		ua_i = u_b;
		ua_line();
	}
}


/* Building the new config.txt and the commands that bring the switch to it.
 * u_buf holds the old config.txt, the new one, the commands and the
 * console line being edited, which a command would overwrite. */
#define UA_OLD		0
#define UA_NEW		CONFIG_LEN
#define UA_EXE		(2 * CONFIG_LEN)
#define UA_SAVE		(UNIFI_BUF_SIZE - CMD_BUF_SIZE)
#define UA_LINE_MAX	(CMD_BUF_SIZE + 64)	/* room kept for the line being written */

static __xdata uint16_t ua_old_end, ua_new_end, ua_gen, ua_x, ua_y, ua_z, ua_lim, ua_a, ua_b, ua_u, ua_nv;
static __xdata uint16_t ua_ha, ua_hb, ua_hy, ua_hz, ua_sa, ua_sb;
static __xdata uint8_t ua_c8, ua_w, ua_m, ua_g, ua_gu, ua_area, ua_t, ua_p;
static __xdata uint16_t ua_bit;
static __xdata uint8_t ua_changed;	/* the new config.txt differs */
static __xdata uint8_t ua_busy;		/* commands are running */
static __xdata uint16_t ua_upvid, ua_nm, ua_sel;	/* the uplink's native VLAN, 0 none */
static __xdata uint8_t ua_lg, ua_vm;
static __xdata uint16_t ua_pb, ua_px;
static __xdata uint16_t ua_sum, ua_osum;	/* config.txt's sum, as planned */
static __xdata uint8_t ua_eff;		/* the areas this plan applies */
static __xdata uint8_t ua_upsrc;	/* 1 + the uplink's ASIC port, as ua_uplink() takes it */
static __xdata uint8_t ua_defer;	/* VLANs wait for an uplink: report no cfgversion */

/* Writing at unifi.pos, below ua_lim */
static uint8_t ua_room(void)
{
	if (unifi.pos + UA_LINE_MAX > ua_lim) {
		ua_over = 1;
		return 0;
	}
	return 1;
}

static void ua_eol(void)
{
	u_c('\n');
}

/* Does a word end at o? */
static uint8_t ua_ended(__xdata uint16_t o)
{
	ua_c8 = u_buf[o];
	if (ua_c8 == ' ' || ua_c8 == '\n' || !ua_c8)
		return 1;
	return 0;
}

/* The word at u_buf + o: is it s? */
static uint8_t ua_at(__xdata uint16_t o, __code const char *__xdata s)
{
	for (u_j = 0; s[u_j]; u_j++)
		if (u_buf[o + u_j] != s[u_j])
			return 0;
	return ua_ended(o + u_j);
}

/* The end of the line at o: its newline or the end of its region */
static uint16_t ua_eolat(__xdata uint16_t o)
{
	while (u_buf[o] != '\n' && u_buf[o])
		o++;
	return o;
}

/* The end of the word at o */
static uint16_t ua_wend(__xdata uint16_t o)
{
	while (!ua_ended(o))
		o++;
	return o;
}

/* Past the word at o and the spaces after it */
static uint16_t ua_skip(__xdata uint16_t o)
{
	o = ua_wend(o);
	while (u_buf[o] == ' ')
		o++;
	return o;
}

/* Are the words at a and b the same? */
static uint8_t ua_weq(__xdata uint16_t a, __xdata uint16_t b)
{
	while (u_buf[a] == u_buf[b] && !ua_ended(a)) {
		a++;
		b++;
	}
	if (!ua_ended(a))
		return 0;
	return ua_ended(b);
}

/* The number at o, 0 when there is none */
static uint16_t ua_numat(__xdata uint16_t o)
{
	for (ua_nv = 0; u_buf[o] >= '0' && u_buf[o] <= '9' && ua_nv < 6553; o++)
		ua_nv = ua_nv * 10 + u_buf[o] - '0';
	return ua_nv;
}

/* Copies [from, to) to unifi.pos */
static void ua_put(__xdata uint16_t from, __xdata uint16_t to)
{
	for (; from < to; from++)
		u_c(u_buf[from]);
}

/* Copies the line at o with its newline */
static void ua_copy(__xdata uint16_t o)
{
	ua_put(o, ua_eolat(o));
	ua_eol();
}

/* Is o a word that sets the link: on, off, auto, a speed or duplex? A
 * port has one such setting, the last line's. */
static uint8_t ua_link_word(__xdata uint16_t o)
{
	if (ua_at(o, "on") || ua_at(o, "off") || ua_at(o, "auto") || ua_at(o, "duplex"))
		return 1;
	for (ua_w = 1; ua_w < 7; ua_w++)
		if (ua_at(o, ua_speeds[ua_w]))
			return 1;
	return 0;
}

/* Front port p: copper? */
static uint8_t ua_copper(__xdata uint8_t p)
{
	if (p >= UA_NP || !ua_asic[p])
		return 0;
	if (machine.is_sfp[ua_asic[p] - 1])
		return 0;
	return 1;
}

/* Front port p: the uplink (the port towards the gateway, else the
 * controller, as the last inform reported it), or in its LAG? */
static uint8_t ua_uplink(__xdata uint8_t p)
{
	ua_t = ua_upsrc;
	if (p >= UA_NP || !ua_t || !ua_asic[p])
		return 0;
	if (ua_asic[p] == ua_t)
		return 1;
	ua_lg = port_lag_of(ua_t - 1);
	if (ua_lg == PORT_LAG_NONE || port_lag_of(ua_asic[p] - 1) != ua_lg)
		return 0;
	return 1;
}

/* Is VLAN v left alone: the management VLAN or the uplink's native one? */
static uint8_t ua_kept_vlan(__xdata uint16_t v)
{
	if (v == management_vlan || v == ua_upvid)
		return 1;
	return 0;
}

/* The area a config.txt line belongs to, 0 for none of them */
/* Past the spaces at o: the parser takes a line that starts with them */
static uint16_t ua_lead(__xdata uint16_t o)
{
	while (u_buf[o] == ' ')
		o++;
	return o;
}

static uint8_t ua_owner(__xdata uint16_t o)
{
	o = ua_lead(o);
	ua_b = ua_skip(o);	/* the second word */
	if (ua_at(o, "vlan")) {
		if (ua_kept_vlan(ua_numat(ua_b)) || ua_at(ua_skip(ua_b), "mgmt"))
			return 0;
		return UA_VLANS;
	}
	if (ua_at(o, "pvid"))
		return ua_uplink(ua_numat(ua_b)) ? 0 : UA_VLANS;
	if (ua_at(o, "eee"))	/* eee on|off <port> */
		return ua_copper(ua_numat(ua_skip(ua_b))) ? UA_PORTS : 0;
	if (ua_at(o, "port")) {
		ua_p = ua_numat(ua_b);
		ua_b = ua_skip(ua_b);
		/* SFP ports: "port N" sets neither link nor flow control there */
		if (ua_copper(ua_p) && (ua_link_word(ua_b) || ua_at(ua_b, "fc")))
			return UA_PORTS;
		return 0;
	}
	if (ua_at(o, "lag"))
		return UA_LAG;
	if (ua_at(o, "stp")) {
		if (ua_at(ua_b, "on") || ua_at(ua_b, "off") || ua_at(ua_b, "prio") || ua_at(ua_b, "version"))
			return UA_STP;
		return 0;
	}
	if (ua_at(o, "igmp"))
		return UA_IGMP;
	if (ua_at(o, "mtu"))
		return UA_JUMBO;
	if (ua_at(o, "mirror"))
		return UA_MIRROR;
	if (ua_at(o, "unifi") && ua_at(ua_b, "apply"))
		return UA_SELF;
	return 0;
}

/* Do the lines at o and y set the same thing? */
static uint8_t ua_same(__xdata uint16_t o, __xdata uint16_t y)
{
	o = ua_lead(o);
	if (!ua_weq(o, y))
		return 0;
	if (ua_at(o, "igmp") || ua_at(o, "mirror"))
		return 1;
	ua_sa = ua_skip(o);
	ua_sb = ua_skip(y);
	if (ua_at(o, "eee"))	/* the port */
		return ua_weq(ua_skip(ua_sa), ua_skip(ua_sb));
	if (ua_at(o, "stp") && (ua_at(ua_sa, "on") || ua_at(ua_sa, "off")))
		return ua_at(ua_sb, "on") | ua_at(ua_sb, "off");
	if (!ua_weq(ua_sa, ua_sb))
		return 0;	/* the VLAN, port, group, or stp setting */
	if (!ua_at(o, "port"))
		return 1;
	ua_t = ua_link_word(ua_skip(ua_sa));	/* a link setting, or fc */
	if (ua_t != ua_link_word(ua_skip(ua_sb)))
		return 0;
	return 1;
}

/* The controller's line in the new config.txt that sets what the line at o
 * does, or 0 */
static uint16_t ua_find(__xdata uint16_t o)
{
	for (ua_u = ua_gen; ua_u < ua_new_end; ua_u = ua_eolat(ua_u) + 1)
		if (ua_same(o, ua_u))
			return ua_u;
	return 0;
}

/* Is the line at o, up to its end, one of the lines in [from, to)? */
static uint8_t ua_has(__xdata uint16_t o, __xdata uint16_t from, __xdata uint16_t to)
{
	ua_hb = ua_eolat(o);
	o = ua_lead(o);	/* from the first word on */
	for (ua_ha = from; ua_ha < to; ua_ha = ua_eolat(ua_ha) + 1) {
		ua_hy = o;
		ua_hz = ua_lead(ua_ha);
		while (ua_hy < ua_hb && u_buf[ua_hz] == u_buf[ua_hy]) {
			ua_hy++;
			ua_hz++;
		}
		if (ua_hy != ua_hb)
			continue;
		ua_c8 = u_buf[ua_hz];
		if (ua_c8 == '\n' || !ua_c8)
			return 1;
	}
	return 0;
}

static void ua_port_word(__xdata uint8_t p)
{
	u_s("port ");
	u_dec(p);
	u_c(' ');
}

/* The mode of the port ua_bit in VLAN slot ua_s: its own line, else the
 * slot's. A leaf on statics, so the compiler spills nothing. */
static __xdata uint8_t ua_s;

static uint8_t ua_mode(void)
{
	if (!(ua_vset[ua_s] & ua_bit))
		return ua_vdef[ua_s];
	if (ua_vun[ua_s] & ua_bit)
		return UA_V_UNTAGGED;
	if (ua_vtag[ua_s] & ua_bit)
		return UA_V_TAGGED;
	return UA_V_EXCLUDE;
}

/* The name config.txt gives VLAN ua_vid[ua_k] last, copied after a space */
static void ua_vlan_name(void)
{
	ua_nm = 0;
	for (ua_y = UA_OLD; ua_y < ua_old_end; ua_y = ua_eolat(ua_y) + 1) {
		if (!ua_at(ua_y, "vlan"))
			continue;
		ua_z = ua_skip(ua_y);
		if (ua_numat(ua_z) != ua_vid[ua_k])
			continue;
		ua_z = ua_skip(ua_z);
		ua_c8 = u_buf[ua_z] | 0x20;
		if (ua_c8 < 'a' || ua_c8 > 'z' || ua_at(ua_z, "mgmt") || ua_at(ua_z, "d"))
			continue;
		ua_nm = ua_z;
	}
	if (!ua_nm || (!ua_vm && u_buf[ua_nm] == 'd'))
		return;	/* "vlan <vid> d..." alone deletes the VLAN */
	u_c(' ');
	ua_put(ua_nm, ua_wend(ua_nm));
}

/* The lines of one area, as the controller wants it */
static void ua_gen_area(__xdata uint8_t area)
{
	if (area == UA_PORTS) {
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!ua_copper(ua_k) || !ua_room())
				continue;	/* SFP ports: see ua_owner() */
			/* One link setting: off wins over a forced speed */
			if (ua_pf[ua_k] & UA_P_OFF) {
				ua_port_word(ua_k);
				u_s("off");
				ua_eol();
			} else if ((ua_pf[ua_k] & UA_P_FORCED) && ua_speed[ua_k]) {
				ua_port_word(ua_k);
				u_s(ua_speeds[ua_speed[ua_k]]);
				if (ua_speed[ua_k] <= 2) {	/* only 10M and 100M have half duplex */
					if (ua_pf[ua_k] & UA_P_FULL)
						u_s(" full");
					else
						u_s(" half");
				}
				ua_eol();
			}
			if (ua_pf[ua_k] & UA_P_EEE) {
				if (ua_pf[ua_k] & UA_P_EEE_ON)
					u_s("eee on ");
				else
					u_s("eee off ");
				u_dec(ua_k);
				ua_eol();
			}
			if (ua_pf[ua_k] & UA_P_FC) {
				ua_port_word(ua_k);
				if (ua_pf[ua_k] & UA_P_FC_ON)
					u_s("fc on");
				else
					u_s("fc off");
				ua_eol();
			}
		}
		return;
	}
	if (area == UA_VLANS) {
		for (ua_k = 0; ua_k < UA_NV; ua_k++) {
			if (!ua_vid[ua_k] || ua_kept_vlan(ua_vid[ua_k]) || !ua_room())
				continue;
			u_s("vlan ");
			u_dec(ua_vid[ua_k]);
			ua_vm = 0;	/* any members */
			ua_s = ua_k;
			for (ua_q = 1; ua_q < UA_NP; ua_q++) {
				ua_bit = (uint16_t)1 << ua_q;
				ua_m = ua_mode();
				if (ua_asic[ua_q] && (ua_m == UA_V_UNTAGGED || ua_m == UA_V_TAGGED))
					ua_vm = 1;
			}
			ua_vlan_name();
			for (ua_q = 1; ua_q < UA_NP; ua_q++) {
				if (!ua_asic[ua_q])
					continue;
				ua_bit = (uint16_t)1 << ua_q;
				ua_s = ua_k;
				ua_m = ua_mode();
				if (ua_m == UA_V_UNTAGGED || ua_m == UA_V_TAGGED) {
					u_c(' ');
					u_dec(ua_q);
					if (ua_m == UA_V_TAGGED)
						u_c('t');
				}
			}
			ua_eol();
		}
		/* The native VLAN: as given, else the slot the port is untagged in */
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!ua_asic[ua_k] || ua_uplink(ua_k) || !ua_room())
				continue;
			ua_n = ua_pvid[ua_k];
			ua_bit = (uint16_t)1 << ua_k;
			for (ua_s = 0; !ua_n && ua_s < UA_NV; ua_s++)
				if (ua_vid[ua_s] && ua_mode() == UA_V_UNTAGGED)
					ua_n = ua_vid[ua_s];
			if (!ua_n)
				ua_n = 1;
			u_s("pvid ");
			u_dec(ua_k);
			u_c(' ');
			u_dec(ua_n);
			ua_eol();
		}
		return;
	}
	if (area == UA_LAG) {
		/* A group per controller LAG id, written with its first member.
		 * The RTL836x's groups are fixed: the members' half of the ports. */
		ua_gu = 0;	/* groups used, a bit each */
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!(ua_pf[ua_k] & UA_P_AGG) || !ua_lag[ua_k] || !ua_asic[ua_k])
				continue;
			for (ua_q = 1; ua_q < ua_k; ua_q++)
				if ((ua_pf[ua_q] & UA_P_AGG) && ua_lag[ua_q] == ua_lag[ua_k] && ua_asic[ua_q])
					break;
			if (ua_q < ua_k || !ua_room())
				continue;
#if SOC_FAMILY == RTL836X
			ua_g = (ua_asic[ua_k] - 1) >> 2;
			ua_m = ua_g + 1;	/* the group's bit */
#else
			for (ua_g = 0, ua_m = 1; ua_gu & ua_m; ua_g++)
				ua_m <<= 1;
#endif
			if (ua_g >= UA_LAGS || (ua_gu & ua_m))
				continue;
			ua_gu |= ua_m;
			u_s("lag ");
			u_dec(ua_g + 1);
			u_s(" lacp");
			for (ua_q = ua_k; ua_q < UA_NP; ua_q++) {
				if ((ua_pf[ua_q] & UA_P_AGG) && ua_lag[ua_q] == ua_lag[ua_k] && ua_asic[ua_q]) {
					u_c(' ');
					u_dec(ua_q);
				}
			}
			ua_eol();
		}
		return;
	}
	if (area == UA_STP) {
		if (!ua_room())
			return;
		if (ua_stp == 2)
			u_s("stp on\n");
		else if (ua_stp == 1)
			u_s("stp off\n");
		if (ua_stpver == 2)
			u_s("stp version rstp\n");
		else if (ua_stpver == 1)
			u_s("stp version stp\n");
		if (ua_prio != 0xffff) {
			u_s("stp prio ");
			u_dec(ua_prio >> 12);	/* in steps of 4096 */
			ua_eol();
		}
		return;
	}
	if (area == UA_IGMP) {
		if (!ua_room())
			return;
		for (ua_k = 0; ua_k < UA_NV; ua_k++)
			if (ua_vid[ua_k] && ua_vigmp[ua_k])
				break;
		if (ua_k < UA_NV)
			u_s("igmp on\n");
		else
			u_s("igmp off\n");
		return;
	}
	if (area == UA_JUMBO) {
		if (ua_jumbo != 2)
			return;
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!ua_asic[ua_k] || !ua_room())
				continue;
			u_s("mtu ");
			u_dec(ua_k);
			u_s(" 9216\n");
		}
		return;
	}
	if (area == UA_MIRROR) {
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!ua_mir[ua_k] || ua_mir[ua_k] == 0xff || !ua_asic[ua_k] || !ua_asic[ua_mir[ua_k]]
			    || ua_mir[ua_k] == ua_k || !ua_room())
				continue;
			u_s("mirror ");
			u_dec(ua_k);
			u_c(' ');
			u_dec(ua_mir[ua_k]);
			ua_eol();
			return;	/* one session */
		}
	}
}

/* The commands for an old line at o that the new config.txt no longer has:
 * the controller's line that sets the same, run again when it is an old
 * line too (a later old line changed it), else back to the default */
static void ua_undo(__xdata uint16_t o)
{
	if (!ua_room())
		return;
	o = ua_lead(o);
	ua_y = ua_find(o);
	if (ua_y) {
		if (ua_has(ua_y, UA_OLD, ua_old_end))
			ua_copy(ua_y);
		return;	/* else it runs with the new lines */
	}
	ua_b = ua_skip(o);	/* the second word */
	ua_a = ua_wend(ua_b);	/* "vlan 10", "port 3" and the like end here */
	if (ua_at(o, "eee")) {
		u_s(MACHINE_DEFAULT_EEE ? "eee on " : "eee off ");
		ua_b = ua_skip(ua_b);
		ua_put(ua_b, ua_wend(ua_b));
	} else if (ua_at(o, "stp")) {
		if (ua_at(ua_b, "prio"))
			u_s("stp prio 8");
		else if (ua_at(ua_b, "version"))
			u_s("stp version rstp");
		else
			return;	/* on or off is always given with the others */
	} else if (ua_at(o, "mirror")) {
		u_s("mirror off");
	} else if (ua_at(o, "igmp")) {
		return;	/* always given */
	} else {
		ua_put(o, ua_a);
		if (ua_at(o, "vlan") || ua_at(o, "lag"))
			u_s(" d");
		else if (ua_at(o, "mtu"))
			u_s(" 1522");
		else if (ua_at(o, "pvid"))
			u_s(" 1");
		else if (ua_at(ua_skip(ua_b), "fc"))
			u_s(" fc on");
		else
			u_s(" auto");
	}
	ua_eol();
}

/* Can every controller LAG be a group? The RTL836x's groups are fixed to
 * ports 0-3 and 4-7. If not, the LAGs stay as they are. */
static uint8_t ua_lag_ok(void)
{
#if SOC_FAMILY == RTL836X
	for (ua_k = 1; ua_k < UA_NP; ua_k++) {
		if (!(ua_pf[ua_k] & UA_P_AGG) || !ua_lag[ua_k] || !ua_asic[ua_k])
			continue;
		for (ua_q = 1; ua_q < UA_NP; ua_q++)
			if ((ua_pf[ua_q] & UA_P_AGG) && ua_lag[ua_q] == ua_lag[ua_k] && ua_asic[ua_q]
			    && ((ua_asic[ua_q] - 1) >> 2) != ((ua_asic[ua_k] - 1) >> 2)) {
				print_string("UniFi: a LAG across both port groups: LAGs not applied\n");
				return 0;
			}
	}
#endif
	return 1;
}

extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;

/* Reads config.txt into [UA_OLD, ua_old_end) */
static void ua_read_config(void)
{
	for (ua_a = 0; ua_a < CONFIG_LEN; ua_a += 256) {
		flash_region.addr = CONFIG_START + ua_a;
		flash_region.len = 256;
		flash_read_bulk(u_buf + UA_OLD + ua_a);
	}
	for (ua_old_end = UA_OLD; ua_old_end < UA_OLD + CONFIG_LEN - 1; ua_old_end++)
		if (!u_buf[ua_old_end] || u_buf[ua_old_end] == 0xff)
			break;
	u_buf[ua_old_end] = 0;
	for (ua_sum = 0, ua_a = UA_OLD; ua_a < ua_old_end; ua_a++)
		ua_sum = (ua_sum << 1 | ua_sum >> 15) + u_buf[ua_a];
	/* A last line without a newline gets one, so lines compare alike */
	if (ua_old_end > UA_OLD && u_buf[ua_old_end - 1] != '\n' && ua_old_end < UA_OLD + CONFIG_LEN - 1) {
		u_buf[ua_old_end++] = '\n';
		u_buf[ua_old_end] = 0;
	}
}

/* After ua_parse(): the new config.txt and the commands. Returns 1 when
 * there is something to run or to write. */
static uint8_t ua_plan(void)
{
	ua_changed = 0;
	ua_over = 0;
	ua_eff = ua_mask & ua_seen;
	if (ua_vover) {
		/* Some VLANs are missing: IGMP, which follows them, waits too */
		ua_eff &= ~UA_IGMP;
		if (ua_eff & UA_VLANS)
			ua_over = 1;
	}
	if (!ua_lag_ok())
		ua_eff &= ~UA_LAG;
	ua_read_config();
	ua_osum = ua_sum;

	/* The uplink: the gateway's port, else the one this reply came in on,
	 * whichever is a front port. Its native VLAN: its last pvid line,
	 * else 1. */
	ua_upvid = 0;
	ua_upsrc = u_gw;
	if (!ua_upsrc)
		ua_upsrc = u_uplink;
again:
	for (ua_k = 1; ua_k < UA_NP && !ua_upvid; ua_k++) {
		if (!ua_uplink(ua_k))
			continue;
		ua_upvid = 1;
		for (ua_x = UA_OLD; ua_x < ua_old_end; ua_x = ua_eolat(ua_x) + 1) {
			ua_y = ua_lead(ua_x);
			if (!ua_at(ua_y, "pvid"))
				continue;
			ua_y = ua_skip(ua_y);
			if (ua_numat(ua_y) == ua_k)
				ua_upvid = ua_numat(ua_skip(ua_y));
		}
	}
	if (!ua_upvid && ua_upsrc != u_uplink) {
		ua_upsrc = u_uplink;
		goto again;
	}
	ua_defer = 0;
	if (!ua_upvid && (ua_eff & UA_VLANS)) {
		/* No uplink on a front port: its VLAN cannot be kept, so the
		 * VLANs wait, and the controller sends them again */
		print_string("UniFi: no uplink known, VLANs not applied\n");
		ua_eff &= ~UA_VLANS;
		ua_defer = 1;
	}

	/* The new config.txt: the old lines of the areas not applied, the
	 * selection, then the controller's lines of the areas applied */
	unifi.pos = UA_NEW;
	ua_lim = UA_NEW + CONFIG_LEN - 1;
	for (ua_x = UA_OLD; ua_x < ua_old_end; ua_x = ua_eolat(ua_x) + 1) {
		if (ua_owner(ua_x) & (ua_eff | UA_SELF))
			continue;
		if (!ua_room())
			break;
		ua_copy(ua_x);
	}
	ua_sel = unifi.pos;
	if (ua_room()) {
		u_s("unifi apply");
		for (ua_k = 0; ua_k < UA_AREAS; ua_k++) {
			if (ua_mask >> ua_k & 1) {
				u_c(' ');
				u_s(ua_names[ua_k]);
			}
		}
		ua_eol();
		u_buf[unifi.pos] = 0;
		if (!ua_has(ua_sel, UA_OLD, ua_old_end))
			ua_changed = 1;	/* config.txt gets the new selection */
	}
	ua_gen = unifi.pos;
	for (ua_area = 1; ua_area <= UA_MIRROR; ua_area <<= 1)
		if (ua_area & ua_eff)
			ua_gen_area(ua_area);
	ua_new_end = unifi.pos;
	if (ua_new_end > UA_NEW + CONFIG_LEN - 1)
		ua_over = 1;	/* never past the config.txt sector */
	u_buf[ua_new_end] = 0;

	/* The commands: undo the old lines that went, then the new lines that
	 * were not there */
	unifi.pos = UA_EXE;
	ua_lim = UA_SAVE - 1;
	for (ua_x = UA_OLD; ua_x < ua_old_end; ua_x = ua_eolat(ua_x) + 1) {
		if (!(ua_owner(ua_x) & ua_eff) || ua_has(ua_x, ua_gen, ua_new_end))
			continue;
		ua_changed = 1;
		ua_undo(ua_x);
	}
	for (ua_x = ua_gen; ua_x < ua_new_end; ua_x = ua_eolat(ua_x) + 1) {
		if (ua_has(ua_x, UA_OLD, ua_old_end))
			continue;
		ua_changed = 1;
		if (!ua_room())
			break;
		ua_copy(ua_x);
	}
	if (unifi.pos >= UA_SAVE)
		ua_over = 1;
	u_buf[unifi.pos] = 0;
	if (ua_over) {
		print_string("UniFi: the controller's settings do not fit, nothing applied\n");
		return 0;
	}
	ua_x = UA_EXE;
	return ua_changed;
}

/* Lines of the applied areas in the command history would win over the
 * new config.txt at the next Save to flash: they are blanked */
static void ua_purge(void)
{
	ua_px = (cmd_history_ptr + 1) & CMD_HISTORY_MASK;	/* the oldest byte */
	for (ua_z = 0; ua_z < CMD_HISTORY_SIZE; ua_z++) {
		ua_pb = ua_px;	/* the line's start */
		for (ua_y = 0; ua_z < CMD_HISTORY_SIZE && ua_y < CMD_BUF_SIZE
		     && cmd_history[ua_px] && cmd_history[ua_px] != '\n'; ua_y++, ua_z++) {
			u_buf[UA_OLD + ua_y] = cmd_history[ua_px];
			ua_px = (ua_px + 1) & CMD_HISTORY_MASK;
		}
		u_buf[UA_OLD + ua_y] = 0;
		if (ua_y && (ua_owner(UA_OLD) & ua_eff)) {
			for (; ua_y; ua_y--) {
				cmd_history[ua_pb] = ' ';
				ua_pb = (ua_pb + 1) & CMD_HISTORY_MASK;
			}
		}
		ua_px = (ua_px + 1) & CMD_HISTORY_MASK;
	}
}

/* One command per tick. Returns 1 when all ran, config.txt is written and
 * the cfgversion is reported. */
static uint8_t ua_run(void)
{
	if (u_buf[ua_x]) {
		ua_a = ua_eolat(ua_x);
		if (ua_a - ua_x < CMD_BUF_SIZE) {
			memcpy(u_buf + UA_SAVE, cmd_buffer, CMD_BUF_SIZE);
			memcpy(cmd_buffer, u_buf + ua_x, ua_a - ua_x);
			cmd_buffer[ua_a - ua_x] = 0;
			print_string("UniFi apply: ");
			print_string_x(cmd_buffer);
			write_char('\n');
			ua_q = save_cmd;
			save_cmd = 0;	/* config.txt is written below, not by Save */
			cmd_tokenize();
			if (err_status == ERR_OK)
				cmd_parser();
			save_cmd = ua_q;
			memcpy(cmd_buffer, u_buf + UA_SAVE, CMD_BUF_SIZE);
		}
		ua_x = ua_a + 1;
		return 0;
	}
	/* A Save to flash while it ran: its config.txt stays, and the
	 * controller sends the settings again for a new plan */
	ua_read_config();
	if (ua_sum != ua_osum) {
		print_string("UniFi: config.txt changed meanwhile, the controller will send it again\n");
		return 1;
	}
	/* The new config.txt, NUL-terminated as execute_config() reads it */
	flash_region.addr = CONFIG_START;
	flash_sector_erase();
	flash_region.addr = CONFIG_START;
	flash_region.len = ua_new_end - UA_NEW + 1;
	flash_write_bytes(u_buf + UA_NEW);
	/* Read back: only a config.txt that holds reports the cfgversion */
	ua_b = ua_new_end - UA_NEW + 1;
	for (ua_a = 0; ua_a < ua_b; ua_a += 256) {
		flash_region.addr = CONFIG_START + ua_a;
		flash_region.len = 256;
		flash_read_bulk(u_buf + UA_OLD + ua_a);
	}
	if (memcmp(u_buf + UA_OLD, u_buf + UA_NEW, ua_b)) {
		print_string("UniFi: config.txt did not write, the controller will send it again\n");
		return 1;
	}
	print_string("UniFi: config.txt written\n");
	memcpy(unifi.cfgver, ua_cfgver, UNIFI_CFGVER_LEN);
	u_save();
	return 1;
}

/* A system_cfg arrived with a new cfgversion: plan, and run from the next
 * tick on. Until all ran, the saved cfgversion is none: an apply that a
 * reboot or "unifi off" interrupts gets the settings again. */
static void ua_start(void)
{
	ua_parse();
	ua_busy = 0;
	ua_defer = 0;
	if (ua_mask & ua_seen) {
		ua_busy = ua_plan();
		/* Now, while the plan is made: edits made later stay */
		if (!ua_over)
			ua_purge();
	}
	if (ua_busy) {
		memcpy(ua_cfgver, unifi.cfgver, UNIFI_CFGVER_LEN);
		unifi.cfgver[0] = 0;
	}
	if (ua_defer)
		ua_cfgver[0] = unifi.cfgver[0] = 0;	/* the VLANs come again */
}

/* "unifi apply <area>..." | all | none */
static void ua_set(void)
{
	ua_g = 0;
	for (ua_m = 2; ua_m < cmd_words_len; ua_m++) {
		u_p = &cmd_buffer[cmd_words_b[ua_m]];
		if (strstart(u_p, "all") && (!u_p[3] || u_p[3] == ' ')) {
			ua_g = UA_ALL;
			continue;
		}
		if (strstart(u_p, "none") && (!u_p[4] || u_p[4] == ' '))
			continue;
		for (ua_k = 0; ua_k < UA_AREAS; ua_k++) {
			ua_q = strlen(ua_names[ua_k]);
			if (strstart(u_p, ua_names[ua_k]) && (!u_p[ua_q] || u_p[ua_q] == ' '))
				break;
		}
		if (ua_k == UA_AREAS) {
			u_error("unifi apply all | none | vlans ports lag stp igmp jumbo mirror\n");
			return;
		}
		ua_g |= 1 << ua_k;
	}
	if (ua_g == ua_mask)
		return;
	ua_mask = ua_g;
	/* An apply running now reports no cfgversion when it is done */
	ua_cfgver[0] = 0;
	/* At run time, not while config.txt replays: forget the cfgversion, so
	 * the controller sends its settings again for the new areas */
	if (save_cmd && unifi.adopted) {
		unifi.cfgver[0] = 0;
		unifi.unsaved = 1;
		unifi.next = uptime_secs;
	}
}

/* "unifi" status: the areas applied */
static void ua_show(void)
{
	print_string("Applies: ");
	if (!ua_mask)
		print_string("nothing");
	for (ua_k = 0; ua_k < UA_AREAS; ua_k++) {
		if (ua_mask >> ua_k & 1) {
			print_string(ua_names[ua_k]);
			write_char(' ');
		}
	}
	write_char('\n');
}

/* The capabilities for the inform: what is applied, so the controller
 * offers and sends only that */
static __xdata uint32_t ua_cap;

static void ua_caps(void)
{
	if (!ua_imask)
		return;	/* as before apply existed */
	u_s(",\"switch_caps\":{\"feature_caps\":");
	ua_cap = 0;
	if (ua_imask & UA_STP)
		ua_cap |= 64;
	if (ua_imask & UA_IGMP)
		ua_cap |= 256;
	if (ua_imask & UA_LAG)
		ua_cap |= 8192;
	if (ua_imask & UA_JUMBO)
		ua_cap |= 524288;
	u_dec(ua_cap);
	u_s(",\"max_aggregate_sessions\":");
	u_c(ua_imask & UA_LAG ? '0' + UA_LAGS : '0');
	u_s(",\"max_mirror_sessions\":");
	u_c(ua_imask & UA_MIRROR ? '1' : '0');
	u_c('}');
}
