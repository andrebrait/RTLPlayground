#!/bin/bash
# A daemon start on a bridge that is there. (1) The deployed path: a startup configuration with VLAN 1,
# ip, netmask and gw but no pvid lines, on a new and on an adopted bridge, gives every port PVID 1 and
# swbr its address and route at once. (2) An adopted bridge keeps switching while the daemon restarts:
# host 1 floods VLAN 10 (untagged, PVID 10) through 20 restarts and 5 /reset and not one frame reaches host 3
# (VLAN 1); neither do the frames of the two parked ports (host 3: `pvid 3 99`, host 4: `ingress 4t`) reach
# host 5 (VLAN 1). (3) Descriptors: /reset does not leak any, and the DHCP client has no descriptor of the daemon.
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/vlan_lib.sh"
SW_PASS=benchpass
hash=$(openssl passwd -6 "$SW_PASS") || exit 1
floods=()
trap '(( ${#floods[@]} == 0 )) || kill "${floods[@]}" 2>/dev/null' EXIT
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

# ---- (2) the bridge keeps switching through restarts and /reset ----
printf 'passwdhash %s\nvlan 1 1 2 3 4 5 6\nvlan 10 lab 1 2t\npvid 1 10\npvid 3 99\ningress 4t\n' "$hash" > "$SW_CFG/startup-config"
in_host 2 ip link add link h2 name h2.10 type vlan id 10 && in_host 2 ip link set h2.10 up || exit 1
rtlp_sw ip link del swbr || exit 1	# a new bridge for this configuration
sw_start
want=$(table "lan1 1:U 10:P:U" "lan2 1:P:U 10" "lan3 1:U $(pk 3):P:U" "lan4 1:U $(pk 4):P:U" "$(ports 5 6 1:P:U)" "swbr 1:P:U 10")
assert_eq "the configuration is applied (ports 3 and 4 parked)" "$want" "$(bv)"
cat > "$RUN/flood.py" <<'PY'
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
while True:
    s.sendto(b"x", ("192.0.2.255", 9)); time.sleep(0.001)
PY
mac1=$(in_host 1 cat /sys/class/net/h1/address); mac3=$(in_host 3 cat /sys/class/net/h3/address); mac4=$(in_host 4 cat /sys/class/net/h4/address)
listen() { # <host> <source MAC> <out file> <ready file> [seconds]: starts a listener, its pid is $!
	ip netns exec "rtlp-h$1" python3 "$RUN/seen.py" "$4" "${5:-300}" "$2" > "$3" &
}
rm -f "$RUN"/ready_*
listen 2 "$mac1" "$RUN/seen_h1_at_2" "$RUN/ready_a"; la=$!
listen 3 "$mac1" "$RUN/seen_h1_at_3" "$RUN/ready_b"; lb=$!
listen 5 "$mac3" "$RUN/seen_h3_at_5" "$RUN/ready_c"; lc=$!
listen 5 "$mac4" "$RUN/seen_h4_at_5" "$RUN/ready_d"; ld=$!
for _ in $(seq 100); do [[ -e $RUN/ready_a && -e $RUN/ready_b && -e $RUN/ready_c && -e $RUN/ready_d ]] && break; sleep 0.05; done
[[ -e $RUN/ready_a && -e $RUN/ready_b && -e $RUN/ready_c && -e $RUN/ready_d ]] || { echo "listeners not ready"; exit 1; }
floods=()
for h in 1 3 4; do ip netns exec "rtlp-h$h" python3 "$RUN/flood.py" & floods+=($!); done
sleep 0.5
jar=$RUN/cookies
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
for i in $(seq 20); do
	sw_stop
	sw_start
	[[ $i != 1 && $i != 20 ]] || assert_eq "restart $i: the same bridge state" "$want" "$(bv)"
done
for i in 1 2 3 4 5; do
	reset_daemon
	assert_eq "/reset $i: the same bridge state" "$want" "$(bv)"
