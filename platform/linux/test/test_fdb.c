/* fdb.c on a canned bridge: the slot assignment, the lookup table, the page loop of send_l2 (a copy of
 * its control flow) and the dump count of a page. The "kernel" is the array kfdb; nl_dump walks it
 * and hands fdb.c the RTM_NEWNEIGH messages a real dump would. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define if_nametoindex t_ifindex
#define SWBR_IF 100
static unsigned t_ifindex(const char *n);
#include "../fdb.c"
#undef if_nametoindex

/* ---- what fdb.c links against ---- */
const char *const board_ifnames[9] = { "lan1", "lan2", "lan3", "lan4", "lan5", "lan6" };
__xdata uint8_t port_buf[24];
__xdata struct l2_entry l2_buf;
__xdata struct uip_eth_addr uip_ethaddr;
__xdata uint16_t management_vlan = 1;
bool bridge_vid_reserved(uint16_t vid) { return vid >= 4089; }
static int64_t clock_ms;
int64_t now_ms(void) { return clock_ms; }
void cmd_error(const char *m) { }
void print_string(const char *s) { }
void write_char(char c) { }
void print_short(uint16_t v) { }
void print_mac(__xdata uint8_t *m) __banked { }
void print_phys_port(uint8_t p) { }
void itoa(uint8_t v) { }
uint8_t port_lag_of(uint8_t p) __banked { return PORT_LAG_NONE; }
struct nlmsghdr *nl_msg(char *buf, uint16_t type, uint16_t flags)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	return nlh;
}
void nl_error(const char *what, int err) { }

static unsigned t_ifindex(const char *n)
{
	if (!strcmp(n, "swbr"))
		return SWBR_IF;
	return !strncmp(n, "lan", 3) ? n[3] - '0' : 0;
}

#define KMAX 6000
static struct { uint8_t mac[6]; uint16_t vid; int ifindex; uint16_t state; } kfdb[KMAX];
static int kn, dumps, dump_rc, deleted, last_del_if;

static int dump_msg(unsigned i, mnl_cb_t cb, void *data)
{
	char buf[256];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct ndmsg *nd;

	nlh->nlmsg_type = RTM_NEWNEIGH;
	nd = mnl_nlmsg_put_extra_header(nlh, sizeof *nd);
	nd->ndm_family = AF_BRIDGE;
	nd->ndm_ifindex = kfdb[i].ifindex;
	nd->ndm_state = kfdb[i].state;
	mnl_attr_put(nlh, NDA_LLADDR, 6, kfdb[i].mac);
	mnl_attr_put_u16(nlh, NDA_VLAN, kfdb[i].vid);
	mnl_attr_put_u32(nlh, NDA_MASTER, SWBR_IF);
	return cb(nlh, data);
}

int nl_dump(struct nlmsghdr *nlh, mnl_cb_t cb, void *data)
{
	dumps++;
	if (dump_rc)
		return dump_rc;
	for (int i = 0; i < kn; i++)
		dump_msg(i, cb, data);
	return 0;
}

int nl_talk(struct nlmsghdr *nlh)
{
	const struct ndmsg *nd = mnl_nlmsg_get_payload(nlh);

	deleted++;
	last_del_if = nd->ndm_ifindex;
	return 0;
}

/* ---- helpers ---- */
static int fails;
static void check(const char *what, long want, long got)
{
	if (want != got) {
		printf("FAIL: %s: expected %ld, got %ld\n", what, want, got);
		fails++;
	} else {
		printf("ok:   %s\n", what);
	}
}

/* the MACs that share a home: groups c and d equal (so they cancel), 1..4095 */
static void colliding_mac(uint8_t m[6], unsigned v)
{
	m[0] = m[1] = m[2] = 0;
	m[3] = v >> 4;
	m[4] = (v & 15) << 4 | v >> 8;
	m[5] = v;
}

static void kadd(const uint8_t *mac, uint16_t vid, int ifindex, uint16_t state)
{
	memcpy(kfdb[kn].mac, mac, 6);
	kfdb[kn].vid = vid;
	kfdb[kn].ifindex = ifindex;
	kfdb[kn].state = state;
	kn++;
}

static void kadd_v(unsigned v, uint16_t vid, int ifindex, uint16_t state)
{
	uint8_t m[6];

	colliding_mac(m, v);
	kadd(m, vid, ifindex, state);
}

/* removes v (a colliding MAC) from the kernel's table */
static void kdel_v(unsigned v)
{
	uint8_t m[6];

	colliding_mac(m, v);
	for (int i = 0; i < kn; i++)
		if (!memcmp(kfdb[i].mac, m, 6)) {
			kfdb[i] = kfdb[--kn];
			return;
		}
}

/* the slot an entry has now (after a dump), -1 if it is not listed */
static int slot_of_v(unsigned v)
{
	uint8_t m[6];

	colliding_mac(m, v);
	for (int s = 0; s < SLOTS; s++)
		if (slot[s].used && !memcmp(slot[s].mac, m, 6))
			return s;
	return -1;
}

