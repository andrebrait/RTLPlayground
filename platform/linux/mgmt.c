/*
 * The management IP of the switch: it lives on swbr itself, which carries the management VLAN
 * untagged (bridge.c), not on a VLAN sub-interface.
 *
 * The shared commands `ip`, `netmask` and `gw` only store into uip_hostaddr, uip_netmask and
 * uip_draddr (cmd_parser.c:1519..1577; the 8051's uIP stack reads them), and `ip dhcp` calls
 * dhcp_start(). So mgmt_apply(), which the platform calls after every command run, compares those
 * three with what the kernel was last given, and applies a difference with RTM_NEWADDR / RTM_NEWROUTE
 * (every address of swbr is removed first; if the kernel refuses the new one the old one is put
 * back). The result is read back from the kernel into the three variables, so what `ip` prints and
 * information.json reports is what is configured. A failure prints an error through cmd_error().
 *   - Netmask 0.0.0.0 (nothing set yet) gives a /32. A netmask or gateway typed while there is no
 *     address stays stored and applies with the address. A gateway that is typed and not reachable
 *     through the address and netmask in force (which did not change in the same run of commands) is
 *     an error (the kernel refuses the route) and the old one stays. A gateway that is not reachable
 *     because the address or netmask moved in the same run, or moved away from it, is kept (stored,
 *     reported by `gw`, no route) with a warning, and the route comes back when a later change makes
 *     it reachable. The default route is only ever swbr's own.
 *
 * DHCP is BusyBox `udhcpc -i swbr -f -S -s <script>`, a child of the daemon. The script
 * (udhcpc.script next to this file; --dhcp-script) puts the lease on swbr, valid for the lease
 * time and "preferred" for half of it, which is what `ip` prints as the renewal time. The daemon
 * reads the lease back once a second (mgmt_service) into the same three variables.
 *   - `ip dhcp` starts the new client FIRST and only when it runs takes the static address away: a
 *     client that cannot be started (no busybox) leaves the address, the routes and the old client
 *     as they were and reports the error.
 *   - The child is reaped as soon as it exits (SIGCHLD wakes the poll loop; waitpid(WNOHANG)).
 *     While DHCP is on and the child is gone it is started again after 5 s; if it cannot be
 *     started (no busybox), DHCP is switched off and the error logged.
 *   - It is stopped by `ip <address>` (which the shared code answers with dhcp_stop) and by `ip dhcp`
 *     (a fresh start): SIGTERM at once to its process group (the client is its own group leader: the lease
 *     script it may be running goes with it), the reaping and, after KILL_MS, the SIGKILL are done by
 *     mgmt_service, so the poll loop never waits for it. The address that `ip <address>` sets is
 *     applied to the kernel when the client has exited (mgmt_service; at once if there was none), not
 *     before: a client that has its ACK and not yet seen the SIGTERM still runs its lease script, which
 *     would flush the new address. `ip` shows the new address immediately. When the daemon exits (mgmt_exit) the
 *     clients are stopped the same way, but there it waits for them (at most about KILL_MS): a
 *     re-executed daemon must not meet its predecessor's client. If the daemon is killed, the
 *     kernel's parent-death signal ends them. The lease stays on swbr until it runs out.
 *   - The only wait in the loop: starting a client waits until it has exec'd busybox (it reports a
 *     failed exec through a pipe), at most EXEC_WAIT_MS.
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nl.h"
#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"
#include "uip/uip.h"
#include "dhcp.h"

#define RESPAWN_MS 5000
#define KILL_MS 1000		/* SIGTERM to SIGKILL */
#define EXEC_WAIT_MS 200
#define LEASE_POLL_MS 1000
#define MAX_ADDRS 16

extern __xdata uip_ipaddr_t uip_hostaddr, uip_netmask, uip_draddr;
extern __xdata struct uip_eth_addr uip_ethaddr;
extern __xdata struct dhcp_state dhcp_state;
extern __xdata uint16_t management_vlan;

