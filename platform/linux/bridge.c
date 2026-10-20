/*
 * The switch fabric as a Linux bridge: swbr (vlan_filtering 1, STP off, vlan_default_pvid 0) holds
 * every board port. The VLAN table of the 8051 (rtl837x_port.c) maps onto the bridge VLAN entries:
 *
 *   8051 VLAN table entry, bit per logical port    bridge VLAN entry of that port (member, untagged flag)
 *   CPU port (bit 9), always a tagged member       the bridge itself ("self" entry), tagged -- except the
 *                                                  management VLAN, where it is untagged + PVID so that
 *                                                  the management IP lives on swbr itself (mgmt.c)
 *   PVID register of a port                        the PVID flag of that port's entry for that VLAN
 *   ingress filter of a port                       see port_ingress_filter()
 *
 * The kernel is the table: every read is a RTM_GETLINK dump (AF_BRIDGE, RTEXT_FILTER_BRVLAN), cached
 * for 250 ms and dropped after each command, so one page that asks for 4094 VLANs costs one dump.
 * Only two things have no home in the bridge and are kept here: the PVID registers (below) and the
 * ingress modes. Every kernel call is checked: a failed change prints an error through cmd_error();
 * vlan_create() puts the entry back as it was.
 *
 * Start (bridge_init, bridge_commit): a bridge that does not exist is created and given the state
 * of a booting 8051 (vlan_setup, rtl837x_port.c:350): VLAN 1 with every port an untagged member,
 * PVID register 1 on every port, ingress mode `any`. A bridge that exists (a daemon restart, or /reset)
 * is ADOPTED without touching a VLAN or a PVID, so the traffic keeps flowing exactly as it did: the
 * PVID registers are read back from the kernel, the startup configuration then applies as a diff
 * (apply_vlan changes only what differs), and bridge_commit() removes the VLANs the configuration
 * did not create. A port found PARKED has no known register or mode: it keeps its parking VLAN
 * (register 0, mode `tagged`) until the configuration has set them, and at bridge_commit() a register
 * still 0 becomes 1 and a mode nobody set becomes `any` -- unparking it before would let its
 * untagged frames into VLAN 1 for as long as the startup configuration takes. VLAN 1 is the exception the 8051 makes: it exists at boot unless the configuration
 * deletes it, so a configuration that says nothing about it gets the boot state of VLAN 1.
 *
 * States of the 8051 that a bridge cannot hold, and what is done instead:
 *  - A port with NO PVID. The 8051 has such states: a PVID register naming a VLAN the port is not a
 *    member of (port_pvid_set() does not check, rtl837x_port.c:107; the chip's ingress filter then
 *    drops untagged frames), the ingress mode `allow tagged` (they are dropped, too), and a port
 *    whose PVID VLAN was deleted. A bridge port with no PVID drops untagged frames in the software
 *    bridge, but the hardware offload of the Realtek DSA driver passes them as VID 0 (OpenWrt issue
 *    25993, open). So no port is ever left without PVID: such a port is PARKED, i.e. it gets
 *    its own parking VLAN as PVID (untagged member, no other member, not a member of the bridge
 *    itself) and an untagged frame is dropped on any driver because nobody is there to receive it.
 *    The parking VLAN of logical port N is 4094 - N (park_vid()); every port has its own, because
 *    two parked ports in one VLAN would pass frames between them. RESERVED RANGE (for doc/linux.md):
 *    exactly one VID per board port, 4094 - N for each logical port N the board has (4089..4094 on
 *    the six-port bench board; a board without a logical port N does not reserve 4094 - N). The 8051
 *    parser allows them, on Linux vlan_create(), vlan_delete() and port_pvid_set() refuse them,
 *    vlan_entry_get() reports them as no VLAN, so /vlan.json, /vlanlist and `vlan show` never show
 *    one, and the daemon owns them: at start, any entry in a parking VID that is not that port's own
 *    (another port's, the bridge's own) is removed (clean_reserved()). The register of the port is kept; the
 *    port leaves the parking VLAN when it joins the VLAN the register names and goes back when
 *    that VLAN is deleted, before the entry is, so there is no moment without a PVID.
 *    A host can still send frames tagged with its own parking VID: they go nowhere either.
 *  - Ingress mode `allow untagged` (VLAN_UNTAGGED) would drop tagged frames; a bridge has no such
 *    per-port switch, so the request is refused (port_ingress_filter returns false). The 8051
 *    accepts and sets two bits per port (port_ingress_filter, rtl837x_port.c:69-88).
 *  - Ingress filtering on/off per port (vlan_dump's "Filtering", RTL837X_VLAN_PORT_IGR_FLTR) is always
 *    on: vlan_filtering is a property of the bridge and vlan_setup() (rtl837x_port.c:386) enables it
 *    on every port too.
 *  - The untagged bit of a non-member: the chip stores ~tagged for every port (vlan_create:
 *    `a = ~tagged`), a bridge entry exists only for members. vlan_entry_get() reports untagged =
 *    members that are untagged; the web UI masks the word with the members the same way (app.js:1481).
 */
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nl.h"
#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"

