/*
 * The L2 table of the 8051 is the forwarding database of the bridge swbr: every read is an
 * RTM_GETNEIGH dump (AF_BRIDGE), every delete an RTM_DELNEIGH.
 *
 * The 8051 index is a hash slot of the 4096-entry table (send_l2, httpd/page_impl.c): an entry keeps
 * its slot for as long as it exists, the page walks the slots upwards with `idx=` the last slot + 1,
 * and the walk ends when the client sees a slot twice. A position in a sorted dump would shift under
 * a page that is being walked (an entry added before the cursor shows up a second time at the next
 * position), so the daemon keeps the slots too: an entry gets the slot nearest to its hash (home())
 * that has never held an entry, the first time a dump shows it, and keeps it while later dumps show it.
 * A slot whose entry is gone is not listed but keeps the key: the same entry coming back (a station
 * that is aged out and learned again during one walk of the page) gets its old slot, so it is never
 * listed twice by one walk. Only when no never-used slot is left a new entry takes such a freed slot
 * (the first one from its home). The table holds what the hardware holds, 4096 entries: of a bridge
 * with more live entries, the ones that found no slot are not listed (said once on stderr).
 * Slots are not kept across a daemon restart: entries that collide may get other slots then, so a
 * page being walked while the daemon restarts may list such an entry twice or miss it.
 * shortcut: an entry whose old slot was taken over (no never-used slot was left, so the table was
 * full of keys) and that returns within one walk may show twice; the page then shows a row twice.
 *
 * One page of /l2.json asks for up to 30 entries in a row (l2_entry_next): they are read from ONE dump
 * of the kernel's table, reused for DUMP_REUSE_MS (the slots stay as that dump left them, which a
 * walk needs anyway); a dump of a table of 4000 entries costs about 50 ms on a fast ARM, so a dump per
 * entry made a walk of the table take seconds. Every other reader (delete, forget, `l2` show, the
 * MACs column) dumps afresh, and a delete or forget drops the reused dump.
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
#define TAB_BITS 13
#define TAB (1 << TAB_BITS)	/* lookup table over the slots: at most half full, 3/4 before it is rebuilt */
#define DUMP_REUSE_MS 250

extern __xdata uint16_t management_vlan;
extern __xdata struct uip_eth_addr uip_ethaddr;

struct slot {
	uint16_t vid;
	uint8_t mac[6];
	uint8_t port;		/* logical port, CPU_PORT for the bridge */
	bool is_static;
	bool keyed;		/* vid and mac are an entry's: it is listed now (used) or was and may return */
	bool used;		/* the last dump showed it */
	uint32_t seen;		/* the dump that last showed it */
};

static struct slot slot[SLOTS];
/* slot + 1 by key, 0 = empty. A cell stays when its slot is given another key (the lookup compares the
 * slot's key, so it only costs a probe); every dump rebuilds the table, and so does tab_put() at 3/4. */
static uint16_t tab[TAB];
static unsigned tab_n;
static uint32_t dump_no;
static unsigned learned[CPU_PORT + 1];	/* the learned entries per port in the last dump */
static bool warned_full;
static bool dump_failed;
static int64_t dump_at;		/* when the slots were last read in full; 0: they may not be used as they are */

/* The slot nearest to home: the three 12-bit groups of the address folded together and the VLAN, so
 * addresses that differ only in their low bits (the ones of one NIC vendor's run) never collide. */
static unsigned home(const uint8_t *mac, uint16_t vid)
{
	unsigned a = mac[0] << 4 | mac[1] >> 4, b = (mac[1] & 15) << 8 | mac[2],
		 c = mac[3] << 4 | mac[4] >> 4, d = (mac[4] & 15) << 8 | mac[5];

	return (a ^ b ^ c ^ d ^ vid) & (SLOTS - 1);
}

/* Where the lookup table starts to probe: a different, multiplicative hash over the whole key, so
 * addresses that share a home (which a host on the segment can arrange) do not share a probe chain,
 * and the whole table is used. */
static unsigned thash(const uint8_t *mac, uint16_t vid)
{
	uint64_t k = 0;

	for (int i = 0; i < 6; i++)
		k = k << 8 | mac[i];
	return ((k << 12 | vid) * 0x9E3779B97F4A7C15ull) >> (64 - TAB_BITS);
}