done
sleep 0.3
kill -TERM "$la" "$lb" "$lc" "$ld"; wait "$la" "$lb" "$lc" "$ld" 2>/dev/null
got_a=$(<"$RUN/seen_h1_at_2"); got_b=$(<"$RUN/seen_h1_at_3"); got_c=$(<"$RUN/seen_h3_at_5"); got_d=$(<"$RUN/seen_h4_at_5")
[[ $got_a =~ ^[0-9]+$ && $got_b =~ ^[0-9]+$ && $got_c =~ ^[0-9]+$ && $got_d =~ ^[0-9]+$ ]] || { echo "listener output: '$got_a' '$got_b' '$got_c' '$got_d'"; exit 1; }
(( got_a > 1000 )) || { echo "host 2 (VLAN 10, tagged) heard only $got_a flood frames"; exit 1; }
assert_eq "frames of the VLAN 10 flood that reached host 3 (VLAN 1) during 20 restarts and 5 /reset ($got_a reached host 2)" 0 "$got_b"
assert_eq "frames of parked port 3 (pvid 3 99) that reached host 5 (VLAN 1)" 0 "$got_c"
assert_eq "frames of parked port 4 (ingress 4t) that reached host 5 (VLAN 1)" 0 "$got_d"
# control: the listeners do hear the hosts once the ports are unparked
sw_cmd "pvid 1 1" >/dev/null; sw_cmd "pvid 3 1" >/dev/null; sw_cmd "ingress 4a" >/dev/null
rm -f "$RUN"/ready_*
listen 3 "$mac1" "$RUN/seen_h1_at_3" "$RUN/ready_b" 1; lb=$!
listen 5 "$mac3" "$RUN/seen_h3_at_5" "$RUN/ready_c" 1; lc=$!
listen 5 "$mac4" "$RUN/seen_h4_at_5" "$RUN/ready_d" 1; ld=$!
for _ in $(seq 100); do [[ -e $RUN/ready_b && -e $RUN/ready_c && -e $RUN/ready_d ]] && break; sleep 0.05; done
wait "$lb" "$lc" "$ld"
(( $(<"$RUN/seen_h1_at_3") > 100 )) || { echo "control: host 3 does not hear host 1 on VLAN 1"; exit 1; }
(( $(<"$RUN/seen_h3_at_5") > 100 )) || { echo "control: host 5 does not hear host 3 once port 3 is in VLAN 1"; exit 1; }
(( $(<"$RUN/seen_h4_at_5") > 100 )) || { echo "control: host 5 does not hear host 4 once port 4 allows untagged"; exit 1; }
for f in "${floods[@]}"; do kill "$f"; done; wait "${floods[@]}" 2>/dev/null; floods=()

# ---- (3) descriptors ----
sw_cmd "pvid 1 10" >/dev/null
pid=$(cat "$RUN/pid")
nfds() { ls "/proc/$pid/fd" | wc -l; }
warm() { rm -f "$jar"; sw_cmd foo >/dev/null; sw_get mtu.json >/dev/null; sw_cmd "vlan show" >/dev/null; sw_cmd "stat" >/dev/null; }
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

# ---- (4) the parking VLANs belong to the daemon: entries a previous run or somebody else left there go ----
printf 'passwdhash %s\nvlan 1 1 2 3 4 5 6\ningress 5t\n' "$hash" > "$SW_CFG/startup-config"
sw_stop
# 4090 is the parking VLAN of port 5: lan1 (also with its PVID), lan2 and the bridge itself are in it
rtlp_sw bridge vlan add vid 4090 dev lan1 pvid untagged && rtlp_sw bridge vlan add vid 4090 dev lan2 untagged \
	&& rtlp_sw bridge vlan add vid 4090 dev swbr self untagged || exit 1
in_host 1 ip link add link h1 name h1.4090 type vlan id 4090 && in_host 1 ip addr add 198.51.100.1/24 dev h1.4090 && in_host 1 ip link set h1.4090 up || exit 1
rtlp_sw timeout 20 stdbuf -oL bridge monitor all > "$RUN/mon.log" 2>&1 &
mon=$!
for _ in $(seq 100); do	# a probe entry tells when the monitor is listening
	rtlp_sw bridge vlan add vid 99 dev lan6 && rtlp_sw bridge vlan del vid 99 dev lan6 || exit 1
	grep -q '^\[VLAN\]lan6 *99' "$RUN/mon.log" && break
	sleep 0.05
done
grep -q '^\[VLAN\]lan6 *99' "$RUN/mon.log" || { echo "the bridge monitor does not report"; exit 1; }
sw_start
kill "$mon" 2>/dev/null; wait "$mon" 2>/dev/null
parked=$(grep -n "^\[VLAN\]lan1 *$(pk 1) PVID" "$RUN/mon.log" | head -1 | cut -d: -f1)
gone=$(grep -n "^\[VLAN\]Deleted lan1 *4090\$" "$RUN/mon.log" | head -1 | cut -d: -f1)
[[ -n $parked && -n $gone ]] || { echo "lan1: no park ($parked) or delete ($gone) event"; exit 1; }
(( parked < gone )) || { echo "lan1 lost the PVID 4090 (event $gone) before it was parked (event $parked): a moment without PVID"; exit 1; }
assert_eq "foreign entries in the parking VLAN of port 5 are gone, port 5 is parked, the rest is as configured" "$(table "$(ports 1 4 1:P:U)" "lan5 1:U $(pk 5):P:U" "lan6 1:P:U" "swbr 1:P:U")" "$(bv)"
assert_eq "parked port 5: its untagged frames reach neither host 1" 0 "$(frames_seen 5 1)"
assert_eq "...nor host 2" 0 "$(frames_seen 5 2)"
assert_eq "...nor the bridge" 0 "$(frames_seen 5 sw)"
assert_eq "frames tagged 4090 from host 1 do not reach parked host 5" 0 "$(frames_seen 1 5 h1.4090 198.51.100.255)"
assert_eq "control: host 1 reaches host 6" 5 "$(frames_seen 1 6)"