#define VID_MAX 4094
#define SNAP_MS 250

extern __xdata uint16_t management_vlan;
extern __xdata uint16_t vlan_ptr;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
extern const uint8_t * const hex;

/* the 8051 registers that have no bridge counterpart (see the top) */
static uint16_t pvid_reg[BOARD_PORTS];
static vlan_ingress_mode_t ingress_mode[BOARD_PORTS];
static bool mgmt_active = true;		/* false between the removal and the re-adding of the management entry */
/* what the startup configuration did, for bridge_commit() */
static bool created[VID_MAX + 1];
static bool vlan1_decided;
/* an adopted bridge shows a parked port's register and mode as unknown until the configuration has
 * said them (bridge_init); a register of 0 is "not known yet" */
static bool mode_unknown[BOARD_PORTS];

static struct {
	uint16_t member[VID_MAX + 1];	/* bit per logical port, bit CPU_PORT = the bridge itself */
	uint16_t untag[VID_MAX + 1];
	uint16_t pvid[CPU_PORT + 1];
	int64_t at;
	bool ok;
} snap;
static int snap_if[CPU_PORT + 1];	/* ifindex per logical port while a dump runs; CPU_PORT = swbr */

static bool on_board(uint8_t port)
{
	return port < BOARD_PORTS && board_ifnames[port];
}

static uint16_t park_vid(uint8_t port)
{
	return VID_MAX - port;
}

/* One VID per board port is reserved, see the top. */
static bool reserved(uint16_t vid)
{
	return vid <= VID_MAX && VID_MAX - vid < BOARD_PORTS && on_board(VID_MAX - vid);
}

bool bridge_vid_reserved(uint16_t vid)
{
	return reserved(vid);
}

static void report_port(const char *what, uint8_t port, int err)
{
	char w[100];

	snprintf(w, sizeof w, "%s on %s", what, port == CPU_PORT ? SWBR : board_ifnames[port]);
	nl_error(w, err);
}

static uint16_t mgmt_vid(void)
{
	return management_vlan ? management_vlan : 1;
}

/* The PVID the port should carry (0: none): the register, unless untagged frames are not allowed. */
static uint16_t wanted_pvid(uint8_t port)
{
	return ingress_mode[port] == VLAN_TAGGED ? 0 : pvid_reg[port];
}

/* The PVID the port has to have in the bridge: its register's VLAN if it is a member there, else its
 * parking VLAN. Needs a current snapshot. */
static uint16_t target_pvid(uint8_t port)
{
	uint16_t w = wanted_pvid(port);

	return w && snap.member[w] >> port & 1 ? w : park_vid(port);
}

static uint16_t board_mask(void)
{
	uint16_t m = 0;

	for (uint8_t p = 0; p < BOARD_PORTS; p++)
		if (on_board(p))
			m |= 1 << p;
	return m;
}

/* ---- the kernel's view ---- */