/* what the kernel holds for swbr, IPv4, network byte order */
struct kip {
	uint32_t host, mask, gw;
	uint32_t preferred;		/* seconds of the lease left until renewal; UINT32_MAX = forever */
};

static const char *script = "/usr/share/rtlplayground/udhcpc.script";
static struct kip acc;		/* the last state the kernel was given, or the lease taken from it */
static pid_t dhcp_pid;
static pid_t dying_pid;		/* a client that has been sent SIGTERM and is not reaped yet */
static bool apply_pending;	/* mgmt_apply() found a change to make while a client was dying */
static int64_t respawn_at, poll_at, kill_at;

static uint32_t rd(const uip_ipaddr_t a)
{
	uint32_t v;

	memcpy(&v, a, 4);
	return v;
}

static void wr(uip_ipaddr_t a, uint32_t v)
{
	memcpy(a, &v, 4);
}

static uint32_t mask_of(unsigned plen)
{
	return plen ? htonl(~0u << (32 - plen)) : 0;
}

/* ---- the kernel's view of swbr ---- */

struct kview {
	int ifx;
	int n;
	uint32_t local[MAX_ADDRS];
	uint8_t plen[MAX_ADDRS];
	uint32_t preferred;		/* of the first address */
	uint32_t gw;
};

static int addr_cb(const struct nlmsghdr *nlh, void *data)
{
	struct kview *v = data;
	const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(nlh);
	struct nlattr *attr;
	uint32_t local = 0, pref = UINT32_MAX;

	if (ifa->ifa_family != AF_INET || (int)ifa->ifa_index != v->ifx || v->n == MAX_ADDRS)
		return MNL_CB_OK;
	mnl_attr_for_each(attr, nlh, sizeof *ifa) {
		if (mnl_attr_get_type(attr) == IFA_LOCAL && mnl_attr_get_payload_len(attr) == 4)
			memcpy(&local, mnl_attr_get_payload(attr), 4);
		else if (mnl_attr_get_type(attr) == IFA_CACHEINFO && mnl_attr_get_payload_len(attr) >= sizeof(struct ifa_cacheinfo))
			pref = ((const struct ifa_cacheinfo *)mnl_attr_get_payload(attr))->ifa_prefered;
	}
	if (!v->n)
		v->preferred = pref;
	v->local[v->n] = local;
	v->plen[v->n++] = ifa->ifa_prefixlen;
	return MNL_CB_OK;
}

static int route_cb(const struct nlmsghdr *nlh, void *data)
{
	struct kview *v = data;
	const struct rtmsg *rtm = mnl_nlmsg_get_payload(nlh);
	struct nlattr *attr;
	uint32_t gw = 0;
	int oif = 0;

	if (rtm->rtm_family != AF_INET || rtm->rtm_dst_len || rtm->rtm_table != RT_TABLE_MAIN || v->gw)
		return MNL_CB_OK;
	mnl_attr_for_each(attr, nlh, sizeof *rtm) {
		if (mnl_attr_get_type(attr) == RTA_OIF && mnl_attr_get_payload_len(attr) == 4)
			memcpy(&oif, mnl_attr_get_payload(attr), 4);
		else if (mnl_attr_get_type(attr) == RTA_GATEWAY && mnl_attr_get_payload_len(attr) == 4)
			memcpy(&gw, mnl_attr_get_payload(attr), 4);
	}
	if (oif == v->ifx)
		v->gw = gw;
	return MNL_CB_OK;
}

static int kernel_view(struct kview *v, bool routes)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_GETADDR, 0);
	struct ifaddrmsg *ifa = mnl_nlmsg_put_extra_header(nlh, sizeof *ifa);
	int rc;

	memset(v, 0, sizeof *v);
	v->ifx = if_nametoindex(SWBR);
	if (!v->ifx)
		return -ENODEV;
	ifa->ifa_family = AF_INET;
	rc = nl_dump(nlh, addr_cb, v);
	if (!rc && routes) {
		struct rtmsg *rtm;

		nlh = nl_msg(buf, RTM_GETROUTE, 0);
		rtm = mnl_nlmsg_put_extra_header(nlh, sizeof *rtm);
		rtm->rtm_family = AF_INET;
		rc = nl_dump(nlh, route_cb, v);
	}
	return rc;
}