static int listed(void)
{
	int n = 0;

	for (int s = 0; s < SLOTS; s++)
		n += slot[s].used;
	return n;
}

/* send_l2 (httpd/page_impl.c) without the output: how many calls one page makes, the idx values
 * it listed; the entries are appended to seen[] */
static int page(uint16_t idx, uint16_t *seen, int *nseen)
{
	int left = 30, calls = 0;
	uint16_t first_entry = 0xffff;

	l2_buf.idx = idx & 0xfff;
	while (1) {
		left--;
		calls++;
		if (l2_entry_next())
			seen[(*nseen)++] = l2_buf.idx;
		l2_buf.idx += 1;
		if (first_entry == 0xffff)
			first_entry = l2_buf.idx;
		else if (first_entry == l2_buf.idx || !left)
			break;
	}
	return calls;
}

static void reset(void)
{
	memset(slot, 0, sizeof slot);
	kn = dumps = dump_rc = deleted = 0;
	dump_no = 0;
	warned_full = dump_failed = false;
	dump_at = 0;
	clock_ms += 10000;
}

int main(void)
{
	static uint16_t seen[200];
	int nseen;

	/* ---- item 2: a slot that was freed keeps its key; a new key does not take it ---- */
	reset();
	for (unsigned v = 1; v <= 10; v++)
		kadd_v(v, 1, 3, 0);
	fdb_dump();
	int s1 = slot_of_v(1), s6 = slot_of_v(6);
	check("colliding entries get consecutive slots from their home", 1, s6 - s1 == 5);
	kdel_v(1);
	kdel_v(6);
	fdb_dump();
	check("freed entries are not listed", 8, listed());
	kadd_v(77, 1, 3, 0);	/* a new key, same home, between the two returns */
	kadd_v(1, 1, 3, 0);
	kadd_v(6, 1, 3, 0);
	fdb_dump();
	check("a returning entry gets its old slot (1)", s1, slot_of_v(1));
	check("a returning entry gets its old slot (6)", s6, slot_of_v(6));
	check("the new key went to a never-used slot", 1, slot_of_v(77) > slot_of_v(10));
	check("no entry twice, none lost", 11, listed());

	/* ---- a full table of keys: a new entry takes a freed slot, the walk is complete and once each ---- */
	reset();
	for (unsigned v = 1; v <= 4095; v++)
		kadd_v(v, 1, 3, 0);
	kadd_v(1, 2, 3, 0);
	check("4096 entries fill every slot", 4096, (fdb_dump(), listed()));
	for (unsigned v = 1; v <= 50; v++)
		kdel_v(v);
	fdb_dump();
	check("50 freed", 4096 - 50, listed());
	for (unsigned v = 1; v <= 50; v++)
		kadd_v(v, 3, 4, 0);
	fdb_dump();
	check("50 new entries take the freed slots: the table is full again", 4096, listed());
	for (unsigned v = 1; v <= 50; v++)
		kadd_v(v, 1, 5, 0);
	fdb_dump();
	check("more entries than slots: 4096 listed, the rest are not", 4096, listed());
	check("...said once", 1, warned_full);
	int dup = 0;
	for (int a = 0; a < SLOTS; a++)
		for (int b = a + 1; b < SLOTS && !dup; b += 1)
			if (slot[a].used && slot[b].used && slot[a].vid == slot[b].vid && !memcmp(slot[a].mac, slot[b].mac, 6))
				dup = 1;
	check("...and none twice", 0, dup);

	/* ---- item 3: the key is MAC and VLAN; a station that moves keeps its slot and gets the new port ---- */
	reset();
	kadd_v(9, 1, 3, 0);
	kadd_v(9, 30, 4, NUD_NOARP);
	fdb_dump();
	check("the same MAC in two VLANs: two entries", 2, listed());
	check("...in two slots", 1, slot_of_v(9) >= 0 && slot[slot_of_v(9)].vid != 0);
	int n9 = 0;
	for (int s = 0; s < SLOTS; s++)
		n9 += slot[s].used && slot[s].vid == 30 && slot[s].port == 3 && slot[s].is_static;
	check("...each with its own VLAN, port and type", 1, n9);
	int before = -1, after = -1;
	for (int s = 0; s < SLOTS; s++)
		if (slot[s].used && slot[s].vid == 1)
			before = s;
	for (int i = 0; i < kn; i++)
		if (kfdb[i].vid == 1)
			kfdb[i].ifindex = 5;
	fdb_dump();
	for (int s = 0; s < SLOTS; s++)
		if (slot[s].used && slot[s].vid == 1)
			after = s;
	check("a station moves: same slot", before, after);
	check("...new port", 4, slot[after].port);

	/* ---- item 4: only the CPU entry is protected ---- */
	reset();
	memcpy(uip_ethaddr.addr, (uint8_t[]){ 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc }, 6);
	kadd(uip_ethaddr.addr, 1, SWBR_IF, NUD_PERMANENT);
	kadd(uip_ethaddr.addr, 5, 1, NUD_NOARP);	/* a user's static entry of that MAC in another VLAN, on lan1 */
	fdb_dump();
	check("the own entry and the user's: two entries", 2, listed());
	int own = -1, usr = -1;
	for (int s = 0; s < SLOTS; s++)
		if (slot[s].used)
			*(slot[s].port == CPU_PORT ? &own : &usr) = s;
	check("the own entry is not deleted", 0, l2_entry_delete(own));
	check("...no kernel call", 0, deleted);
	check("a user's entry with the same MAC is deleted", 1, l2_entry_delete(usr));
	check("...on its port", 1, last_del_if);

	/* ---- item 5: the lookup table spreads colliding entries over the whole of it ---- */
	reset();
	for (unsigned v = 1; v <= 3500; v++)
		kadd_v(v, 1, 3, 0);
	fdb_dump();
	unsigned worst = 0, upper = 0;
	for (unsigned c = 0; c < TAB; c++) {
		if (!tab[c])
			continue;
		unsigned from = thash(slot[tab[c] - 1].mac, slot[tab[c] - 1].vid), d = (c - from) & (TAB - 1);

		worst = d > worst ? d : worst;
		upper += c >= TAB / 2;
	}
	check("3500 entries with one home: the longest probe chain of the lookup table is short", 1, worst < 64);
	check("...and the table is used from end to end", 1, upper > 1000);

	/* ---- item 6: a page's six MACs counts come from one dump ---- */
	reset();
	kadd_v(1, 1, 1, 0);
	kadd_v(2, 1, 1, 0);
	kadd_v(3, 1, 2, 0);
	kadd_v(4, 1, 2, NUD_NOARP);
	dumps = 0;
	unsigned got[6];
	for (int p = 0; p < 6; p++) {
		port_macs_learned_get(p);
		got[p] = port_buf[2] << 8 | port_buf[3];
	}
	check("six ports: one dump", 1, dumps);
	check("port 1 has two learned", 2, got[0]);
	check("port 2 has one learned (the static one is not counted)", 1, got[1]);
	port_macs_learned_get(0);
	check("the next run starts a new dump", 2, dumps);
	clock_ms += 300;
	port_macs_learned_get(1);
	check("a run older than 250 ms too", 3, dumps);

	/* ---- item 7: the page loop on an empty or unreadable table ---- */
	reset();
	nseen = 0;
	check("an empty table: the page makes 2 calls", 2, page(0, seen, &nseen));
	check("...it lists nothing", 0, nseen);
	check("...from one dump", 1, dumps);
	dumps = 0;
	check("from another index", 2, page(0x7ff, seen, &nseen));
	dumps = 0;
	kadd_v(1, 1, 1, 0);
	dump_rc = -EIO;
	clock_ms += 300;	/* the empty table's dump is old */
	nseen = 0;
	check("a failed dump: 2 calls", 2, page(0, seen, &nseen));
	check("...and 2 dumps", 2, dumps);
	dump_rc = 0;
	for (unsigned v = 2; v <= 100; v++)
		kadd_v(v, 1, 1, 0);
	clock_ms += 300;
	nseen = 0;
	int calls = page(0, seen, &nseen);
	check("a table with 100 entries: a page lists 29 or more", 1, nseen >= 29 && nseen <= 30);
	check("...in at most L2_MAX_TRANSFER calls", 1, calls <= 30);
	uint16_t next = seen[nseen - 1] + 1;
	int total = nseen, pages = 1;
	for (; pages < 20; pages++) {
		nseen = 0;
		page(next, seen, &nseen);
		if (!nseen)
			break;
		total += nseen;
		next = seen[nseen - 1] + 1;
		if (next >= SLOTS)
			break;
	}
	check("the walk by pages reaches all 100 entries (the last page wraps)", 1, total >= 100);

	/* ---- a page of 30 entries reads the kernel's table once, not once per entry ---- */
	reset();
	for (unsigned v = 1; v <= 100; v++)
		kadd_v(v, 1, 1, 0);
	dumps = 0;
	nseen = 0;
	page(0, seen, &nseen);
	check("a page of 30 entries: 30 listed", 30, nseen);
	check("...from one dump", 1, dumps);
	nseen = 0;
	page(seen[29] + 1, seen, &nseen);
	check("the next page, right after: the same dump, the same slots", 1, dumps);
	clock_ms += 300;
	nseen = 0;
	page(0, seen, &nseen);
	check("a page 300 ms later reads the table again", 2, dumps);
	l2_entry_delete(slot_of_v(5));
	check("a delete dumps (it needs the slot as it is)", 3, dumps);
	nseen = 0;
	page(0, seen, &nseen);
	check("...and the page after it too", 4, dumps);

	printf(fails ? "%d FAILED\n" : "all ok\n", fails);
	return fails != 0;
}