static int snap_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(nlh);
	struct nlattr *attr, *spec;
	int port = -1;

	for (int p = 0; p <= CPU_PORT; p++)
		if (snap_if[p] && snap_if[p] == ifi->ifi_index)
			port = p;
	if (port < 0)
		return MNL_CB_OK;
	mnl_attr_for_each(attr, nlh, sizeof *ifi) {
		if (mnl_attr_get_type(attr) != IFLA_AF_SPEC)
			continue;
		mnl_attr_for_each_nested(spec, attr) {
			const struct bridge_vlan_info *v;

			if (mnl_attr_get_type(spec) != IFLA_BRIDGE_VLAN_INFO || mnl_attr_get_payload_len(spec) < sizeof *v)
				continue;
			v = mnl_attr_get_payload(spec);
			if (!v->vid || v->vid > VID_MAX)
				continue;
			snap.member[v->vid] |= 1 << port;
			if (v->flags & BRIDGE_VLAN_INFO_UNTAGGED)
				snap.untag[v->vid] |= 1 << port;
			if (v->flags & BRIDGE_VLAN_INFO_PVID)
				snap.pvid[port] = v->vid;
		}
	}
	return MNL_CB_OK;
}

/* Reads the VLAN entries of the ports and of the bridge itself; 0 or -errno. */
static int snap_refresh(void)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;
	int rc;

	if (snap.ok && now_ms() - snap.at < SNAP_MS)
		return 0;
	snap.ok = false;
	memset(snap.member, 0, sizeof snap.member);
	memset(snap.untag, 0, sizeof snap.untag);
	memset(snap.pvid, 0, sizeof snap.pvid);
	for (int p = 0; p < BOARD_PORTS; p++)
		snap_if[p] = board_ifnames[p] ? if_nametoindex(board_ifnames[p]) : 0;
	snap_if[CPU_PORT] = if_nametoindex(SWBR);
	if (!snap_if[CPU_PORT])
		return -ENODEV;
	nlh = nl_msg(buf, RTM_GETLINK, 0);
	ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);
	ifi->ifi_family = AF_BRIDGE;
	mnl_attr_put_u32(nlh, IFLA_EXT_MASK, RTEXT_FILTER_BRVLAN);
	rc = nl_dump(nlh, snap_cb, NULL);
	if (rc)
		return rc;
	snap.at = now_ms();
	snap.ok = true;
	return 0;
}

/* The snapshot as the kernel now has it after a successful vlan_op. */
static void snap_patch(uint16_t type, uint8_t port, uint16_t flags, uint16_t vid)
{
	uint16_t bit = 1 << port;

	if (type == RTM_DELLINK) {
		snap.member[vid] &= ~bit;
		snap.untag[vid] &= ~bit;
		if (snap.pvid[port] == vid)
			snap.pvid[port] = 0;
		return;
	}
	snap.member[vid] |= bit;
	if (flags & BRIDGE_VLAN_INFO_UNTAGGED)
		snap.untag[vid] |= bit;
	else
		snap.untag[vid] &= ~bit;
	if (flags & BRIDGE_VLAN_INFO_PVID)
		snap.pvid[port] = vid;
	else if (snap.pvid[port] == vid)
		snap.pvid[port] = 0;
}

/* Adds (RTM_SETLINK: also changes the flags of an existing entry) or deletes (RTM_DELLINK) one VLAN
 * entry of a port, or of the bridge itself. Deleting what is not there is no error. */
static int vlan_op(uint16_t type, uint8_t port, uint16_t flags, uint16_t vid)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, type, 0);
	struct ifinfomsg *ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);
	struct bridge_vlan_info vi = { .flags = flags, .vid = vid };
	struct nlattr *spec;
	int rc;

	ifi->ifi_family = AF_BRIDGE;
	ifi->ifi_index = if_nametoindex(port == CPU_PORT ? SWBR : board_ifnames[port]);
	if (!ifi->ifi_index)
		return -ENODEV;
	spec = mnl_attr_nest_start(nlh, IFLA_AF_SPEC);
	if (port == CPU_PORT)
		mnl_attr_put_u16(nlh, IFLA_BRIDGE_FLAGS, BRIDGE_FLAGS_SELF);
	mnl_attr_put(nlh, IFLA_BRIDGE_VLAN_INFO, sizeof vi, &vi);
	mnl_attr_nest_end(nlh, spec);
	rc = nl_talk(nlh);
	if (type == RTM_DELLINK && rc == -ENOENT)
		rc = 0;
	if (rc)
		snap.ok = false;
	else if (snap.ok)
		snap_patch(type, port, flags, vid);
	return rc;
}

