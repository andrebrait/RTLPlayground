/*
 * The L2 table of the 8051 is the forwarding database of the bridge swbr: every read is an
 * RTM_GETNEIGH dump (AF_BRIDGE), every delete an RTM_DELNEIGH.
 *
 * The 8051 index is a hash slot of the 4096-entry table (send_l2, httpd/page_impl.c): an entry keeps
 * its slot for as long as it exists, the page walks the slots upwards with `idx=` the last slot + 1,
 * and the walk ends when the client sees a slot twice. A position in a sorted dump would shift under
 * a page that is being walked (an entry added before the cursor shows up a second time at the next
 * position), so the daemon keeps the slots too: an entry gets the free slot nearest to its hash
 * (home()) the first time a dump shows it, keeps it while later dumps show it, and gives it up
 * with the first dump that does not. The table holds what the hardware holds, 4096 entries: of
 * a bridge with more, the entries that found no free slot are not listed (said once on stderr).
 * shortcut: an entry that vanishes and returns within one walk of the page may come back in another
 * slot (its home taken meanwhile) and show twice; the page then shows a row twice, nothing else.
 *
 * Listed: the entries on a board port (learned = dynamic, static = `bridge fdb ... static`) and the
 * entry of the switch's own MAC in the management VLAN (static, port CPU), as the 8051 shows it
 * (page_impl.c l2_delete refuses it). Not listed: the local entries of the ports
 * (permanent, one per VLAN), the bridge's own entry of every other VLAN, entries of a parking VLAN
 * (the daemon's, see bridge.c) and entries on ports that are not board ports. An entry a user adds
 * as `permanent` is local to the kernel and is not listed either; `static` is.
 */
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "nl.h"
#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"
#include "uip/uip.h"

#define SLOTS 4096		/* the slots of the 8051 table; l2_buf.idx is 12 bits */
#define TAB (2 * SLOTS)		/* lookup table over the slots, at most half full */

extern __xdata uint16_t management_vlan;
extern __xdata struct uip_eth_addr uip_ethaddr;

struct slot {
	uint16_t vid;
	uint8_t mac[6];
	uint8_t port;		/* logical port, CPU_PORT for the bridge */
	bool is_static;
	bool used;
	uint32_t seen;		/* the dump that last showed it */
};

static struct slot slot[SLOTS];
static uint16_t tab[TAB];	/* slot + 1 by key, 0 = empty; rebuilt by every dump */
static uint32_t dump_no;
static unsigned learned[CPU_PORT + 1];	/* the learned entries per port in the last dump */
static bool warned_full;
static bool dump_failed;

/* The slot nearest to home: the three 12-bit groups of the address folded together and the VLAN, so
 * addresses that differ only in their low bits (the ones of one NIC vendor's run) never collide. */
static unsigned home(const uint8_t *mac, uint16_t vid)
{
	unsigned a = mac[0] << 4 | mac[1] >> 4, b = (mac[1] & 15) << 8 | mac[2],
		 c = mac[3] << 4 | mac[4] >> 4, d = (mac[4] & 15) << 8 | mac[5];

	return (a ^ b ^ c ^ d ^ vid) & (SLOTS - 1);
}

struct dump {
	int br;
	int port_if[CPU_PORT];
};

static void tab_put(unsigned s)
{
	unsigned h = home(slot[s].mac, slot[s].vid);

	while (tab[h])
		h = (h + 1) & (TAB - 1);
	tab[h] = s + 1;
}

static int tab_find(const uint8_t *mac, uint16_t vid)
{
	for (unsigned h = home(mac, vid); tab[h]; h = (h + 1) & (TAB - 1)) {
		const struct slot *s = &slot[tab[h] - 1];

		if (s->vid == vid && !memcmp(s->mac, mac, 6))
			return tab[h] - 1;
	}
	return -1;
}