static int kernel_ip(struct kip *k)
{
	struct kview v;
	int rc = kernel_view(&v, true);

	memset(k, 0, sizeof *k);
	if (rc)
		return rc;
	if (v.n) {
		k->host = v.local[0];
		k->mask = mask_of(v.plen[0]);
		k->gw = v.gw;
		k->preferred = v.preferred;
	}
	return 0;
}

static int flush_addrs(void)
{
	struct kview v;
	int rc = kernel_view(&v, false);

	for (int i = 0; !rc && i < v.n; i++) {
		char buf[NL_MSG_MAX];
		struct nlmsghdr *nlh = nl_msg(buf, RTM_DELADDR, 0);
		struct ifaddrmsg *ifa = mnl_nlmsg_put_extra_header(nlh, sizeof *ifa);

		ifa->ifa_family = AF_INET;
		ifa->ifa_prefixlen = v.plen[i];
		ifa->ifa_index = v.ifx;
		mnl_attr_put_u32(nlh, IFA_LOCAL, v.local[i]);
		rc = nl_talk(nlh);
	}
	return rc;
}

static int add_addr(uint32_t host, uint8_t plen)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL);
	struct ifaddrmsg *ifa = mnl_nlmsg_put_extra_header(nlh, sizeof *ifa);

	ifa->ifa_family = AF_INET;
	ifa->ifa_prefixlen = plen;
	ifa->ifa_index = if_nametoindex(SWBR);
	if (!ifa->ifa_index)
		return -ENODEV;
	mnl_attr_put_u32(nlh, IFA_LOCAL, host);
	mnl_attr_put_u32(nlh, IFA_ADDRESS, host);
	if (plen < 31)
		mnl_attr_put_u32(nlh, IFA_BROADCAST, host | ~mask_of(plen));
	return nl_talk(nlh);
}

/* The default route of swbr. APPEND, not REPLACE: the kernel's REPLACE swaps whatever default route
 * has the same metric, also one through another interface, which is not ours to touch. */
static int add_route(uint32_t gw)
{
	char buf[NL_MSG_MAX];
	struct nlmsghdr *nlh = nl_msg(buf, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_APPEND);
	struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof *rtm);

	rtm->rtm_family = AF_INET;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_STATIC;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_type = RTN_UNICAST;
	mnl_attr_put_u32(nlh, RTA_GATEWAY, gw);
	mnl_attr_put_u32(nlh, RTA_OIF, if_nametoindex(SWBR));
	return nl_talk(nlh);
}

/* a state the kernel holds becomes the state of the shared variables */
static void adopt(const struct kip *k)
{
	acc = *k;
	wr(uip_hostaddr, k->host);
	wr(uip_netmask, k->mask);
	wr(uip_draddr, k->gw);
}

/* ---- static addresses ---- */