static uint16_t entry_flags(uint8_t port, uint16_t vid, uint16_t tagged)
{
	if (port == CPU_PORT)
		return mgmt_active && vid == mgmt_vid() ? BRIDGE_VLAN_INFO_UNTAGGED | BRIDGE_VLAN_INFO_PVID : 0;
	return (tagged >> port & 1 ? 0 : BRIDGE_VLAN_INFO_UNTAGGED) | (wanted_pvid(port) == vid ? BRIDGE_VLAN_INFO_PVID : 0);
}

/* Makes VLAN vid have exactly these members (bit per logical port, bit CPU_PORT the bridge itself),
 * `tagged` of them as tagged ones; no members deletes the VLAN. Reads the snapshot (call
 * snap_refresh first) and changes only what differs. A port loses its PVID VLAN only after it has
 * been parked. Returns 0 or -errno, *bad = the port at fault. */
static int apply_vlan(uint16_t vid, uint16_t members, uint16_t tagged, uint8_t *bad)
{
	for (uint8_t p = 0; p <= CPU_PORT; p++) {
		bool want = members >> p & 1, have = snap.member[vid] >> p & 1;
		uint16_t fl = entry_flags(p, vid, tagged);
		int rc = 0;

		if (p != CPU_PORT && !on_board(p))
			continue;
		if (want) {
			if (!have || (snap.untag[vid] >> p & 1) != !!(fl & BRIDGE_VLAN_INFO_UNTAGGED)
			    || ((fl & BRIDGE_VLAN_INFO_PVID) && snap.pvid[p] != vid))
				rc = vlan_op(RTM_SETLINK, p, fl, vid);
		} else if (have) {
			if (p != CPU_PORT && snap.pvid[p] == vid)
				rc = vlan_op(RTM_SETLINK, p, BRIDGE_VLAN_INFO_PVID | BRIDGE_VLAN_INFO_UNTAGGED, park_vid(p));
			if (!rc)
				rc = vlan_op(RTM_DELLINK, p, 0, vid);
		}
		if (rc) {
			*bad = p;
			return rc;
		}
	}
	return 0;
}

/* Brings a port to its target PVID (parking it or taking it out of its parking VLAN). */
static int sync_port(uint8_t port)
{
	uint16_t t = target_pvid(port);
	int rc = 0;

	if (snap.pvid[port] != t)
		rc = vlan_op(RTM_SETLINK, port, BRIDGE_VLAN_INFO_PVID
			     | (t == park_vid(port) || snap.untag[t] >> port & 1 ? BRIDGE_VLAN_INFO_UNTAGGED : 0), t);
	if (!rc && t != park_vid(port) && snap.member[park_vid(port)] >> port & 1)
		rc = vlan_op(RTM_DELLINK, port, 0, park_vid(port));
	return rc;
}

/* Every port; goes on after a failure and returns the first. */
static int sync_all(void)
{
	int rc = 0;

	for (uint8_t p = 0; p < BOARD_PORTS; p++) {
		int r = on_board(p) ? sync_port(p) : 0;

		if (r && !rc)
			rc = r;
	}
	return rc;
}

/* Removes from every parking VLAN what is not its own port: other ports' entries (a port whose PVID
 * it was is parked first) and the bridge's own entry. */
static int clean_reserved(void)
{
	for (uint8_t p = 0; p < BOARD_PORTS; p++) {
		uint16_t v = park_vid(p);

		for (uint8_t q = 0; on_board(p) && q <= CPU_PORT; q++) {
			int rc = 0;

			if (q == p || !(snap.member[v] >> q & 1))
				continue;
			if (q != CPU_PORT && snap.pvid[q] == v)
				rc = vlan_op(RTM_SETLINK, q, BRIDGE_VLAN_INFO_PVID | BRIDGE_VLAN_INFO_UNTAGGED, park_vid(q));
			if (!rc)
				rc = vlan_op(RTM_DELLINK, q, 0, v);
			if (rc)
				return rc;
		}
	}
	return 0;
}

/* ---- bridge lifecycle ---- */

