#!/bin/bash
# Ports: link state and codes, counters, MTU, speed/duplex, chip temperature.
# veth reports 10000 Mb/s full duplex through ethtool (link code 5), has no advertised modes, no
# pause parameters and refuses every setting change.
. "$(dirname "$0")/lib.sh"
sw_start
sw_cmd "passwd $SW_PASS" >/dev/null

# status.json field of port N as JSON text: status_field <port> <key>
status_field() {
	sw_get status.json | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)[int(sys.argv[1])-1][sys.argv[2]]))' "$1" "$2" || exit 1
}
# word W of counters.json?port=N (the MIB list of app.js) as a decimal: ctr <port> <word>
ctr() {
	sw_get "counters.json?port=$1" | python3 -c 'import json,sys; print(int(json.load(sys.stdin)[int(sys.argv[1])],16))' "$2" || exit 1
}
mtu_of() { ip -n rtlp-sw -o link show "$1" | sed -n 's/.* mtu \([0-9]*\) .*/\1/p'; }
rtlp_sw() { ip netns exec rtlp-sw "$@"; }

# ---- link codes: veth is 10G, a port whose peer is down is down ----
for n in 1 2 3 4 5 6; do
	assert_eq "lan$n link code (10G)" 5 "$(status_field $n link)"
	assert_eq "lan$n enabled" 1 "$(status_field $n enabled)"
done
in_host 3 ip link set h3 down
assert_eq "lan3 with its peer down" 0 "$(status_field 3 link)"
assert_eq "lan4 unaffected" 5 "$(status_field 4 link)"
assert_contains "port 3 show: down" "Link speed: Down" "$(sw_cmd 'port 3 show')"
in_host 3 ip link set h3 up
assert_eq "lan3 back up" 5 "$(status_field 3 link)"
out=$(sw_cmd "port 1 show")
assert_contains "port 1 show: speed and duplex" "Link speed: 10G full duplex" "$out"
assert_contains "port 1 show: pause (veth has none to report)" "Pause negotiated: unknown" "$out"
stat=$(sw_cmd stat | tr -d '\r')
assert_eq "stat: six ports, on, 10G" 6 "$(grep -cP '^[1-6]\tOn\t10G\t' <<<"$stat")"

# ---- off / on: IFF_UP ----
assert_contains "port 3 off" "Setting port 3 to disabled" "$(sw_cmd 'port 3 off')"
assert_contains "lan3 is down" " DOWN " "$(ip -n rtlp-sw -br link show lan3)"
assert_eq "lan3 not enabled" 0 "$(status_field 3 enabled)"
assert_eq "lan3 has no link" 0 "$(status_field 3 link)"
assert_contains "stat shows port 3 Off and Down" $'3\tOff\tDown\t' "$(sw_cmd stat | tr -d '\r')"
assert_contains "port 3 on" "Port 3 is on; it has no selectable link modes" "$(sw_cmd 'port 3 on')"
assert_contains "lan3 is up again" " UP " "$(ip -n rtlp-sw -br link show lan3)"
assert_eq "lan3 enabled again" 1 "$(status_field 3 enabled)"
assert_eq "lan3 linked again" 5 "$(status_field 3 link)"

# ---- veth refuses speed and duplex changes: an error, and nothing changes ----
for c in "port 1 1g" "port 1 100m full" "port 1 duplex half" "port 1 2g5"; do
	out=$(sw_cmd "$c")
	assert_contains "'$c' fails" "Error: " "$out"
	[[ $out != *"Setting port"* ]] || { echo "'$c' says it applied: $out"; exit 1; }
done
assert_contains "lan1 still up" " UP " "$(ip -n rtlp-sw -br link show lan1)"
assert_contains "lan1 speed unchanged" "Speed: 10000Mb/s" "$(rtlp_sw ethtool lan1)"
assert_contains "port 1 show unchanged" "Link speed: 10G full duplex" "$(sw_cmd 'port 1 show')"
assert_eq "lan1 link code unchanged" 5 "$(status_field 1 link)"
# nothing to advertise on veth: auto only makes sure the port is on
out=$(sw_cmd "port 1 auto")
assert_contains "port 1 auto says there is nothing to select" "Port 1 is on; it has no selectable link modes" "$out"
[[ $out != *"Setting port"* ]] || { echo "auto on veth says it applied: $out"; exit 1; }

