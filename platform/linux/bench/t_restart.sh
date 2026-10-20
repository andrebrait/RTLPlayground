#!/bin/bash
# A daemon start on a bridge that is there. (1) The deployed path: a startup configuration with VLAN 1,
# ip, netmask and gw but no pvid lines, on a new and on an adopted bridge, gives every port PVID 1 and
# swbr its address and route at once. (2) An adopted bridge keeps switching while the daemon restarts:
# host 1 floods VLAN 10 (untagged, PVID 10) through 20 restarts and not one frame reaches host 3 (VLAN 1).
# (3) Descriptors: /reset does not leak any, and the DHCP client has no descriptor of the daemon.
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/vlan_lib.sh"
SW_PASS=benchpass
hash=$(openssl passwd -6 "$SW_PASS") || exit 1
flood=
trap '[[ -z $flood ]] || kill "$flood" 2>/dev/null' EXIT
mkdir -p "$SW_CFG"

# ---- (1) the deployed path ----
printf 'passwdhash %s\nvlan 1 1 2 3 4 5 6\nip 192.0.2.50\nnetmask 255.255.255.0\ngw 192.0.2.11\n' "$hash" > "$SW_CFG/startup-config"
check_deployed() { # <what>
	assert_eq "$1: every port has PVID 1, swbr is in VLAN 1" "$(table "$(ports 1 6 1:P:U)" "swbr 1:P:U")" "$(bv)"
	assert_eq "$1: swbr has the address of the file" "192.0.2.50/24" "$(addrs)"
	assert_eq "$1: and the default route" "default via 192.0.2.11 dev swbr" "$(defroute)"
	assert_eq "$1: ip says so" "Current IP: 192.0.2.50 (static)" "$(sw_cmd ip)"
	assert_eq "$1: vlan show: PVID 1 on every port" 6 "$(sw_cmd 'vlan show' | grep -c $'^[1-6]\t0x0001\tAny')"
}
sw_start
check_deployed "new bridge"
sw_stop
sw_start
check_deployed "adopted bridge"
sw_stop
rtlp_sw ip link del swbr || exit 1
sw_start
check_deployed "bridge deleted, new again"
sw_stop

# ---- (2) the bridge keeps switching through restarts ----
printf 'passwdhash %s\nvlan 1 1 2 3 4 5 6\nvlan 10 lab 1 2t\npvid 1 10\n' "$hash" > "$SW_CFG/startup-config"
in_host 2 ip link add link h2 name h2.10 type vlan id 10 && in_host 2 ip link set h2.10 up || exit 1
rtlp_sw ip link del swbr || exit 1	# a new bridge for this configuration
sw_start
want=$(table "lan1 1:U 10:P:U" "lan2 1:P:U 10" "$(ports 3 6 1:P:U)" "swbr 1:P:U 10")
assert_eq "the configuration is applied" "$want" "$(bv)"
cat > "$RUN/flood.py" <<'PY'
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
while True:
    s.sendto(b"x", ("192.0.2.255", 9)); time.sleep(0.001)
PY
mac1=$(in_host 1 cat /sys/class/net/h1/address)
listen() { # <host> <out file> <ready file> [seconds]: starts a listener for host 1's frames, returns its pid in $!
	ip netns exec "rtlp-h$1" python3 "$RUN/seen.py" "$3" "${4:-120}" "$mac1" > "$2" &
}
rm -f "$RUN/ready2" "$RUN/ready3"
listen 2 "$RUN/seen2" "$RUN/ready2"; l2=$!
listen 3 "$RUN/seen3" "$RUN/ready3"; l3=$!
for _ in $(seq 100); do [[ -e $RUN/ready2 && -e $RUN/ready3 ]] && break; sleep 0.05; done
[[ -e $RUN/ready2 && -e $RUN/ready3 ]] || { echo "listeners not ready"; exit 1; }
ip netns exec rtlp-h1 python3 "$RUN/flood.py" & flood=$!
sleep 0.5
for i in $(seq 20); do
	sw_stop
	sw_start