static int link_set(int ifindex, const char *name, bool bridge, int master)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_NEWLINK, ifindex ? 0 : NLM_F_CREATE | NLM_F_EXCL);
	struct ifinfomsg *ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);

	ifi->ifi_index = ifindex;
	if (bridge) {
		struct nlattr *info, *data;

		ifi->ifi_flags = IFF_UP;
		ifi->ifi_change = IFF_UP;
		if (!ifindex)
			mnl_attr_put_strz(nlh, IFLA_IFNAME, name);
		info = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
		mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "bridge");
		data = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
		mnl_attr_put_u8(nlh, IFLA_BR_VLAN_FILTERING, 1);
		mnl_attr_put_u16(nlh, IFLA_BR_VLAN_DEFAULT_PVID, 0);
		mnl_attr_put_u32(nlh, IFLA_BR_STP_STATE, 0);
		mnl_attr_nest_end(nlh, data);
		mnl_attr_nest_end(nlh, info);
	} else {
		mnl_attr_put_u32(nlh, IFLA_MASTER, master);
	}
	return nl_talk(nlh);
}

static void log_fail(const char *what, const char *who, int err)
{
	fprintf(stderr, "rtlplayground: bridge: cannot %s %s: %s\n", what, who, strerror(-err));
}

/*
 * Creates swbr or adopts the one that is there (it keeps switching while the daemon is down) and
 * puts every board port in it; see the top for what a new and an adopted bridge are given. A failure
 * is logged and the daemon goes on: the VLAN commands then report the error.
 */
void bridge_init(void)
{
	uint8_t bad = 0;
	int rc, br = if_nametoindex(SWBR);
	bool adopted = br != 0;

	rc = link_set(br, SWBR, true, 0);
	if (rc)
		log_fail(br ? "configure" : "create", SWBR, rc);
	br = if_nametoindex(SWBR);
	for (uint8_t p = 0; br && p < BOARD_PORTS; p++) {
		if (!board_ifnames[p])
			continue;
		rc = if_nametoindex(board_ifnames[p]) ? link_set(if_nametoindex(board_ifnames[p]), NULL, false, br) : -ENODEV;
		if (rc)
			log_fail("add", board_ifnames[p], rc);
	}
	rc = snap_refresh();
	if (rc) {
		log_fail("read the VLANs of", SWBR, rc);
		return;
	}
	for (uint8_t p = 0; p < BOARD_PORTS; p++) {
		bool user = adopted && snap.pvid[p] && !reserved(snap.pvid[p]);

		/* adopted: the PVID a port has on a user VLAN is its register; a parked port (or one with
		 * none) is not known yet, see the top */
		pvid_reg[p] = user ? snap.pvid[p] : adopted ? 0 : 1;
		ingress_mode[p] = user || !adopted ? VLAN_ALL : VLAN_TAGGED;
		mode_unknown[p] = !user && adopted;
	}
	rc = clean_reserved();
	if (!rc && !adopted)
		rc = apply_vlan(1, board_mask() | PMASK_CPU, 0, &bad);
	if (!rc && !adopted)
		rc = sync_all();
	for (uint8_t p = 0; !rc && adopted && p < BOARD_PORTS; p++)
		if (on_board(p) && snap.pvid[p] != park_vid(p) && mode_unknown[p])
			rc = sync_port(p);	/* a port with no PVID, or in another port's parking VLAN */
	if (rc)
		log_fail("set up the VLANs of", SWBR, rc);
	snap.ok = false;
}

void bridge_commit(void)
{
	uint8_t bad = 0;
	int rc = snap_refresh();

	for (uint8_t p = 0; p < BOARD_PORTS; p++) {
		if (!on_board(p))
			continue;
		if (!pvid_reg[p])
			pvid_reg[p] = 1;
		if (mode_unknown[p])
			ingress_mode[p] = VLAN_ALL;
		mode_unknown[p] = false;
	}
	if (!rc)
		rc = clean_reserved();
	for (uint16_t vid = 1; !rc && vid <= VID_MAX; vid++)
		if (!reserved(vid) && snap.member[vid] && !created[vid] && (vid != 1 || vlan1_decided))
			rc = apply_vlan(vid, 0, 0, &bad);
	if (!rc && !vlan1_decided)
		rc = apply_vlan(1, board_mask() | PMASK_CPU, 0, &bad);
	if (!rc)
		rc = sync_all();
	if (rc)
		log_fail("finish the VLANs of", SWBR, rc);
	snap.ok = false;
}

/* ---- management hooks (mgmt.c) ---- */

/* The bridge's own entry for VLAN vid (if the VLAN exists) is the management one -- untagged, PVID --
 * or an ordinary tagged one. */
