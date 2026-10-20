#!/bin/bash
# Bridge VLANs, PVIDs, ingress modes and the management VLAN and IP: the commands of the shared
# parser change the kernel's bridge (swbr), and the VLAN pages, `vlan show` and `bridge vlan show`
# tell the same. Then DHCP (a BusyBox udhcpd in host 1) and static addresses on swbr, and the
# daemon's lifecycle: restart adopts the bridge, the DHCP client is reaped and killed.
. "$(dirname "$0")/lib.sh"
SW_PASS=benchpass
jar=$RUN/cookies
dpid=
trap '[[ -z $dpid ]] || kill "$dpid" 2>/dev/null' EXIT
. "$(dirname "$0")/vlan_lib.sh"
DHCP_SCRIPT=$RUN/udhcpc.script	# a copy the test can change
mkdir -p "$RUN" && cp "$BENCH_DIR/../udhcpc.script" "$DHCP_SCRIPT" && chmod +x "$DHCP_SCRIPT" || exit 1
sw_start
sw_cmd "passwd $SW_PASS" >/dev/null
# ---- the default configuration: VLAN 1, every port an untagged member with PVID 1, the bridge too ----
assert_eq "swbr: vlan_filtering, default pvid, STP state" "1 0 0" \
	"$(rtlp_sw cat /sys/class/net/swbr/bridge/vlan_filtering /sys/class/net/swbr/bridge/default_pvid /sys/class/net/swbr/bridge/stp_state | tr '\n' ' ' | sed 's/ $//')"
for n in 1 2 3 4 5 6; do
	assert_eq "lan$n is a port of swbr" ../swbr "$(rtlp_sw readlink /sys/class/net/lan$n/master)"
done
assert_eq "default: bridge vlan show" "$(table "$(ports 1 6 1:P:U)" "swbr 1:P:U")" "$(bv)"
assert_eq "default: /vlan.json?vid=1 (members 0x23f all untagged, PVID of 6 ports)" \
	'{"members":"0x208fe3f","name":"","pvid":"0x003f"}' "$(vlan_json 1)"
assert_eq "default: /vlanlist" '{"mgmt":1,"vlan":[{"id":1,"name":""}]}' "$(sw_get vlanlist)"
assert_eq "no VLAN 2" '{"members":"0x0","name":"","pvid":"0x0000"}' "$(vlan_json 2)"
out=$(sw_cmd "vlan show")
assert_contains "vlan show header" $'Port\tPVID\tType\tFiltering' "$out"
assert_contains "vlan show: port 1" $'1\t0x0001\tAny\tEnabled' "$out"
assert_contains "vlan show: CPU" $'CPU\t0x0001\tAny\tEnabled' "$out"
ping_ok 1 192.0.2.12 || { echo "default: host1 does not reach host2"; exit 1; }
[[ $(<"$RUN/daemon.log") != *"Not supported"* ]] || { echo "a Not supported line at boot"; exit 1; }
assert_contains "the default configuration ran" "Management VLAN set to 0x0001" "$REPLY_RAW"
[[ $REPLY_RAW != *"Not supported"* ]] || { echo "a Not supported line on the console at boot: $REPLY_RAW"; exit 1; }

# ---- vlan 10: port 1 untagged (PVID 10), port 2 tagged; host 3 stays in VLAN 1 ----
in_host 1 ip addr add 198.51.100.1/24 dev h1
in_host 2 ip link add link h2 name h2.10 type vlan id 10 && in_host 2 ip addr add 198.51.100.2/24 dev h2.10 && in_host 2 ip link set h2.10 up || exit 1
in_host 3 ip addr add 198.51.100.3/24 dev h3
out=$(sw_cmd "vlan 1 2 3 4 5 6")
assert_contains "vlan 1 without port 1" "vlan: 0x0001, members: 0x023e, tagged: 0x0200" "$out"
out=$(sw_cmd "vlan 10 lab 1 2t")
assert_contains "vlan 10 prints as the 8051 does" "vlan: 0x000a, members: 0x0203, tagged: 0x0202" "$out"
assert_eq "port 1 left VLAN 1 and joined VLAN 10, its PVID register still says 1: it is parked" "$(table "lan1 10:U $(pk 1):P:U" "lan2 1:P:U 10" "$(ports 3 6 1:P:U)" "swbr 1:P:U 10")" "$(bv)"
sw_cmd "pvid 1 10" >/dev/null
assert_eq "bridge vlan show" "$(table "lan1 10:P:U" "lan2 1:P:U 10" "$(ports 3 6 1:P:U)" "swbr 1:P:U 10")" "$(bv)"
assert_eq "vlan 10: ports 1 and 2 and the CPU, only port 1 untagged, PVID of port 1" '{"members":"0x2000603","name":"lab","pvid":"0x0001"}' "$(vlan_json 10)"
assert_eq "vlan 1 without port 1" '{"members":"0x208fa3e","name":"","pvid":"0x003e"}' "$(vlan_json 1)"
assert_eq "/vlanlist" '{"mgmt":1,"vlan":[{"id":1,"name":""},{"id":10,"name":"lab"}]}' "$(sw_get vlanlist)"
ping_ok 1 198.51.100.2 || { echo "vlan 10: untagged host1 does not reach tagged host2"; exit 1; }
ping_ok 2 198.51.100.1 || { echo "vlan 10: tagged host2 does not reach untagged host1"; exit 1; }
ping_ok 3 198.51.100.1 && { echo "vlan 10: host3 reaches host1"; exit 1; }
ping_ok 3 198.51.100.2 && { echo "vlan 10: host3 reaches host2"; exit 1; }
ping_ok 2 192.0.2.13 || { echo "vlan 1: host2 does not reach host3"; exit 1; }

