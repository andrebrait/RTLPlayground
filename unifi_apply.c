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
 * Left alone: the management VLAN (the switch must stay reachable), port
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

#define UA_V_UNTAGGED	1
#define UA_V_TAGGED	2
#define UA_V_EXCLUDE	3

static __xdata uint8_t ua_seen;		/* the areas system_cfg describes */
static __xdata uint8_t ua_pf[UA_NP];	/* UA_P_* */
static __xdata uint8_t ua_speed[UA_NP];	/* index into ua_speeds, 0 = none */
static __xdata uint8_t ua_lag[UA_NP];	/* the controller's LAG id */
static __xdata uint8_t ua_mir[UA_NP];	/* opmode=mirror: the port it mirrors, 0xff unknown */
static __xdata uint16_t ua_pvid[UA_NP];	/* 0 = not given */
static __xdata uint8_t ua_asic[UA_NP];	/* front port to ASIC port, 0 = none */
static __xdata uint16_t ua_vid[UA_NV];	/* 0 = no such slot */
static __xdata uint8_t ua_vdef[UA_NV];	/* the slot's mode, UA_V_* */
static __xdata uint8_t ua_vigmp[UA_NV];
static __xdata uint16_t ua_vset[UA_NV], ua_vun[UA_NV], ua_vtag[UA_NV];	/* per-port lines, bit = front port */
static __xdata uint8_t ua_stp, ua_stpver, ua_jumbo;	/* 0 not given, 1 off/stp, 2 on/rstp */
static __xdata uint16_t ua_prio;	/* 0xffff = not given */

static __code const char *const __code ua_speeds[] = {
	"", "10m", "100m", "1g", "2g5", "5g", "10g"
};

/* The parser: a line of system_cfg is [ua_i, ua_le) */
static __xdata uint16_t ua_i, ua_le, ua_n;
static __xdata uint8_t ua_k, ua_q;

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
	if (!ua_lit(s))
		return 0;
	if (ua_i != ua_le)
		return 0;
	return 1;
}