struct dump {
	int br;
	int port_if[CPU_PORT];
};

static void tab_put(unsigned s)
{
	unsigned h = thash(slot[s].mac, slot[s].vid);

	while (tab[h])
		h = (h + 1) & (TAB - 1);
	tab[h] = s + 1;
	tab_n++;
}

static void tab_build(void)
{
	memset(tab, 0, sizeof tab);
	tab_n = 0;
	for (unsigned s = 0; s < SLOTS; s++)
		if (slot[s].keyed)
			tab_put(s);
}

static int tab_find(const uint8_t *mac, uint16_t vid)
{
	for (unsigned h = thash(mac, vid); tab[h]; h = (h + 1) & (TAB - 1)) {
		const struct slot *s = &slot[tab[h] - 1];

		if (s->keyed && s->vid == vid && !memcmp(s->mac, mac, 6))
			return tab[h] - 1;
	}
	return -1;
}

/* The slot for a key the table does not know: the first one from its home that never held an entry,
 * else the first freed one; -1 when every slot is listed. */
static int slot_new(const uint8_t *mac, uint16_t vid)
{
	unsigned h = home(mac, vid);
	int freed = -1;

	for (unsigned i = 0; i < SLOTS; i++) {
		unsigned c = (h + i) & (SLOTS - 1);

		if (!slot[c].keyed)
			return c;
		if (!slot[c].used && freed < 0)
			freed = c;
	}
	return freed;
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
		s = slot_new(mac, vid);
		if (s < 0) {
			if (!warned_full) {
				warned_full = true;
				fprintf(stderr, "rtlplayground: the bridge has more than %d L2 entries, the others are not listed\n", SLOTS);
			}
			return MNL_CB_OK;
		}
		slot[s].keyed = true;
		slot[s].vid = vid;
		memcpy(slot[s].mac, mac, 6);
		tab_put(s);
		if (tab_n > TAB / 4 * 3)
			tab_build();
	}
	slot[s].used = true;
	slot[s].port = port;
	slot[s].is_static = is_static;
	slot[s].seen = dump_no;
	if (!is_static)
		learned[port]++;
	return MNL_CB_OK;
}

/* Reads the FDB into the slots: a used slot holds an entry the kernel shows now. */
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
	memset(learned, 0, sizeof learned);
	dump_no++;
	tab_build();
	rc = nl_dump(nlh, fdb_cb, &d);
	for (unsigned s = 0; !rc && s < SLOTS; s++)
		if (slot[s].used && slot[s].seen != dump_no)
			slot[s].used = false;
	dump_at = rc ? 0 : now_ms();
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
	bool fresh = dump_at && now_ms() - dump_at < DUMP_REUSE_MS;
	int s = fresh || dump_ok() ? slot_from(l2_buf.idx & (SLOTS - 1)) : -1;

	if (s < 0) {
		/* send_l2 adds 1 and ends its walk when that is the first index it got: with this, an empty
		 * table or a failed dump costs it two calls, not L2_MAX_TRANSFER of them */
		l2_buf.idx--;
		return false;
	}
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
	if (slot[idx].port == CPU_PORT)
		return 0;	/* the switch's own entry keeps management reachable */
	rc = fdb_del(&slot[idx]);
	dump_at = 0;
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
	dump_at = 0;
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

/* The learned entries of a port. A page asks for the ports in ascending order; the counts of one such
 * run come from one dump (a lower or equal port starts a new run, and so does 250 ms). The hardware
 * counter of the learning limit counts what the port learned, not what was configured. */
void port_macs_learned_get(uint8_t port) __banked
{
	static unsigned count[CPU_PORT + 1];
	static int last = -1;
	static int64_t at;
	unsigned n = 0;

	if (port <= last || now_ms() - at > 250) {
		bool ok = dump_ok();

		memcpy(count, ok ? learned : (unsigned[CPU_PORT + 1]){ 0 }, sizeof count);
		at = now_ms();
	}
	last = port;
	if (port <= CPU_PORT)
		n = count[port];
	port_buf[0] = port_buf[1] = 0;
	port_buf[2] = n >> 8;
	port_buf[3] = n;
}