# ---- a PVID is a register: it names a VLAN the port may not be in yet ----
sw_cmd "pvid 3 10" >/dev/null
assert_eq "pvid 3 10, port 3 not in VLAN 10: parked in its own VLAN, untagged ingress dropped" "lan3 1:U $(pk 3):P:U" "$(bv | grep '^lan3 ')"
assert_eq "...host 3's untagged frames reach nobody" 0 "$(frames_seen 3 2)"
assert_contains "...vlan show still says 10" $'3\t0x000a\tAny' "$(sw_cmd 'vlan show')"
sw_cmd "vlan 10 lab 1 2t 3" >/dev/null
assert_eq "joining VLAN 10 gives port 3 its PVID" "lan3 1:U 10:P:U" "$(bv | grep '^lan3 ')"
assert_eq "...PVID mask of VLAN 10: ports 1 and 3" '"pvid":"0x0005"' "$(vlan_json 10 | grep -o '"pvid":"[^"]*"')"
ping_ok 3 198.51.100.1 || { echo "pvid 3 10: host3 does not reach host1 in VLAN 10"; exit 1; }
sw_cmd "pvid 3 1" >/dev/null
assert_eq "pvid 3 1 moves it back" "lan3 1:P:U 10:U" "$(bv | grep '^lan3 ')"
ping_ok 3 198.51.100.1 && { echo "pvid 3 1: host3 still reaches host1"; exit 1; }

# ---- Linux bridge pitfall (OpenWrt #25993): a port without a PVID must drop untagged frames, not put them in VID 0 ----
assert_eq "control: host 4's frames reach host 5, host 6 and the bridge" "5 5 5" "$(frames_seen 4 5) $(frames_seen 4 6) $(frames_seen 4 sw)"
sw_cmd "pvid 4 99" >/dev/null
assert_eq "pvid 4 99: port 4 is parked" "lan4 1:U $(pk 4):P:U" "$(bv | grep '^lan4 ')"
assert_eq "parked: host 4's untagged frames reach nobody (host 5)" 0 "$(frames_seen 4 5)"
assert_eq "parked: ...nor host 6 (no VID 0 leak)" 0 "$(frames_seen 4 6)"
assert_eq "parked: ...nor the bridge itself" 0 "$(frames_seen 4 sw)"
# the parking VLANs (4089..4094 on this board) are not VLANs: not on any page, not usable
assert_eq "vlan.json of port 4's parking VLAN: no VLAN" '{"members":"0x0","name":"","pvid":"0x0000"}' "$(vlan_json "$(pk 4)")"
list=$(sw_get vlanlist)
for n in 1 2 3 4 5 6; do [[ $list != *"\"id\":$(pk $n),"* && $list != *"\"id\":$(pk $n)}"* ]] || { echo "parking VLAN $(pk $n) is in /vlanlist: $list"; exit 1; }; done
assert_eq "/vlanlist while a port is parked" '{"mgmt":1,"vlan":[{"id":1,"name":""},{"id":10,"name":"lab"}]}' "$list"
out=$(sw_cmd "vlan show")
[[ $out != *0x0ff* ]] || { echo "a parking VLAN is in vlan show: $out"; exit 1; }
assert_contains "vlan show still shows the register of port 4" $'4\t0x0063\tAny' "$out"
assert_contains "vlan 4094 is refused" "Error: this VLAN is reserved here" "$(sw_cmd 'vlan 4094 1')"
assert_contains "vlan 4089 is refused" "Error: this VLAN is reserved here" "$(sw_cmd 'vlan 4089 1')"
assert_contains "vlan 4094 foo is refused" "Error: this VLAN is reserved here" "$(sw_cmd 'vlan 4094 foo 1')"
assert_eq "...and leaves no name behind" '{"members":"0x0","name":"","pvid":"0x0000"}' "$(vlan_json 4094)"
assert_contains "vlan 4094 d is refused" "Error: this VLAN is reserved here" "$(sw_cmd 'vlan 4094 d')"
assert_contains "pvid 1 4094 is refused" "Error: this VLAN is reserved here" "$(sw_cmd 'pvid 1 4094')"
assert_eq "...port 1 is as it was" "lan1 10:P:U" "$(bv | grep '^lan1 ')"
assert_contains "vlan 4088 is the highest user VLAN" "vlan: 0x0ff8, members: 0x0202, tagged: 0x0200" "$(sw_cmd 'vlan 4088 2')"
assert_contains "...it shows" '{"id":4088,"name":""}' "$(sw_get vlanlist)"
sw_cmd "vlan 4088 d" >/dev/null
sw_cmd "pvid 4 1" >/dev/null
assert_eq "pvid 4 1: PVID back, the parking entry is gone" "lan4 1:P:U" "$(bv | grep '^lan4 ')"
assert_eq "...and the frames flow again" 5 "$(frames_seen 4 5)"

# ---- ingress modes ----
assert_eq "ingress 5t" "Port 5 ingress filter set to: Tagged" "$(sw_cmd "ingress 5t")"
assert_eq "tagged only: port 5 keeps its VLAN and is parked" "lan5 1:U $(pk 5):P:U" "$(bv | grep '^lan5 ')"
assert_contains "vlan show: Tagged" $'5\t0x0001\tTagged\tEnabled' "$(sw_cmd 'vlan show')"
assert_eq "tagged only: untagged frames of host 5 are dropped" 0 "$(frames_seen 5 6)"
in_host 5 ip link add link h5 name h5.1 type vlan id 1 && in_host 5 ip link set h5.1 up || exit 1
assert_eq "tagged only: tagged frames of host 5 (VLAN 1) pass" 5 "$(frames_seen 5 6 h5.1)"
assert_eq "ingress 5a" "Port 5 ingress filter set to: Any" "$(sw_cmd "ingress 5a")"
assert_eq "any: port 5 has its PVID back" "lan5 1:P:U" "$(bv | grep '^lan5 ')"
assert_eq "any: untagged frames pass" 5 "$(frames_seen 5 6)"
out=$(sw_cmd "ingress 5u")
assert_contains "untagged only cannot be held by a bridge" "Error: a bridge cannot drop the tagged frames of a port" "$out"
assert_contains "...and the shared command says so" "Error: cannot set the ingress filter for port 5" "$out"
assert_contains "...the mode did not change" $'5\t0x0001\tAny\tEnabled' "$(sw_cmd 'vlan show')"
assert_eq "...nor the bridge" "lan5 1:P:U" "$(bv | grep '^lan5 ')"
assert_contains "ingress t: all ports" "All ports ingress filter set to: Tagged" "$(sw_cmd 'ingress t')"
assert_eq "all ports tagged only: every port is parked in its own VLAN" "$(table "lan1 10:U $(pk 1):P:U" "lan2 1:U 10 $(pk 2):P:U" "lan3 1:U 10:U $(pk 3):P:U" "lan4 1:U $(pk 4):P:U" "lan5 1:U $(pk 5):P:U" "lan6 1:U $(pk 6):P:U" "swbr 1:P:U 10")" "$(bv)"
assert_eq "...untagged frames of host 4 reach neither parked host 5 nor parked host 6" "0 0" "$(frames_seen 4 5) $(frames_seen 4 6)"
assert_eq "...nor the bridge" 0 "$(frames_seen 4 sw)"
ping_ok 5 192.0.2.16 && { echo "two parked ports reach each other"; exit 1; }
sw_cmd "ingress a" >/dev/null
assert_eq "ingress a: back" "$(table "lan1 10:P:U" "lan2 1:P:U 10" "lan3 1:P:U 10:U" "$(ports 4 6 1:P:U)" "swbr 1:P:U 10")" "$(bv)"

# ---- the management VLAN: the bridge's own entry, untagged with PVID ----
assert_eq "vlan 20 mgmt before VLAN 20 exists" "Management VLAN set to 0x0014" "$(sw_cmd 'vlan 20 mgmt')"
assert_eq "...VLAN 1 on swbr is an ordinary tagged entry now" "swbr 1 10" "$(bv | grep '^swbr ')"
assert_eq "...and VLAN 20 does not exist" '{"members":"0x0","name":"","pvid":"0x0000"}' "$(vlan_json 20)"
assert_contains "/vlanlist mgmt" '{"mgmt":20,' "$(sw_get vlanlist)"
sw_cmd "vlan 20 6" >/dev/null
assert_eq "creating VLAN 20 makes it the untagged PVID entry of swbr" "swbr 1 10 20:P:U" "$(bv | grep '^swbr ')"
assert_eq "...with port 6 a member" "lan6 1:P:U 20:U" "$(bv | grep '^lan6 ')"
assert_eq "/vlan.json for VLAN 20: port 6 and the CPU, both untagged" '{"members":"0x2088220","name":"","pvid":"0x0000"}' "$(vlan_json 20)"
assert_eq "vlan 1 mgmt moves it back" "Management VLAN set to 0x0001" "$(sw_cmd 'vlan 1 mgmt')"
assert_eq "...VLAN 1 untagged PVID again, 20 tagged" "swbr 1:P:U 10 20" "$(bv | grep '^swbr ')"
assert_contains "vlan 0 mgmt" "Management VLAN disabled" "$(sw_cmd "vlan 0 mgmt")"
assert_eq "...untagged management resolves in VLAN 1, as on the 8051" "swbr 1:P:U 10 20" "$(bv | grep '^swbr ')"
assert_contains "/vlanlist mgmt 0" '{"mgmt":0,' "$(sw_get vlanlist)"
sw_cmd "vlan 1 mgmt" >/dev/null
assert_eq "vlan 20 d" "" "$(sw_cmd 'vlan 20 d')"
assert_eq "...VLAN 20 is gone from every port and the bridge" "$(table "lan1 10:P:U" "lan2 1:P:U 10" "lan3 1:P:U 10:U" "$(ports 4 6 1:P:U)" "swbr 1:P:U 10")" "$(bv)"
assert_eq "vlan 10 d" "" "$(sw_cmd 'vlan 10 d')"
assert_eq "/vlanlist after it: the name is gone too" '{"mgmt":1,"vlan":[{"id":1,"name":""}]}' "$(sw_get vlanlist)"

# ---- vlan_mgmt_removed_console_alive (Review Focus 1): no VLAN 1 anywhere, the management VLAN is VLAN 1 ----
sw_cmd "pvid 1 1" >/dev/null
# watch the kernel's VLAN events (a probe entry tells when the monitor is listening)
rtlp_sw timeout 10 stdbuf -oL bridge monitor all > "$RUN/mon.log" 2>&1 &
mon=$!
for _ in $(seq 100); do
	rtlp_sw bridge vlan add vid 99 dev lan6 && rtlp_sw bridge vlan del vid 99 dev lan6 || exit 1
	grep -q '^\[VLAN\]lan6 *99' "$RUN/mon.log" && break
	sleep 0.05
done
grep -q '^\[VLAN\]lan6 *99' "$RUN/mon.log" || { echo "the bridge monitor does not report"; exit 1; }
assert_eq "vlan 1 d" "" "$(sw_cmd 'vlan 1 d')"
sleep 0.3
kill "$mon" 2>/dev/null; wait "$mon" 2>/dev/null
for n in 2 3 4 5 6; do	# (port 1 is not in VLAN 1 any more)
	parked=$(grep -n "^\[VLAN\]lan$n *$(pk $n) PVID" "$RUN/mon.log" | head -1 | cut -d: -f1)
	gone=$(grep -n "^\[VLAN\]Deleted lan$n *1\$" "$RUN/mon.log" | head -1 | cut -d: -f1)
	[[ -n $parked && -n $gone ]] || { echo "lan$n: no park ($parked) or delete ($gone) event"; cat "$RUN/mon.log"; exit 1; }
	(( parked < gone )) || { echo "lan$n lost VLAN 1 (event $gone) before it was parked (event $parked): a moment without PVID"; exit 1; }
done
assert_eq "no VLAN at all: every port parked in its own VLAN, nothing on the bridge" "$(for n in 1 2 3 4 5 6; do echo "lan$n $(pk $n):P:U"; done)" "$(bv)"
assert_contains "swcli still answers" "Unknown command" "$(sw_cmd foo)"
assert_contains "the console still answers" "Unknown command" "$(sw_console foo)"
kill -0 "$(cat "$RUN/pid")" || { echo "the daemon died"; exit 1; }
assert_eq "/vlanlist: management VLAN 1, no VLAN" '{"mgmt":1,"vlan":[]}' "$(sw_get vlanlist)"
assert_contains "vlan show works" "Ingress VLAN configuration:" "$(sw_cmd 'vlan show')"
ping_ok 1 192.0.2.12 && { echo "hosts reach each other with no VLAN"; exit 1; }
assert_eq "no VLAN: host 1's untagged frames reach nobody" "0 0" "$(frames_seen 1 2) $(frames_seen 1 sw)"
sw_cmd "vlan 1 1 2 3 4 5 6" >/dev/null
assert_eq "recreated VLAN 1: the PVID registers are in force again, swbr is the management entry again" "$(table "$(ports 1 6 1:P:U)" "swbr 1:P:U")" "$(bv)"
ping_ok 1 192.0.2.12 || { echo "recreated VLAN 1: host1 does not reach host2"; exit 1; }

# ---- mac: the bridge's address ----
assert_contains "mac" "MAC set to: 00:11:22:33:44:55" "$(sw_cmd 'mac 00:11:22:33:44:55')"
assert_eq "swbr has the address" 00:11:22:33:44:55 "$(rtlp_sw cat /sys/class/net/swbr/address)"
assert_eq "mac unchanged" "MAC unchanged" "$(sw_cmd 'mac 00:11:22:33:44:55')"
assert_eq "swbr is still the management entry" "swbr 1:P:U" "$(bv | grep '^swbr ')"

# ---- DHCP: BusyBox udhcpd in host 1; the default configuration started the client already ----
kids() {	# "<pid> <state letter> <command line>" of the children of process $1
	local p st
	for p in /proc/[0-9]*; do
		st=$(sed 's/^.*) //' "$p/stat" 2>/dev/null) || continue
		[[ $(cut -d' ' -f2 <<<"$st") == "$1" ]] && printf '%s %s %s\n' "${p#/proc/}" "${st%% *}" "$(tr '\0' ' ' < "$p/cmdline")"
	done
}
kidcmds() { kids "$1" | cut -d" " -f3-; }
udhcpc_procs() { local p; for p in /proc/[0-9]*; do tr '\0' ' ' < "$p/cmdline" 2>/dev/null; echo; done | grep -c '[u]dhcpc -i swbr'; }
daemon=$(cat "$RUN/pid")
client="busybox udhcpc -i swbr -f -S -s $DHCP_SCRIPT "
assert_eq "one udhcpc child of the daemon" "$client" "$(kidcmds "$daemon")"
cat > "$RUN/udhcpd.conf" <<EOF
interface h1
start 192.0.2.101
end 192.0.2.110
option subnet 255.255.255.0
option router 192.0.2.11
option lease 60
lease_file $RUN/udhcpd.leases
pidfile $RUN/udhcpd.pid
EOF
: > "$RUN/udhcpd.leases"
# another interface's default route (dummy0, first in the table): nothing swbr does may touch it
rtlp_sw ip link add dummy0 type dummy && rtlp_sw ip addr add 203.0.113.2/24 dev dummy0 && rtlp_sw ip link set dummy0 up || { echo "no dummy interface"; exit 1; }
rtlp_sw ip route prepend default via 203.0.113.1 dev dummy0 metric 0 || exit 1
# `ip dhcp` takes a static address away at once (no server answers yet): the client starts from nothing.
# The script does nothing for this (its `deconfig` would flush the address, too, a moment later).
printf '#!/bin/sh\nexit 0\n' > "$DHCP_SCRIPT"
sw_cmd "ip 192.0.2.50" >/dev/null
assert_eq "a static address, no DHCP" "192.0.2.50/32" "$(addrs)"
sw_cmd "ip dhcp" >/dev/null
assert_eq "ip dhcp flushes the static address from swbr" "" "$(addrs)"
assert_eq "...and ip shows none" "Current IP: 0.0.0.0 (static)" "$(sw_cmd ip)"
cp "$BENCH_DIR/../udhcpc.script" "$DHCP_SCRIPT"
ip netns exec rtlp-h1 busybox udhcpd -f "$RUN/udhcpd.conf" 2>>"$RUN/udhcpd.log" &	# not in_host: $! must be the server itself
dpid=$!
sleep 0.5
sw_cmd "ip dhcp" >/dev/null	# starts a fresh client: no waiting for the old one's retry
ip_out=
for _ in $(seq 40); do
	ip_out=$(sw_cmd ip)
	[[ $ip_out == *"(dhcp"* ]] && break
	sleep 0.25
done
[[ $ip_out =~ ^Current\ IP:\ (192\.0\.2\.1[01][0-9])\ \(dhcp,\ renewal\ in\ sec:\ 0x001[b-e]\)$ ]] || { echo "no DHCP lease after 10 s: '$ip_out'"; cat "$RUN/daemon.log" "$RUN/udhcpd.log"; exit 1; }
lease=${BASH_REMATCH[1]}
assert_eq "swbr holds the lease" "$lease/24" "$(addrs)"
assert_eq "default route via the router of the lease" "default via 192.0.2.11 dev swbr" "$(defroute)"
info=$(sw_get information.json)
assert_contains "information.json: address" "\"ip_address\":\"$lease\"" "$info"
assert_contains "information.json: gateway" '"ip_gateway":"192.0.2.11"' "$info"
assert_contains "information.json: netmask" '"ip_netmask":"255.255.255.0"' "$info"
assert_contains "information.json: MAC" '"mac_address":"00:11:22:33:44:55"' "$info"
assert_eq "gw shows the router" "Current gw: 192.0.2.11" "$(sw_cmd gw)"
assert_eq "the lease's route is swbr's own: dummy0's default route is still there" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"
ping_ok 2 "$lease" || { echo "host2 does not reach the management address $lease"; exit 1; }
ping_ok 1 "$lease" || { echo "host1 does not reach the management address $lease"; exit 1; }
assert_contains "static values are refused while DHCP is on" "Error: ip, netmask and gw come from DHCP" "$(sw_cmd 'gw 192.0.2.99')"
assert_eq "...and gw is still the lease's" "Current gw: 192.0.2.11" "$(sw_cmd gw)"

# the child is reaped when it dies, and started again after 5 s
cpid=$(kids "$daemon" | cut -d' ' -f1)
[[ $cpid =~ ^[0-9]+$ ]] || { echo "cannot find the udhcpc child: '$cpid'"; exit 1; }
kill -KILL "$cpid"
sleep 1.2
assert_eq "the killed client is reaped (no zombie, no child)" "" "$(kids "$daemon")"
assert_contains "the daemon says it restarts it" "udhcpc ended" "$(cat "$RUN/daemon.log")"
for _ in $(seq 40); do [[ -n $(kids "$daemon") ]] && break; sleep 0.25; done
assert_eq "restarted after 5 s: one new child" "$client" "$(kidcmds "$daemon")"
for _ in $(seq 40); do [[ $(sw_cmd ip) == *"(dhcp"* ]] && break; sleep 0.25; done
assert_contains "the lease is shown again" "(dhcp, renewal in sec:" "$(sw_cmd ip)"

# a static address: the client is gone (reaped), the address, mask and gateway are on swbr
assert_eq "ip <addr> prints as on the 8051" "Setting ip: 192.0.2.50" "$(sw_cmd 'ip 192.0.2.50')"
assert_eq "...the client is stopped and reaped" "" "$(kids "$daemon")"
assert_eq "...the lease is gone, the address has the lease's netmask" "192.0.2.50/24" "$(addrs)"
assert_eq "ip" "Current IP: 192.0.2.50 (static)" "$(sw_cmd ip)"
sw_cmd "netmask 0.0.0.0" >/dev/null
assert_eq "netmask 0.0.0.0 (none): a /32" "192.0.2.50/32" "$(addrs)"
sw_cmd "netmask 255.255.255.0" >/dev/null
assert_eq "netmask: /24" "192.0.2.50/24" "$(addrs)"
sw_cmd "gw 192.0.2.11" >/dev/null
assert_eq "gw: default route" "default via 192.0.2.11 dev swbr" "$(defroute)"
info=$(sw_get information.json)
assert_contains "information.json: static address" '"ip_address":"192.0.2.50","ip_gateway":"192.0.2.11","ip_netmask":"255.255.255.0"' "$info"
ping_ok 1 192.0.2.50 || { echo "host1 does not reach the static address"; exit 1; }
assert_contains "an unreachable gateway is an error" "Error: cannot set the gateway: Network is unreachable" "$(sw_cmd 'gw 203.0.113.1')"
assert_eq "...gw is still the old one" "Current gw: 192.0.2.11" "$(sw_cmd gw)"
assert_eq "...and so is the route" "default via 192.0.2.11 dev swbr" "$(defroute)"
assert_eq "...and the address" "192.0.2.50/24" "$(addrs)"
assert_contains "a netmask with a hole is an error" "Error: the netmask must be a run of one bits" "$(sw_cmd 'netmask 255.0.255.0')"
assert_eq "...netmask is still /24" "Current netmask: 255.255.255.0" "$(sw_cmd netmask)"
assert_eq "...on swbr too" "192.0.2.50/24" "$(addrs)"
# a gateway the address or netmask moves away from is kept, with a warning, and comes back with the route
out=$(sw_cmd "netmask 255.255.255.240")
assert_contains "netmask /28: the gateway is out of reach" "Warning: the gateway is not reachable through this address and netmask" "$out"
assert_eq "...the address is /28" "192.0.2.50/28" "$(addrs)"
assert_eq "...gw is still reported" "Current gw: 192.0.2.11" "$(sw_cmd gw)"
assert_eq "...information.json too" '"ip_gateway":"192.0.2.11"' "$(sw_get information.json | grep -o '"ip_gateway":"[^"]*"')"
assert_eq "...no route" "" "$(defroute)"
assert_eq "...ip shows the address" "Current IP: 192.0.2.50 (static)" "$(sw_cmd ip)"
sw_cmd "netmask 255.255.255.0" >/dev/null
assert_eq "netmask /24 again: the route is back" "default via 192.0.2.11 dev swbr" "$(defroute)"
assert_eq "...gw unchanged" "Current gw: 192.0.2.11" "$(sw_cmd gw)"
sw_cmd "ip 192.0.2.200" >/dev/null	# same subnet: stays
assert_eq "a new address in the subnet keeps the route" "192.0.2.200/24 default via 192.0.2.11 dev swbr" "$(addrs) $(defroute)"
out=$(sw_cmd "ip 198.51.100.7")
assert_contains "an address outside the gateway's subnet warns" "Warning: the gateway is not reachable" "$out"
assert_eq "...gw kept, no route" "Current gw: 192.0.2.11|" "$(sw_cmd gw)|$(defroute)"
sw_cmd "ip 192.0.2.50" >/dev/null
assert_eq "back in the subnet: the route is back" "192.0.2.50/24 default via 192.0.2.11 dev swbr" "$(addrs) $(defroute)"
# one run of commands moves the address to another subnet and types a gateway that does not fit it
out=$(sw_post $'ip 198.18.0.7\nnetmask 255.255.255.0\ngw 198.19.0.1')
assert_contains "address moved, the typed gateway does not fit: a warning" "Warning: the gateway is not reachable" "$out"
[[ $out != *Error* ]] || { echo "an error for a gateway that came with its address: $out"; exit 1; }
assert_eq "...the address moved, the gateway is kept and shown, there is no route" "198.18.0.7/24 Current gw: 198.19.0.1|" "$(addrs) $(sw_cmd gw)|$(defroute)"
sw_cmd "ip 198.19.0.7" >/dev/null
assert_eq "an address change that makes it reachable adds the route" "198.19.0.7/24 default via 198.19.0.1 dev swbr" "$(addrs) $(defroute)"
sw_post $'ip 192.0.2.50\nnetmask 255.255.255.0\ngw 192.0.2.11' >/dev/null
assert_eq "back to the first subnet and gateway" "192.0.2.50/24 default via 192.0.2.11 dev swbr" "$(addrs) $(defroute)"
# swbr's default route is its own: one through another interface is left alone
sw_cmd "gw 192.0.2.12" >/dev/null
assert_eq "gw: swbr's route changes" "default via 192.0.2.12 dev swbr" "$(defroute)"
assert_eq "...dummy0's route is still there" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"
sw_cmd "netmask 255.255.255.128" >/dev/null
assert_eq "netmask: swbr's route is re-made, dummy0's is still there" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"
assert_eq "...swbr's gw route is there" "default via 192.0.2.12 dev swbr" "$(defroute)"
sw_cmd "gw 192.0.2.11" >/dev/null
sw_cmd "netmask 255.255.255.0" >/dev/null
# the web UI sends ip, netmask and gw in one go
swcurl -c "$jar" -d "pwd=$SW_PASS" -o /dev/null "$SW_URL/login"
out=$(swcurl -b "$jar" --data-binary $'ip 192.0.2.60\nnetmask 255.255.255.128\ngw 192.0.2.11' "$SW_URL/cmd")
assert_eq "one /cmd with all three: address, mask and route ($out)" "192.0.2.60/25 default via 192.0.2.11 dev swbr" "$(addrs) $(defroute)"
assert_eq "ip 0.0.0.0 removes the address" "" "$(sw_cmd 'ip 0.0.0.0' >/dev/null; addrs)"
assert_eq "...and dummy0's default route survived every change" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"

# DHCP again, then the daemon ends: SIGTERM stops the client
sw_cmd "ip dhcp" >/dev/null
for _ in $(seq 40); do [[ $(sw_cmd ip) == *"(dhcp"* ]] && break; sleep 0.25; done
assert_contains "DHCP again" "(dhcp, renewal in sec:" "$(sw_cmd ip)"
assert_eq "one client" 1 "$(udhcpc_procs)"
kill -TERM "$daemon"
for _ in $(seq 20); do kill -0 "$daemon" 2>/dev/null || break; sleep 0.1; done
kill -0 "$daemon" 2>/dev/null && { echo "the daemon did not end on SIGTERM"; exit 1; }
wait "$daemon" 2>/dev/null
assert_eq "SIGTERM: the DHCP client is gone with the daemon" 0 "$(udhcpc_procs)"
assert_eq "SIGTERM: swbr stays, and with it the switching" "swbr" "$(rtlp_sw ip -o link show swbr | awk -F'[ :]+' '{print $2}')"
ping_ok 1 192.0.2.12 || { echo "no switching after the daemon ended"; exit 1; }
ifindex=$(rtlp_sw cat /sys/class/net/swbr/ifindex)

# udhcpc.script: a renewal with the router the route already has does not touch the route (no gap);
# another router replaces it; the route of another interface (dummy0) is never touched
run_script() { rtlp_sw env interface=swbr ip=192.0.2.60 mask=24 broadcast=192.0.2.255 router="$1" lease=60 sh "$BENCH_DIR/../udhcpc.script" "$2"; }
rtlp_sw ip -4 addr flush dev swbr
run_script 192.0.2.11 bound || exit 1
assert_eq "script bound: the route" "default via 192.0.2.11 dev swbr" "$(defroute)"
rtlp_sw ip -4 route del default dev swbr && rtlp_sw ip -4 route append default via 192.0.2.11 dev swbr proto 77 || exit 1	# a mark on the route object
run_script 192.0.2.11 renew || exit 1
assert_contains "script renew, same router: the route object is the same (its mark is still on it)" "proto 77" "$(rtlp_sw ip -4 route show default dev swbr)"
run_script 192.0.2.12 renew || exit 1
assert_eq "script renew, other router: the route follows" "default via 192.0.2.12 dev swbr" "$(defroute)"
assert_eq "...and dummy0's route is still there" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"
run_script "" renew || exit 1
assert_eq "script renew, no router: swbr's route is gone" "" "$(defroute)"
assert_eq "...dummy0's is not" "default via 203.0.113.1 dev dummy0" "$(dummy_route)"

# restart: swbr is adopted (same netdev, same address), VLANs a previous run left are removed
rtlp_sw bridge vlan add vid 77 dev lan2 || exit 1
sw_stop
sw_start
assert_eq "swbr was adopted, not recreated" "$ifindex" "$(rtlp_sw cat /sys/class/net/swbr/ifindex)"
assert_eq "the pinned address survived" 00:11:22:33:44:55 "$(rtlp_sw cat /sys/class/net/swbr/address)"
assert_eq "mac shows it" "Current MAC: 00:11:22:33:44:55" "$(sw_cmd mac)"
assert_eq "the stale VLAN 77 is gone, the default state is back" "$(table "$(ports 1 6 1:P:U)" "swbr 1:P:U")" "$(bv)"
assert_eq "a client runs again" 1 "$(udhcpc_procs)"
# SIGKILL: the kernel ends the client (parent-death signal)
daemon=$(cat "$RUN/pid")
kill -KILL "$daemon"; wait "$daemon" 2>/dev/null
for _ in $(seq 20); do [[ $(udhcpc_procs) == 0 ]] && break; sleep 0.1; done
assert_eq "SIGKILL: the DHCP client dies with the daemon" 0 "$(udhcpc_procs)"