static int fdb_cb(const struct nlmsghdr *nlh, void *data)
{
	const struct dump *d = data;
	const struct ndmsg *nd = mnl_nlmsg_get_payload(nlh);
	const uint8_t *mac = NULL;
	uint16_t vid = 0;
	uint32_t master = 0;
	struct nlattr *a;
	uint8_t port = 0xff;
	bool is_static;
	int s;

	if (nlh->nlmsg_type != RTM_NEWNEIGH || nd->ndm_family != AF_BRIDGE)
		return MNL_CB_OK;
	mnl_attr_for_each(a, nlh, sizeof *nd) {
		switch (mnl_attr_get_type(a)) {
		case NDA_LLADDR:
			if (mnl_attr_get_payload_len(a) == 6)
				mac = mnl_attr_get_payload(a);
			break;
		case NDA_VLAN:
			if (mnl_attr_validate(a, MNL_TYPE_U16) >= 0)
				vid = mnl_attr_get_u16(a);
			break;
		case NDA_MASTER:
			if (mnl_attr_validate(a, MNL_TYPE_U32) >= 0)
				master = mnl_attr_get_u32(a);
			break;
		}
	}
	if (!mac || !vid || vid > 4094 || master != (uint32_t)d->br || bridge_vid_reserved(vid))
		return MNL_CB_OK;
	if (nd->ndm_state & NUD_PERMANENT) {
		/* the kernel's local entries: only the management one is the table's own entry; a bridge that
		 * took the address of one of its ports holds it on that port, so the device does not matter */
		if (memcmp(mac, uip_ethaddr.addr, 6) || vid != (management_vlan ? management_vlan : 1))
			return MNL_CB_OK;
		port = CPU_PORT;
		is_static = true;
	} else {
		if (nd->ndm_ifindex == d->br)
			port = CPU_PORT;
		for (uint8_t p = 0; p < CPU_PORT; p++)
			if (d->port_if[p] && d->port_if[p] == nd->ndm_ifindex)
				port = p;
		if (port == 0xff)
			return MNL_CB_OK;
		is_static = nd->ndm_state & NUD_NOARP;
	}
	s = tab_find(mac, vid);
	if (s < 0) {
		for (unsigned i = 0; i < SLOTS && s < 0; i++)
			if (!slot[(home(mac, vid) + i) & (SLOTS - 1)].used)
				s = (home(mac, vid) + i) & (SLOTS - 1);
		if (s < 0) {
			if (!warned_full) {
				warned_full = true;
				fprintf(stderr, "rtlplayground: the bridge has more than %d L2 entries, the others are not listed\n", SLOTS);
			}
			return MNL_CB_OK;
		}
		slot[s].used = true;
		slot[s].vid = vid;
		memcpy(slot[s].mac, mac, 6);
		tab_put(s);
	}
	slot[s].port = port;
	slot[s].is_static = is_static;
	slot[s].seen = dump_no;
	if (!is_static)
		learned[port]++;
	return MNL_CB_OK;
}

/* Reads the FDB into the slots: every slot holds an entry the kernel shows now, or is free. */
static int fdb_dump(void)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_GETNEIGH, 0);
	struct ndmsg *nd = mnl_nlmsg_put_extra_header(nlh, sizeof *nd);
	struct dump d = { .br = if_nametoindex(SWBR) };
	int rc;

	nd->ndm_family = AF_BRIDGE;
	if (!d.br)
		return -ENODEV;
	for (uint8_t p = 0; p < CPU_PORT; p++)
		d.port_if[p] = board_ifnames[p] ? if_nametoindex(board_ifnames[p]) : 0;
	memset(tab, 0, sizeof tab);
	memset(learned, 0, sizeof learned);
	dump_no++;
	for (unsigned s = 0; s < SLOTS; s++)
		if (slot[s].used)
			tab_put(s);
	rc = nl_dump(nlh, fdb_cb, &d);
	for (unsigned s = 0; !rc && s < SLOTS; s++)
		if (slot[s].used && slot[s].seen != dump_no)
			slot[s].used = false;
	return rc;
}

/* A dump failure is said once per run of failures, not once per call of a page that makes 30. */
static bool dump_ok(void)
{
	int rc = fdb_dump();

	if (rc && !dump_failed)
		fprintf(stderr, "rtlplayground: cannot read the L2 table of " SWBR ": %s\n", strerror(-rc));
	dump_failed = rc != 0;
	return !rc;
}

