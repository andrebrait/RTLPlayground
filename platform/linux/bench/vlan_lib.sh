# Sourced by t_vlan.sh and t_restart.sh after lib.sh: helpers for the bridge, VLAN and management tests.
rtlp_sw() { ip netns exec rtlp-sw "$@"; }
ping_ok() { in_host "$1" busybox ping -c 2 -W 2 "$2" >/dev/null 2>&1; }	# host N reaches IP
swcurl() { rtlp_sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 "$@"; }

# bv: the bridge's VLAN table, one line per port: "<dev> <vid>[:P][:U] ..." (P = PVID, U = egress untagged)
bv() {
	rtlp_sw bridge -j vlan show | python3 -c '
import json, sys
short = {"PVID": "P", "Egress Untagged": "U"}
for d in json.load(sys.stdin):
    print(d["ifname"], " ".join("%d%s" % (v["vlan"], "".join(":" + short[f] for f in v.get("flags", []))) for v in d["vlans"]))
' || exit 1
}
# table <line>...: the expected bv output; ports <first> <last> <entries>: one line for each lanN
table() { printf '%s\n' "$@"; }
ports() { local n; for n in $(seq "$1" "$2"); do printf 'lan%s %s\n' "$n" "$3"; done; }
addrs() { rtlp_sw ip -4 -o addr show dev swbr | awk '{printf "%s%s", s, $4; s=" "}'; }
# routes_of <dev>: its default routes, "default via <gw> dev <dev>"
routes_of() { rtlp_sw ip -4 route show default | awk -v d="$1" '$4 == "dev" && $5 == d {print $1, $2, $3, $4, $5}'; }
defroute() { routes_of swbr; }
dummy_route() { routes_of dummy0; }

# seen.py <ready file> <seconds> <source MAC> [dev]: counts the frames of that source MAC (payload "x")
# seen for that long, on dev (default: every device of the namespace); the ready file appears once the
# socket is open, so nothing sent after that is missed; SIGTERM ends it early (it still prints)
cat > "$RUN/seen.py" <<'PY'
import signal, socket, sys, time
mac = bytes.fromhex(sys.argv[3].replace(":", ""))
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3)); s.settimeout(0.05)
if len(sys.argv) > 4: s.bind((sys.argv[4], 0))
open(sys.argv[1], "w").close()
end = time.time() + float(sys.argv[2]); n = 0
def stop(*a): raise SystemExit
signal.signal(signal.SIGTERM, stop)
try:
    while time.time() < end:
        try: f = s.recv(2048)
        except socket.timeout: continue
        if f[6:12] == mac and f.endswith(b"x"): n += 1
finally:
    print(n)
PY
cat > "$RUN/send.py" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
if sys.argv[1]: s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, sys.argv[1].encode())
for _ in range(5): s.sendto(b"x", ("192.0.2.255", 9))
PY
# wait_ready: until the listener just started has opened its socket (its ready file)
wait_ready() { local i; for i in $(seq 100); do [[ -e $RUN/ready ]] && return 0; sleep 0.05; done; echo "listener not ready"; exit 1; }
# pk <port>: the parking VLAN of logical port <port> (1-based, bench board): 4094 - (port - 1)
pk() { echo $((4095 - $1)); }
# frames_seen <from> <to|sw> [dev]: host <from> sends 5 broadcast frames (out of its device <dev>, default
# its own); prints how many of them host <to> (or the bridge itself) received, tagged or not (the
# frames are told apart by their source MAC)
frames_seen() {
	local mac p; mac=$(in_host "$1" cat "/sys/class/net/h$1/address")
	rm -f "$RUN/ready"
	if [[ $2 == sw ]]; then
		rtlp_sw python3 "$RUN/seen.py" "$RUN/ready" 1 "$mac" swbr > "$RUN/seen" &
	else
		in_host "$2" python3 "$RUN/seen.py" "$RUN/ready" 1 "$mac" > "$RUN/seen" &
	fi
	p=$!
	wait_ready
	in_host "$1" python3 "$RUN/send.py" "${3:-}" || exit 1
	wait $p
	cat "$RUN/seen"
}
vlan_json() { sw_get "vlan.json?vid=$1"; }