# ---- counters: the netdev statistics (veth has no driver counters and no broadcast/unicast split) ----
bcast() { in_host 1 python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
for _ in range(6): s.sendto(b"x" * 100, ("192.0.2.255", 9))'; }
tx_bcast() { rtlp_sw python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.setsockopt(socket.SOL_SOCKET, 25, b"lan1")	# SO_BINDTODEVICE: lan1 has no address
for _ in range(5): s.sendto(b"x" * 100, ("255.255.255.255", 9))'; }
rose() { # <what> <before> <after> <at least>
	(( $3 - $2 >= $4 )) || { echo "$1 rose by $(($3 - $2)), expected at least $4"; exit 1; }
}

rx0=$(ctr 1 47); oct0=$(ctr 1 0)
bcast || exit 1
rx1=$(ctr 1 47); oct1=$(ctr 1 0)
rose "RX Good Counter" "$rx0" "$rx1" 6
rose "in Octets" "$oct0" "$oct1" 600
tx0=$(ctr 1 46); toct0=$(ctr 1 1)
tx_bcast || exit 1
tx1=$(ctr 1 46); toct1=$(ctr 1 1)
rose "TX Good Counter" "$tx0" "$tx1" 5
rose "out Octets" "$toct0" "$toct1" 500
(( $(ctr 1 3) <= rx1 )) || { echo "in Multicast above RX Good"; exit 1; }
assert_eq "in Broadcast has no source on veth" 0 "$(ctr 1 4)"
assert_eq "in Unicast has no source" 0 "$(ctr 1 2)"
assert_eq "TX Ether Broadcast/Multicast (word 14) has no source on veth" 0 "$(ctr 1 14)"
assert_eq "word 8 (out discards | in discards) is two 32-bit halves of the netdev drops" 0 "$(ctr 1 8)"
# the status page reads the same words
(( $(printf '%d' "$(status_field 1 rxG | tr -d '"')") >= rx1 )) || { echo "status.json rxG below the counter"; exit 1; }
assert_eq "words past the MIB list are 0" 0 "$(ctr 1 52)"
assert_eq "an unmapped word (size histogram) is 0" 0 "$(ctr 1 21)"

# ---- MTU: V = frame length = netdev MTU + 26 (the Otto driver's RTLDSA_FRAME_OVERHEAD) ----
assert_contains "default mtu.json (1500 + 26)" '"portNum":2,"mtu":"0x05f6"' "$(sw_get mtu.json)"
sw_cmd "mtu 2 9000" >/dev/null
assert_eq "mtu 2 9000: lan2 netdev MTU" 8974 "$(mtu_of lan2)"
assert_eq "mtu 2 9000: lan1 untouched" 1500 "$(mtu_of lan1)"
assert_contains "mtu show" "Port 2 0x2328" "$(sw_cmd 'mtu show')"
assert_contains "mtu.json" '"portNum":2,"mtu":"0x2328"' "$(sw_get mtu.json)"
out=$(sw_cmd "mtu 2 93")	# netdev MTU 67, below the kernel minimum of 68
assert_contains "mtu 2 93 is refused (netdev MTU 67 < 68)" "Error: lan2 takes frame lengths of 94..16383" "$out"
assert_eq "refused mtu changes nothing" 8974 "$(mtu_of lan2)"
assert_contains "mtu show still 9000" "Port 2 0x2328" "$(sw_cmd 'mtu show')"
sw_cmd "mtu 2 94" >/dev/null
assert_eq "mtu 2 94: smallest netdev MTU" 68 "$(mtu_of lan2)"
sw_cmd "mtu 2 1526" >/dev/null
assert_eq "mtu 2 1526 is the default 1500" 1500 "$(mtu_of lan2)"
# a netdev MTU beyond the 14 bits of the register reads as 16383, not wrapped
ip -n rtlp-sw link set lan5 mtu 65535 || exit 1
assert_contains "mtu show clamps 65535 + 26" "Port 5 0x3fff" "$(sw_cmd 'mtu show')"
assert_contains "mtu.json clamps" '"portNum":5,"mtu":"0x3fff"' "$(sw_get mtu.json)"
sw_cmd "mtu 5 1526" >/dev/null
# swbr, the bridge the daemon made over the ports, follows the smallest port MTU: the kernel does it, nobody sets the bridge's
for n in 1 2 3 4 5 6; do sw_cmd "mtu $n 9000" >/dev/null; done
assert_eq "bridge follows the ports to 8974" 8974 "$(mtu_of swbr)"
sw_cmd "mtu 3 1526" >/dev/null
assert_eq "one port back to 1526 pulls the bridge down" 1500 "$(mtu_of swbr)"
assert_eq "...and leaves the other ports" 8974 "$(mtu_of lan4)"
assert_contains "mtu show of port 3" "Port 3 0x05f6" "$(sw_cmd 'mtu show')"

# ---- chip temperature: no thermal zone here, 0 with one log line ----
out=$(sw_cmd temp)
assert_contains "temp now" "Chip temperature: 0.0 C" "$out"
assert_contains "temp at power-on" "At power-on:      0.0 C" "$out"
sw_cmd temp >/dev/null
assert_eq "no thermal zone is logged once" 1 "$(grep -c 'no thermal zone' "$RUN/daemon.log")"