int bridge_mgmt_vlan(uint16_t vid, bool active)
{
	int rc;

	mgmt_active = active;
	rc = snap_refresh();
	if (!rc && (snap.member[vid] >> CPU_PORT & 1))
		rc = vlan_op(RTM_SETLINK, CPU_PORT, entry_flags(CPU_PORT, vid, 0), vid);
	snap.ok = false;
	return rc;
}

int bridge_mac_get(uint8_t mac[6])
{
	struct ifreq ifr = { 0 };
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = 0;

	if (s < 0)
		return -errno;
	snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", SWBR);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0)
		rc = -errno;
	else
		memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	close(s);
	return rc;
}

/* Also pins the address: a bridge whose MAC was set no longer follows its lowest port. */
int bridge_mac_set(const uint8_t mac[6])
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_NEWLINK, 0);
	struct ifinfomsg *ifi = mnl_nlmsg_put_extra_header(nlh, sizeof *ifi);

	ifi->ifi_index = if_nametoindex(SWBR);
	if (!ifi->ifi_index)
		return -ENODEV;
	mnl_attr_put(nlh, IFLA_ADDRESS, 6, mac);
	return nl_talk(nlh);
}

/* ---- the platform.h interface ---- */

void vlan_create(void) __banked
{
	uint16_t vid = vlan_settings.vlan, old_members, old_tagged;
	uint8_t bad;
	int rc;

	if (vid && reserved(vid)) {
		vlan_name_remove(vid);	/* the shared parser has stored the name before it called us */
		cmd_error("this VLAN is reserved here (parking VLAN of a port)\n");
		return;
	}
	if (!vid || vid >= 0xfff) {
		print_string("\nInvalid VLAN: "); print_short(vid); write_char('\n');
		return;
	}
	// Same as the 8051: the CPU port is always a tagged member (the management VLAN is the
	// exception in entry_flags(): the bridge itself is untagged there)
	vlan_settings.members |= PMASK_CPU;
	vlan_settings.tagged |= PMASK_CPU;
	print_string("vlan: "); print_short(vid);
	print_string(", members: "); print_short(vlan_settings.members);
	print_string(", tagged: "); print_short(vlan_settings.tagged); write_char('\n');

	rc = snap_refresh();
	if (rc) {
		nl_error("cannot read the bridge VLANs", rc);
		return;
	}
	old_members = snap.member[vid];
	old_tagged = old_members & ~snap.untag[vid];
	rc = apply_vlan(vid, vlan_settings.members, vlan_settings.tagged, &bad);
	if (rc) {
		report_port("cannot create the VLAN", bad, rc);
		/* back to what it was, as far as that works */
		if (!snap_refresh())
			apply_vlan(vid, old_members, old_tagged, &bad);
	} else {
		created[vid] = true;
		vlan1_decided |= vid == 1;
	}
	if (!snap_refresh())
		sync_all();
	snap.ok = false;
}

void vlan_delete(uint16_t vlan) __banked
{
	uint8_t bad;
	int rc;

	if (!vlan || vlan >= 0xfff)
		return;
	vlan_name_remove(vlan);
	if (reserved(vlan)) {
		cmd_error("this VLAN is reserved here (parking VLAN of a port)\n");
		return;
	}
	rc = snap_refresh();
	if (!rc)
		rc = apply_vlan(vlan, 0, 0, &bad);
	if (rc)
		nl_error("cannot delete the VLAN", rc);
	else {
		created[vlan] = false;
		vlan1_decided |= vlan == 1;
	}
	if (!snap_refresh())
		sync_all();
	snap.ok = false;
}

void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked
{
	uint16_t old;
	int rc;

	if (!on_board(port)) {
		cmd_error("no such port on this board\n");
		return;
	}
	if (reserved(pvid)) {
		cmd_error("this VLAN is reserved here (parking VLAN of a port)\n");
		return;
	}
	old = pvid_reg[port];
	pvid_reg[port] = pvid;
	rc = snap_refresh();
	if (!rc)
		rc = sync_port(port);
	if (rc) {
		pvid_reg[port] = old;
		report_port("cannot set the PVID", port, rc);
	}
	snap.ok = false;
}

/* The register, as on the 8051 (rtl837x_port.c:135): what the port's untagged frames will join, even
 * where it is a VLAN the port is not in yet. */