/* The first used slot at or after from, wrapping; -1 for an empty table. */
static int slot_from(unsigned from)
{
	for (unsigned i = 0; i < SLOTS; i++)
		if (slot[(from + i) & (SLOTS - 1)].used)
			return (from + i) & (SLOTS - 1);
	return -1;
}

bool l2_entry_next(void) __banked
{
	int s = dump_ok() ? slot_from(l2_buf.idx & (SLOTS - 1)) : -1;

	if (s < 0)
		return false;
	l2_buf.idx = s;
	l2_buf.vid = slot[s].vid;
	memcpy(l2_buf.mac, slot[s].mac, 6);
	l2_buf.port = slot[s].port;
	l2_buf.is_static = slot[s].is_static;
	return true;
}

static int fdb_del(const struct slot *s)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_DELNEIGH, 0);
	struct ndmsg *nd = mnl_nlmsg_put_extra_header(nlh, sizeof *nd);

	nd->ndm_family = AF_BRIDGE;
	nd->ndm_flags = NTF_MASTER;
	nd->ndm_ifindex = if_nametoindex(board_ifnames[s->port]);
	if (!nd->ndm_ifindex)
		return -ENODEV;
	mnl_attr_put(nlh, NDA_LLADDR, 6, s->mac);
	mnl_attr_put_u16(nlh, NDA_VLAN, s->vid);
	return nl_talk(nlh);
}

/* Only the entry in slot idx: a slot that was emptied meanwhile must not delete its neighbour. */
uint8_t l2_entry_delete(uint16_t idx) __banked
{
	int rc;

	if (idx >= SLOTS || !dump_ok() || !slot[idx].used)
		return 0;
	if (slot[idx].port == CPU_PORT || !memcmp(slot[idx].mac, uip_ethaddr.addr, 6))
		return 0;	/* the switch's own entry keeps management reachable */
	rc = fdb_del(&slot[idx]);
	if (rc && rc != -ENOENT)
		fprintf(stderr, "rtlplayground: cannot delete an L2 entry: %s\n", strerror(-rc));
	return !rc;
}

/* Every learned entry, as the hardware flush of all ports does; static entries stay. */
uint8_t port_l2_forget(void) __banked
{
	int first = 0;

	if (fdb_dump()) {
		cmd_error("cannot read the L2 table\n");
		return 0;
	}
	for (unsigned s = 0; s < SLOTS; s++) {
		int rc;

		if (!slot[s].used || slot[s].is_static || slot[s].port == CPU_PORT)
			continue;
		rc = fdb_del(&slot[s]);
		if (rc && rc != -ENOENT && !first)
			first = rc;
	}
	if (first)
		nl_error("cannot forget every learned L2 entry", first);
	return !first;
}

void port_l2_learned(void) __banked
{
	print_string("\n\tMAC\t\tVLAN\ttype\tport\n");
	if (fdb_dump()) {
		cmd_error("cannot read the L2 table\n");
		return;
	}
	for (unsigned s = 0; s < SLOTS; s++) {
		uint8_t lag;

		if (!slot[s].used)
			continue;
		print_mac(slot[s].mac);
		write_char('\t');
		print_short(slot[s].vid);
		print_string(slot[s].is_static ? "\tstatic\t" : "\tlearned\t");
		lag = port_lag_of(slot[s].port);
		if (lag == PORT_LAG_NONE) {
			print_phys_port(slot[s].port);
		} else {
			print_string("LAG");
			itoa(lag + 1);
		}
		write_char('\n');
	}
}

/* The learned entries of a port: the hardware counter of the learning limit counts what the port learned, not what was configured. */
void port_macs_learned_get(uint8_t port) __banked
{
	unsigned n = port <= CPU_PORT && dump_ok() ? learned[port] : 0;

	port_buf[0] = port_buf[1] = 0;
	port_buf[2] = n >> 8;
	port_buf[3] = n;
}