void mgmt_apply(void)
{
	struct kip want = { rd(uip_hostaddr), rd(uip_netmask), rd(uip_draddr), UINT32_MAX }, got;
	struct kview old;
	uint32_t m, prev_gw = acc.gw;
	bool restore = false, moved = want.host != acc.host || want.mask != acc.mask;
	int rc;

	if (want.host == acc.host && want.mask == acc.mask && want.gw == acc.gw)
		return;
	if (dying_pid) {
		/* The client that is being stopped may be in the middle of its lease script, or about to run it
		 * (it has the ACK and not yet seen the SIGTERM): that script would flush the address set now.
		 * mgmt_service applies the change as soon as the client has exited. */
		apply_pending = true;
		return;
	}
	if (dhcp_state.state != DHCP_OFF) {
		cmd_error("ip, netmask and gw come from DHCP; 'ip <address>' stops it first\n");
		adopt(&acc);
		return;
	}
	m = ntohl(want.mask);
	if (~m & (~m + 1)) {	/* ones that are not one run from the top */
		cmd_error("the netmask must be a run of one bits\n");
		wr(uip_netmask, acc.mask);
		return;
	}
	rc = kernel_view(&old, true);
	if (!rc)
		rc = flush_addrs();
	if (rc) {
		nl_error("cannot change the address of " SWBR, rc);
		adopt(&acc);
		return;
	}
	if (!want.host) {	/* netmask and gateway wait for an address */
		acc = want;
		return;
	}
	rc = add_addr(want.host, want.mask ? __builtin_popcount(m) : 32);
	if (rc) {
		nl_error("cannot set the address of " SWBR, rc);
		/* the kernel refused the new address: put the old one back */
		for (int i = 0; i < old.n; i++)
			add_addr(old.local[i], old.plen[i]);
		restore = true;
	} else if (want.gw && (rc = add_route(want.gw))) {
		if (want.gw != prev_gw && !moved) {	/* a gateway that was just typed, the address as it was: refused, the old one stays */
			nl_error("cannot set the gateway", rc);
			restore = true;
		} else {	/* it does not fit the address and netmask that came with it or that moved away from it:
			 * it stays, and is applied as soon as an address change makes it reachable */
			print_string("Warning: the gateway is not reachable through this address and netmask; it is kept"
				     " and applied as soon as it is\n");
		}
	}
	if (restore && old.n && prev_gw && old.gw == prev_gw)
		add_route(prev_gw);
	rc = kernel_ip(&got);
	if (rc) {
		nl_error("cannot read the address of " SWBR, rc);
		return;
	}
	adopt(&got);
	/* the gateway the user has is the one in force, or the one waiting for the address to fit */
	if (restore ? prev_gw : want.gw) {
		got.gw = restore ? prev_gw : want.gw;
		adopt(&got);
	}
}

/* ---- DHCP ---- */

/* Starts a client; its pid in *out. -errno if it cannot be started (fork, or busybox is not there). */
static int spawn_udhcpc(pid_t *out)
{
	int ep[2], err = 0, pr;
	pid_t pid;
	ssize_t n;
	struct pollfd p;

	if (pipe2(ep, O_CLOEXEC))
		return -errno;
	pid = fork();
	if (pid < 0) {
		err = -errno;
		close(ep[0]);
		close(ep[1]);
		return err;
	}
	if (!pid) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		setpgid(0, 0);	/* its own group: the signals that stop it reach the lease script it is running, too */
		signal(SIGPIPE, SIG_DFL);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
		}
		execlp("busybox", "busybox", "udhcpc", "-i", SWBR, "-f", "-S", "-s", script, (char *)NULL);
		err = errno;
		n = write(ep[1], &err, sizeof err);
		(void)n;
		_exit(127);
	}
	close(ep[1]);
	setpgid(pid, pid);	/* (also done by the child; whichever is first) */
	/* The pipe closes when the child execs and carries an errno if the exec failed. An exec that is
	 * not done after EXEC_WAIT_MS is taken as good; if it fails then, the child ends and is started
	 * again like any client that ends (mgmt_service). */
	p = (struct pollfd){ .fd = ep[0], .events = POLLIN };
	do
		pr = poll(&p, 1, EXEC_WAIT_MS);
	while (pr < 0 && errno == EINTR);
	n = pr > 0 ? read(ep[0], &err, sizeof err) : 0;
	close(ep[0]);
	if (n == sizeof err) {
		waitpid(pid, NULL, 0);
		return -err;
	}
	*out = pid;
	return 0;
}

/* Stops a client: SIGTERM now, the reaping and the SIGKILL in mgmt_service. One client dies at a
 * time: if another is still dying (two restarts within KILL_MS) it is killed and reaped first. */