uint16_t port_pvid_get(uint8_t port) __banked
{
	if (port == CPU_PORT)
		return mgmt_vid();
	return on_board(port) ? pvid_reg[port] : 1;
}

/* Only the two modes a bridge can hold, see the top. */
bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked
{
	vlan_ingress_mode_t old;
	int rc;

	if (!on_board(port) || type >= VLAN_INVALID) {
		cmd_error("Invalid port or ingress filter type\n");
		return false;
	}
	if (type == VLAN_UNTAGGED) {
		cmd_error("a bridge cannot drop the tagged frames of a port: only 'a' (any) and 't' (tagged) are supported\n");
		return false;
	}
	old = ingress_mode[port];
	ingress_mode[port] = type;
	mode_unknown[port] = false;
	rc = snap_refresh();
	if (!rc)
		rc = sync_port(port);
	snap.ok = false;
	if (rc) {
		ingress_mode[port] = old;
		report_port("cannot set the ingress filter", port, rc);
		return false;
	}
	return true;
}

void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked
{
	switch (mode) {
	case VLAN_UNTAGGED:
		print_string("Untag.");
		break;
	case VLAN_TAGGED:
		print_string("Tagged");
		break;
	case VLAN_ALL:
		print_string("Any");
		break;
	default:
		print_string("!!err!!");
	}
}

static void print_vlan_ingress_port(uint8_t port)
{
	print_phys_port(port); write_char('\t');
	print_short(port_pvid_get(port)); write_char('\t');
	print_port_ingress_filter_mode(port == CPU_PORT ? VLAN_ALL : ingress_mode[port]); write_char('\t');
	print_string("Enabled\n");
}

void vlan_dump(void) __banked
{
	print_string("Ingress VLAN configuration:\n");
	print_string("Port\tPVID\tType\tFiltering\n");
	for (uint8_t port = machine.min_port; port <= machine.max_port; port++)
		print_vlan_ingress_port(port);
	print_vlan_ingress_port(CPU_PORT);

	write_char('\n');
	print_string("Type - Which frame types are allowed: untagged, tagged or any\n");
	print_string("Filtering - Whether packets not belonging to member VLANs on that port are dropped\n");
	print_string("PVID - Assumed VLAN for untagged packets\n");
}

/* The word layout of platform.h: bit 25 valid, bits 19..10 untagged ports, bits 9..0 members. The
 * parking VLANs are not VLANs. */
bool vlan_entry_get(uint16_t vlan) __banked
{
	uint32_t word = 0;

	if (vlan && vlan < 0xfff && !reserved(vlan) && !snap_refresh() && snap.member[vlan])
		word = 1UL << 25 | (uint32_t)(snap.untag[vlan] & snap.member[vlan] & 0x3ff) << 10 | (snap.member[vlan] & 0x3ff);
	for (uint8_t i = 0; i < 4; i++)
		port_buf[i] = word >> (24 - 8 * i);
	return word != 0;
}

/* The name table is the 8051's (rtl837x_port.c:158 and :290): "<3 hex digits of the VLAN><name> ". */
__xdata uint16_t vlan_name(uint16_t vlan) __banked
{
	__xdata int16_t i = 0;
	__xdata uint8_t begin = 1;
	while (vlan_names[i]) {
		if (begin && vlan_names[i] == hex[(vlan >> 8) & 0xf] && vlan_names[i + 1] == hex[(vlan >> 4) & 0xf] && vlan_names[i + 2] == hex[vlan & 0xf])
			break;
		begin = vlan_names[i++] == ' ' ? 1 : 0;
	}
	if (vlan_names[i])
		return i + 3;

	return 0xffff;
}

void vlan_name_remove(uint16_t vlan) __banked
{
	uint16_t name_pos = vlan_name(vlan), entry_start, pos, j;

	if (name_pos == 0xffff)
		return;

	entry_start = name_pos - 3;
	pos = entry_start;

	while (pos < vlan_ptr && vlan_names[pos] != ' ')
		pos++;
	if (pos >= vlan_ptr)
		return;
	pos++;

	for (j = 0; j < vlan_ptr - pos; j++)
		vlan_names[entry_start + j] = vlan_names[pos + j];

	vlan_ptr -= pos - entry_start;
	vlan_names[vlan_ptr] = 0;
}