done
assert_eq "after 20 restarts: the same bridge state" "$want" "$(bv)"
sleep 0.3
kill -TERM "$l2" "$l3"; wait "$l2" "$l3" 2>/dev/null
got2=$(<"$RUN/seen2"); got3=$(<"$RUN/seen3")
[[ $got2 =~ ^[0-9]+$ && $got3 =~ ^[0-9]+$ ]] || { echo "listener output: '$got2' '$got3'"; exit 1; }
(( got2 > 1000 )) || { echo "host 2 (VLAN 10, tagged) heard only $got2 flood frames"; exit 1; }
assert_eq "frames of the VLAN 10 flood that reached host 3 (VLAN 1) during 20 restarts ($got2 reached host 2)" 0 "$got3"
# control: the listener on host 3 does hear host 1 once its port is in VLAN 1
sw_cmd "pvid 1 1" >/dev/null
rm -f "$RUN/ready3"
listen 3 "$RUN/seen3" "$RUN/ready3" 1; l3=$!
for _ in $(seq 100); do [[ -e $RUN/ready3 ]] && break; sleep 0.05; done
wait "$l3"
(( $(<"$RUN/seen3") > 100 )) || { echo "control: host 3 does not hear host 1 on VLAN 1"; exit 1; }
kill "$flood"; wait "$flood" 2>/dev/null; flood=

# ---- (3) descriptors ----
jar=$RUN/cookies
sw_cmd "pvid 1 10" >/dev/null
pid=$(cat "$RUN/pid")
nfds() { ls "/proc/$pid/fd" | wc -l; }
warm() { rm -f "$jar"; sw_cmd foo >/dev/null; sw_get mtu.json >/dev/null; sw_cmd "vlan show" >/dev/null; sw_cmd "stat" >/dev/null; }
reset_daemon() {
	rm -f "$jar"
	swcurl -c "$jar" -d "pwd=$SW_PASS" -o /dev/null "$SW_URL/login"
	assert_contains "/reset answers" "Restarting" "$(swcurl -b "$jar" "$SW_URL/reset")"
	for _ in $(seq 100); do
		[[ $(timeout -s KILL 2 "$SWCLI" -s "$RUN/cli.sock" foo 2>/dev/null) == *"Unknown command"* ]] && return 0
		sleep 0.1
	done
	echo "the daemon did not come back after /reset"; exit 1
}
warm
n0=$(nfds)
for i in 1 2 3; do
	reset_daemon
	assert_eq "same process after /reset $i" "$pid" "$(cat "$RUN/pid")"
	warm
	assert_eq "descriptors of the daemon after /reset $i" "$n0" "$(nfds)"
done
assert_eq "the bridge kept its state through the resets" "$want" "$(bv)"
# the DHCP client has no descriptor of the daemon but its stdio
sw_cmd "ip dhcp" >/dev/null
cpid=$(for p in /proc/[0-9]*; do [[ $(sed 's/^.*) //' "$p/stat" 2>/dev/null | cut -d' ' -f2) == "$pid" ]] && echo "${p#/proc/}"; done)
[[ $cpid =~ ^[0-9]+$ ]] || { echo "no DHCP client child: '$cpid'"; exit 1; }
assert_eq "udhcpc: stdin" /dev/null "$(readlink "/proc/$cpid/fd/0")"
assert_eq "udhcpc: stdout" /dev/null "$(readlink "/proc/$cpid/fd/1")"
dsocks=$(for f in /proc/$pid/fd/*; do readlink "$f"; done | grep '^socket:' | sort)
mine=$(for f in /proc/$cpid/fd/*; do n=${f##*/}; (( n > 2 )) && readlink "$f"; done | sort)
assert_eq "udhcpc: no /dev/null left open" "" "$(grep -x /dev/null <<<"$mine")"
assert_eq "udhcpc: no socket of the daemon" "" "$(comm -12 <(echo "$dsocks") <(echo "$mine"))"
[[ -n $dsocks ]] || { echo "the daemon has no socket?"; exit 1; }