static void stop_udhcpc(pid_t pid)
{
	if (dying_pid) {
		kill(-dying_pid, SIGKILL);
		waitpid(dying_pid, NULL, 0);
	}
	kill(-pid, SIGTERM);
	dying_pid = pid;
	kill_at = now_ms() + KILL_MS;
}

static void reap_dying(int64_t now)
{
	if (!dying_pid)
		return;
	if (waitpid(dying_pid, NULL, WNOHANG)) {	/* reaped (or not ours any more) */
		dying_pid = 0;
	} else if (now >= kill_at) {
		/* shortcut: a child that ignores SIGTERM for KILL_MS is killed without a lease release */
		kill(-dying_pid, SIGKILL);
		kill_at = now + KILL_MS;	/* reaped by the next call; SIGCHLD wakes the loop */
	}
}

/* The daemon ends or re-executes: both clients are gone when this returns (a client that has not
 * taken SIGTERM within KILL_MS is killed). */
static void kill_udhcpc_wait(void)
{
	pid_t pids[2] = { dhcp_pid, dying_pid };

	dhcp_pid = dying_pid = 0;
	for (int i = 0; i < 2; i++)
		if (pids[i])
			kill(-pids[i], SIGTERM);
	for (int i = 0; i < 2; i++) {
		for (int t = 0; pids[i] && t < KILL_MS / 10; t++) {
			if (waitpid(pids[i], NULL, WNOHANG)) {
				pids[i] = 0;
				break;
			}
			usleep(10000);
		}
		if (pids[i]) {
			kill(-pids[i], SIGKILL);
			waitpid(pids[i], NULL, 0);
		}
	}
}

void dhcp_start(void) __banked
{
	struct kip k;
	pid_t pid;
	int rc = spawn_udhcpc(&pid);

	if (rc) {
		/* nothing was touched: the static address, the routes and a running client are as they were */
		nl_error("cannot start the DHCP client", rc);
		return;
	}
	apply_pending = false;	/* the address that waited for the old client is not wanted any more */
	/* the old client (and a lease script it is running) is told to stop before the address is taken away */
	if (dhcp_pid)
		stop_udhcpc(dhcp_pid);
	dhcp_pid = pid;
	rc = flush_addrs();
	if (rc)
		nl_error("cannot take the address of " SWBR " away", rc);	/* the client runs and replaces it */
	/* no address until a lease arrives; what `ip` shows is what is on swbr */
	if (kernel_ip(&k))
		memset(&k, 0, sizeof k);
	adopt(&k);
	memset(dhcp_state.current_ip, 0, 4);
	dhcp_state.dhcp_timer = 0;
	dhcp_state.state = DHCP_START;
	respawn_at = 0;
	poll_at = now_ms() + LEASE_POLL_MS;
}

void dhcp_stop(void) __banked
{
	if (dhcp_pid)
		stop_udhcpc(dhcp_pid);
	dhcp_pid = 0;
	dhcp_state.state = DHCP_OFF;
	memset(&acc, 0, sizeof acc);	/* the kernel holds a lease: the next static address is applied for sure */
}

static void lease_poll(void)
{
	struct kip k;

	if (kernel_ip(&k))
		return;
	if (k.host) {
		adopt(&k);
		memcpy(dhcp_state.current_ip, &k.host, 4);
		memcpy(dhcp_state.subnet, &k.mask, 4);
		memcpy(dhcp_state.router, &k.gw, 4);
		dhcp_state.dhcp_timer = k.preferred > UINT16_MAX ? 0 : k.preferred;
		dhcp_state.state = DHCP_LEASING;
	} else if (dhcp_state.state == DHCP_LEASING) {
		memset(&k, 0, sizeof k);
		adopt(&k);
		dhcp_state.state = DHCP_START;
	}
}