/* Decimal digits into ua_n; 0 when there are none or too many */
static uint8_t ua_num(void)
{
	ua_n = 0;
	for (ua_q = 0; ua_i < ua_le && u_p[ua_i] >= '0' && u_p[ua_i] <= '9'; ua_i++) {
		if (++ua_q > 5)
			return 0;
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
	u_ls = ua_i;
	if (ua_val("status=disabled")) {
		ua_pf[ua_k] |= UA_P_OFF;
		return;
	}
	ua_i = u_ls;
	if (ua_val("autoneg=disabled")) {
		ua_pf[ua_k] |= UA_P_FORCED;
		return;
	}
	ua_i = u_ls;
	if (ua_val("duplex=enabled")) {
		ua_pf[ua_k] |= UA_P_FULL;
		return;
	}
	ua_i = u_ls;
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
		if (ua_n == 10)
			ua_speed[ua_k] = 1;
		else if (ua_n == 100)
			ua_speed[ua_k] = 2;
		else if (ua_n == 1000)
			ua_speed[ua_k] = 3;
		else if (ua_n == 2500)
			ua_speed[ua_k] = 4;
		else if (ua_n == 5000)
			ua_speed[ua_k] = 5;
		else if (ua_n == 10000)
			ua_speed[ua_k] = 6;
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
	if (!ua_num() || ua_n == 0 || ua_n > UA_NV || !ua_lit("."))
		return;	/* switch.vlan.status and the like */
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
	ua_seen = ua_stp = ua_stpver = ua_jumbo = 0;
	ua_prio = 0xffff;
	for (ua_k = MACHINE_MIN_PORT; ua_k <= MACHINE_MAX_PORT; ua_k++)
		if (machine.log_to_phys_port[ua_k] && machine.log_to_phys_port[ua_k] < UA_NP)
			ua_asic[machine.log_to_phys_port[ua_k]] = ua_k;
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
#define UA_LINE_MAX	100	/* room kept for the line being written */

static __xdata uint16_t ua_old_end, ua_new_end, ua_gen, ua_x, ua_y, ua_z, ua_lim, ua_a, ua_b;
static __xdata uint8_t ua_c8, ua_w, ua_m, ua_g, ua_area;
static __xdata uint16_t ua_bit;
static __xdata uint8_t ua_over;		/* a region overflowed: nothing is applied */
static __xdata uint8_t ua_changed;	/* the new config.txt differs */
static __xdata uint8_t ua_busy;		/* commands are running */

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

/* The line at u_buf + o: is it s, then a space or its end? */
static uint8_t ua_at(__xdata uint16_t o, __code const char *s)
{
	for (u_j = 0; s[u_j]; u_j++)
		if (u_buf[o + u_j] != s[u_j])
			return 0;
	ua_c8 = u_buf[o + u_j];
	if (ua_c8 == ' ' || ua_c8 == '\n' || !ua_c8)
		return 1;
	return 0;
}

/* The end of the line at o: its newline or the end of its region */
static uint16_t ua_eolat(__xdata uint16_t o)
{
	while (u_buf[o] != '\n' && u_buf[o])
		o++;
	return o;
}

/* Past the word at o and the spaces after it */
static uint16_t ua_skip(__xdata uint16_t o)
{
	while (u_buf[o] != ' ' && u_buf[o] != '\n' && u_buf[o])
		o++;
	while (u_buf[o] == ' ')
		o++;
	return o;
}

/* Is o a speed word: a port line that sets the link, not its name or LLDP? */
static uint8_t ua_link_word(__xdata uint16_t o)
{
	if (ua_at(o, "on") || ua_at(o, "off") || ua_at(o, "auto") || ua_at(o, "duplex"))
		return 1;
	for (ua_w = 1; ua_w < 7; ua_w++)
		if (ua_at(o, ua_speeds[ua_w]))
			return 1;
	return 0;
}

/* The area a config.txt line belongs to, 0 for none of them */
static uint8_t ua_owner(__xdata uint16_t o)
{
	if (ua_at(o, "vlan")) {
		ua_b = ua_skip(ua_skip(o));
		if (ua_at(ua_b, "mgmt"))
			return 0;
		return UA_VLANS;
	}
	if (ua_at(o, "pvid"))
		return UA_VLANS;
	if (ua_at(o, "eee"))
		return UA_PORTS;
	if (ua_at(o, "port")) {
		ua_b = ua_skip(ua_skip(o));
		if (ua_link_word(ua_b) || ua_at(ua_b, "fc"))
			return UA_PORTS;
		return 0;
	}
	if (ua_at(o, "lag"))
		return UA_LAG;
	if (ua_at(o, "stp")) {
		ua_b = ua_skip(o);
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
	return 0;
}

/* Is the line at o, up to its end, one of the lines in [from, to)? */
static uint8_t ua_has(__xdata uint16_t o, __xdata uint16_t from, __xdata uint16_t to)
{
	ua_b = ua_eolat(o);
	for (ua_a = from; ua_a < to; ua_a = ua_eolat(ua_a) + 1) {
		ua_y = o;
		ua_z = ua_a;
		while (ua_y < ua_b && u_buf[ua_z] == u_buf[ua_y]) {
			ua_y++;
			ua_z++;
		}
		if (ua_y != ua_b)
			continue;
		ua_c8 = u_buf[ua_z];
		if (ua_c8 == '\n' || !ua_c8)
			return 1;
	}
	return 0;
}

/* Does a line in [from, to) start with the first n bytes at o, then end or
 * go on with a space? */
static uint8_t ua_has_key(__xdata uint16_t o, __xdata uint16_t n, __xdata uint16_t from, __xdata uint16_t to)
{
	for (ua_z = from; ua_z < to; ua_z = ua_eolat(ua_z) + 1) {
		if (memcmp(u_buf + ua_z, u_buf + o, n))
			continue;
		ua_c8 = u_buf[ua_z + n];
		if (ua_c8 == ' ' || ua_c8 == '\n' || !ua_c8)
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

/* The lines of one area, as the controller wants it */
static void ua_gen_area(__xdata uint8_t area)
{
	if (area == UA_PORTS) {
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!ua_asic[ua_k] || !ua_room())
				continue;
			if ((ua_pf[ua_k] & UA_P_FORCED) && ua_speed[ua_k]) {
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
			if (ua_pf[ua_k] & UA_P_OFF) {
				ua_port_word(ua_k);
				u_s("off");
				ua_eol();
			}
			if (machine.is_sfp[ua_asic[ua_k]])
				continue;	/* no EEE or flow control on SFP ports */
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
			if (!ua_vid[ua_k] || ua_vid[ua_k] == management_vlan || !ua_room())
				continue;
			u_s("vlan ");
			u_dec(ua_vid[ua_k]);
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
			if (!ua_asic[ua_k] || !ua_room())
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
		/* The controller's LAG ids become groups 1..4 in order */
		ua_g = 0;	/* groups written */
		for (ua_k = 1; ua_k < UA_NP; ua_k++) {
			if (!(ua_pf[ua_k] & UA_P_AGG) || !ua_lag[ua_k] || !ua_asic[ua_k])
				continue;
			for (ua_q = 1; ua_q < ua_k; ua_q++)
				if ((ua_pf[ua_q] & UA_P_AGG) && ua_lag[ua_q] == ua_lag[ua_k] && ua_asic[ua_q])
					break;
			if (ua_q < ua_k || ua_g >= 4 || !ua_room())
				continue;	/* written with its first member */
			u_s("lag ");
			u_dec(++ua_g);
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

/* The command that undoes the old line at o, now that the new config.txt,
 * [UA_NEW, ua_new_end), no longer has it; nothing when a new line takes
 * its place anyway */
static void ua_undo(__xdata uint16_t o, __xdata uint8_t area)
{
	if (!ua_room())
		return;
	ua_b = ua_skip(o);	/* the second word */
	if (area == UA_VLANS && ua_at(o, "vlan")) {
		ua_a = ua_skip(ua_b);	/* the key is "vlan <id>" */
		while (ua_a > o && u_buf[ua_a - 1] == ' ')
			ua_a--;
		if (ua_has_key(o, ua_a - o, UA_NEW, ua_new_end))
			return;
		ua_n = 0;
		for (ua_a = ua_b; u_buf[ua_a] >= '0' && u_buf[ua_a] <= '9'; ua_a++)
			ua_n = ua_n * 10 + u_buf[ua_a] - '0';
		if (!ua_n || ua_n == management_vlan)
			return;
		u_s("vlan ");
		u_dec(ua_n);
		u_s(" d\n");
		return;
	}
	if (area == UA_PORTS && ua_at(o, "port")) {
		ua_a = ua_skip(ua_b);	/* the third word */
		ua_n = 0;	/* the port */
		for (ua_y = ua_b; u_buf[ua_y] >= '0' && u_buf[ua_y] <= '9'; ua_y++)
			ua_n = ua_n * 10 + u_buf[ua_y] - '0';
		if (ua_at(ua_a, "off")) {
			ua_port_word(ua_n);
			u_s("on\n");
			return;
		}
		if (ua_at(ua_a, "fc"))
			return;
		/* A link setting: back to auto, unless a new one replaces it */
		for (ua_y = UA_NEW; ua_y < ua_new_end; ua_y = ua_eolat(ua_y) + 1) {
			if (!ua_at(ua_y, "port"))
				continue;
			ua_z = ua_skip(ua_y);
			ua_m = 0;
			for (; u_buf[ua_z] >= '0' && u_buf[ua_z] <= '9'; ua_z++)
				ua_m = ua_m * 10 + u_buf[ua_z] - '0';
			if (ua_m != ua_n)
				continue;
			ua_z = ua_skip(ua_z);
			if (ua_link_word(ua_z) && !ua_at(ua_z, "off"))
				return;
		}
		ua_port_word(ua_n);
		u_s("auto\n");
		return;
	}
	if (area == UA_LAG) {
		ua_a = ua_skip(ua_b);
		while (ua_a > o && u_buf[ua_a - 1] == ' ')
			ua_a--;
		if (ua_has_key(o, ua_a - o, UA_NEW, ua_new_end))
			return;
		u_s("lag ");
		u_c(u_buf[ua_b]);
		u_s(" d\n");
		return;
	}
	if (area == UA_JUMBO) {
		ua_a = ua_skip(ua_b);
		while (ua_a > o && u_buf[ua_a - 1] == ' ')
			ua_a--;
		if (ua_has_key(o, ua_a - o, UA_NEW, ua_new_end))
			return;
		for (; o < ua_a; o++)
			u_c(u_buf[o]);
		u_s(" 1522\n");
		return;
	}
	if (area == UA_MIRROR) {
		if (ua_has_key(o, 6, UA_NEW, ua_new_end))
			return;
		u_s("mirror off\n");
		return;
	}
	if (area == UA_IGMP || area == UA_STP)
		return;	/* their new lines are explicit */
}

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
	ua_over = 0;
	ua_changed = 0;
	ua_read_config();

	/* The new config.txt: the old lines of the areas not applied, then the
	 * controller's lines of those that are */
	unifi.pos = UA_NEW;
	ua_lim = UA_NEW + CONFIG_LEN - 1;
	for (ua_x = UA_OLD; ua_x < ua_old_end; ua_x = ua_eolat(ua_x) + 1) {
		ua_k = ua_owner(ua_x);
		if (ua_k & ua_mask & ua_seen)
			continue;
		if (!ua_room())
			break;
		for (ua_a = ua_x; ua_a < ua_eolat(ua_x); ua_a++)
			u_c(u_buf[ua_a]);
		ua_eol();
	}
	ua_gen = unifi.pos;
	for (ua_area = 1; ua_area <= UA_MIRROR; ua_area <<= 1)
		if (ua_area & ua_mask & ua_seen)
			ua_gen_area(ua_area);
	ua_new_end = unifi.pos;
	u_buf[ua_new_end] = 0;

	/* The commands: undo the old lines that went, then the new lines that
	 * were not there */
	unifi.pos = UA_EXE;
	ua_lim = UA_SAVE - 1;
	for (ua_x = UA_OLD; ua_x < ua_old_end; ua_x = ua_eolat(ua_x) + 1) {
		ua_k = ua_owner(ua_x) & ua_mask & ua_seen;
		if (!ua_k || ua_has(ua_x, ua_gen, ua_new_end))
			continue;
		ua_changed = 1;
		ua_undo(ua_x, ua_k);
	}
	for (ua_x = ua_gen; ua_x < ua_new_end; ua_x = ua_eolat(ua_x) + 1) {
		if (ua_has(ua_x, UA_OLD, ua_old_end))
			continue;
		ua_changed = 1;
		if (!ua_room())
			break;
		for (ua_a = ua_x; ua_a < ua_eolat(ua_x); ua_a++)
			u_c(u_buf[ua_a]);
		ua_eol();
	}
	u_buf[unifi.pos] = 0;
	if (ua_over) {
		print_string("UniFi: the controller's settings do not fit, nothing applied\n");
		return 0;
	}
	ua_x = UA_EXE;
	return ua_changed;
}

/* One command per tick. Returns 1 when all ran and config.txt is written. */
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
	/* The new config.txt, NUL-terminated as execute_config() reads it */
	flash_region.addr = CONFIG_START;
	flash_sector_erase();
	flash_region.addr = CONFIG_START;
	flash_region.len = ua_new_end - UA_NEW + 1;
	flash_write_bytes(u_buf + UA_NEW);
	print_string("UniFi: config.txt written\n");
	return 1;
}

/* A system_cfg arrived with a new cfgversion: plan, and run from the next
 * tick on */
static void ua_start(void)
{
	ua_parse();
	for (ua_k = 0; ua_k < UA_AREAS; ua_k++) {
		if ((ua_seen >> ua_k & 1) && !(ua_mask >> ua_k & 1)) {
			print_string("UniFi: not applied: ");
			print_string(ua_names[ua_k]);
			write_char('\n');
		}
	}
	ua_busy = 0;
	if (ua_mask & ua_seen)
		ua_busy = ua_plan();
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
	if (!ua_mask)
		return;	/* as before apply existed */
	u_s(",\"switch_caps\":{\"feature_caps\":");
	ua_cap = 0;
	if (ua_mask & UA_STP)
		ua_cap |= 64;
	if (ua_mask & UA_IGMP)
		ua_cap |= 256;
	if (ua_mask & UA_LAG)
		ua_cap |= 8192;
	if (ua_mask & UA_JUMBO)
		ua_cap |= 524288;
	u_dec(ua_cap);
	u_s(",\"max_aggregate_sessions\":");
	u_c(ua_mask & UA_LAG ? '4' : '0');
	u_s(",\"max_mirror_sessions\":");
	u_c(ua_mask & UA_MIRROR ? '1' : '0');
	u_c('}');
}