/* Called by the poll loop after every wake-up. */
void mgmt_service(int64_t now)
{
	int st = 0;

	reap_dying(now);
	if (apply_pending && !dying_pid) {
		apply_pending = false;
		mgmt_apply();
	}
	if (dhcp_pid) {
		pid_t r = waitpid(dhcp_pid, &st, WNOHANG);

		if (r) {
			fprintf(stderr, "rtlplayground: udhcpc ended (status 0x%x), restarting in %d s\n", st, RESPAWN_MS / 1000);
			dhcp_pid = 0;
			respawn_at = now + RESPAWN_MS;
		}
	}
	if (dhcp_state.state == DHCP_OFF)
		return;
	if (!dhcp_pid && now >= respawn_at) {
		int rc = spawn_udhcpc(&dhcp_pid);

		if (rc) {
			fprintf(stderr, "rtlplayground: cannot start udhcpc: %s; DHCP is off\n", strerror(-rc));
			dhcp_state.state = DHCP_OFF;
			return;
		}
	}
	if (now >= poll_at) {
		poll_at = now + LEASE_POLL_MS;
		lease_poll();
	}
}

static void on_sigchld(int sig)
{
	/* only here to interrupt poll(): the child is reaped in mgmt_service */
}

void mgmt_exit(void)
{
	kill_udhcpc_wait();
}

void mgmt_init(const char *dhcp_script)
{
	struct sigaction sa = { .sa_handler = on_sigchld, .sa_flags = SA_RESTART | SA_NOCLDSTOP };
	uint8_t mac[6];
	int rc;

	if (dhcp_script)
		script = dhcp_script;
	sigaction(SIGCHLD, &sa, NULL);
	/* a restart starts without an address, as a booting 8051 does: the configuration gives it */
	rc = flush_addrs();
	if (!rc)
		rc = bridge_mac_get(mac);
	if (!rc) {
		memcpy(uip_ethaddr.addr, mac, 6);
		rc = bridge_mac_set(mac);	/* pins it */
	}
	if (rc)
		fprintf(stderr, "rtlplayground: management interface " SWBR ": %s\n", strerror(-rc));
}

/* ---- the management VLAN and MAC ---- */

/* The shared code moves the management VLAN with a remove for the old VLAN and an add for the new
 * one, and the MAC the same way (cmd_parser.c:473, 618). The bridge's own entry for the VLAN is
 * the management entry; VLAN 0 means "off", where the 8051 lets untagged management resolve in
 * VLAN 1 (rtl837x_port.c:1162). */
void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked
{
	static uint16_t last_vlan = 1;	/* management_vlan as the last successful add left it */
	int rc;

	if (!vlan)
		vlan = 1;
	if (!remove_entry && bridge_vid_reserved(vlan)) {
		/* the shared code has already removed the old entry and stored the new VLAN: undo both */
		char w[80];

		snprintf(w, sizeof w, "VLAN %u is reserved for a parked port, it cannot manage the switch\n", vlan);
		cmd_error(w);
		management_vlan = last_vlan;
		rc = bridge_mgmt_vlan(last_vlan ? last_vlan : 1, true);
		if (rc)
			nl_error("cannot restore the management entry of " SWBR, rc);
		return;
	}
	if (remove_entry) {
		rc = bridge_mgmt_vlan(vlan, false);
	} else {
		uint8_t cur[6];
		int mac_rc = bridge_mac_get(cur);

		if (!mac_rc && memcmp(cur, mac, 6) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
			mac_rc = bridge_mac_set(mac);
			if (mac_rc)
				memcpy(mac, cur, 6);	/* the shared variable keeps what the bridge has */
		}
		if (mac_rc)
			nl_error("cannot set the MAC address of " SWBR, mac_rc);
		/* the management entry is moved whatever became of the MAC: the console must not be left
		 * without its VLAN because the address was refused */
		rc = bridge_mgmt_vlan(vlan, true);
		if (!rc)
			last_vlan = management_vlan;
	}
	if (rc)
		nl_error("cannot set the management entry of " SWBR, rc);
}
