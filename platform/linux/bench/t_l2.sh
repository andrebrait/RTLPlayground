#!/bin/bash
# The L2 table: /l2.json, /l2_del.json, `l2`, `l2 forget` and the MACs column are the bridge's
# forwarding database (fdb.c). Ground truth is `bridge -j fdb show`; the walk of the page is a
# mirror of l2Load() in html/app.js (pages of idx = last idx + 1, stop at the first idx seen twice).
. "$(dirname "$0")/lib.sh"
. "$BENCH_DIR/vlan_lib.sh"
sw_start
sw_cmd "passwd $SW_PASS" >/dev/null

cat > "$RUN/l2tool.py" <<'PY'
import json, sys, urllib.request

def opener():
    # the session of sw_get (a login replaces the one before it): its cookie is in the jar curl wrote
    jar = [l.split("\t") for l in open(sys.argv[1]) if "\tsession\t" in l]
    op = urllib.request.build_opener()
    op.addheaders = [("Cookie", "session=" + jar[0][6].strip())]
    return op

def walk(op):
    """l2Load() of html/app.js: [(mac, vlan, type, port, idx)], pages fetched"""
    seen, rows, idx, guard = {}, [], 0, 0
    while True:
        s = json.load(op.open("http://127.0.0.1/l2.json?idx=%d" % idx, timeout=5))
        if not s:
            break
        wrapped = False
        for e in s:
            i = int(e["idx"], 16)
            if i in seen:
                wrapped = True
                continue
            seen[i] = 1
            rows.append((e["mac"], int(e["vlan"], 16), e["type"], e["port"], i))
        guard += 1
        if wrapped or guard > 140:
            break
        idx = int(s[-1]["idx"], 16) + 1
    return rows, guard

cmd = sys.argv[2]
if cmd == "rows":      # one line per entry: mac vlan type port
    print("\n".join(sorted("%s %d %s %d" % r[:4] for r in walk(opener())[0])))
elif cmd == "idx":     # idx of <mac> <vlan> (decimal), "none" if not listed
    print(next((r[4] for r in walk(opener())[0] if r[0] == sys.argv[3] and r[1] == int(sys.argv[4])), "none"))
elif cmd == "count":   # entries and pages of one walk
    rows, pages = walk(opener())
    print(len(rows), pages, len({r[:2] for r in rows}))
elif cmd == "kernel":  # the same lines from `bridge -j fdb show` on stdin, argv[3] = the switch's own MAC
    own, out = sys.argv[3], []
    for e in json.load(sys.stdin):
        if e.get("master") != "swbr" or "vlan" not in e or e["vlan"] >= 4089:
            continue
        if e["state"] == "permanent":    # the switch's own entry sits on swbr or, with a port's address, on that port
            if e["mac"] == own and e["vlan"] == 1:
                out.append("%s 1 s 9" % own)
        elif e["ifname"].startswith("lan"):
            out.append("%s %d %s %d" % (e["mac"], e["vlan"], "s" if e["state"] == "static" else "l", int(e["ifname"][3:]) - 1))
    print("\n".join(sorted(out)))
elif cmd == "churn":   # N walks while entries come and go: argv[3] = N, argv[4] = file of MACs that stay
    op, stay = opener(), set(open(sys.argv[4]).read().split())
    most = churned = 0
    for n in range(int(sys.argv[3])):
        rows, pages = walk(op)
        keys = [r[:2] for r in rows]
        assert pages <= 140, "walk %d did not end" % n
        assert len(keys) == len(set(keys)), "walk %d lists an entry twice: %s" % (n, sorted(k for k in keys if keys.count(k) > 1))
        missing = stay - {r[0] for r in rows}
        assert not missing, "walk %d lost %s" % (n, sorted(missing)[:3])
        most = max(most, pages)
        churned += any(r[0].startswith("02:00:00:00:c") for r in rows)
    print("ok %d %d" % (most, churned))
PY
sw_get l2.json >/dev/null || exit 1	# logs in
l2() { rtlp_sw python3 "$RUN/l2tool.py" "$RUN/cookies" "$@" || exit 1; }
own=$(rtlp_sw cat /sys/class/net/swbr/address)
kernel() { rtlp_sw bridge -j fdb show br swbr | python3 "$RUN/l2tool.py" "$RUN/cookies" kernel "${1:-$own}" || exit 1; }
hmac() { in_host "$1" cat "/sys/class/net/h$1/address"; }
l2del() { sw_get "l2_del.json?idx=$1"; }
status_macs() { sw_get status.json | python3 -c 'import json,sys; print(" ".join(str(int(p["macs"], 16)) for p in json.load(sys.stdin)))' || exit 1; }
kcount() { # kcount <dev> <state>: entries of the port in the kernel's table (VLAN-less ones are not entries of the table)
	rtlp_sw bridge -j fdb show dev "$1" | python3 -c '
import json, sys
print(sum(1 for e in json.load(sys.stdin) if e.get("master") == "swbr" and "vlan" in e and e["vlan"] < 4089 and e["state"] == sys.argv[1]))' "$2" || exit 1
}
# no IPv6 chatter from the hosts (router solicitations for 12 s after they come up) to move the table under the comparisons
for n in 1 2 3 4 5 6; do in_host $n sh -c "echo 1 > /proc/sys/net/ipv6/conf/all/disable_ipv6" || exit 1; done
h1=$(hmac 1); h2=$(hmac 2); h3=$(hmac 3)

# ---- traffic from three hosts: their MACs on the right ports and VLAN, nothing else listed ----
in_host 1 busybox ping -c 1 -W 2 192.0.2.12 >/dev/null 2>&1
in_host 3 busybox ping -c 1 -W 2 192.0.2.11 >/dev/null 2>&1
in_host 2 busybox ping -c 1 -W 2 192.0.2.13 >/dev/null 2>&1
rows=$(l2 rows)
assert_contains "host 1 on port 1 (logical 0), learned, VLAN 1" "$h1 1 l 0" "$rows"
assert_contains "host 2 on port 2" "$h2 1 l 1" "$rows"
assert_contains "host 3 on port 3" "$h3 1 l 2" "$rows"
assert_contains "the switch's own MAC: static, CPU port, management VLAN" "$own 1 s 9" "$rows"
assert_eq "the page lists what the kernel table holds, no local entry of a port" "$(kernel)" "$rows"
raw=$(sw_get l2.json)
assert_contains "the JSON of an entry" "{\"vlan\":\"001\",\"mac\":\"$h1\",\"type\":\"l\",\"port\":0,\"lag\":0,\"idx\":\"" "$raw"
cli=$(sw_cmd l2)
assert_contains "l2: header" $'\tMAC\t\tVLAN\ttype\tport' "$cli"
assert_contains "l2: a learned entry" "$h1"$'\t0x0001\tlearned\t1' "$cli"
assert_contains "l2: the own entry" "$own"$'\t0x0001\tstatic\tCPU' "$cli"

# ---- static, dynamic, a parking VLAN ----
m0=$(status_macs)
rtlp_sw bridge fdb add 02:00:00:00:00:a1 dev lan4 master static vlan 1 || exit 1
assert_eq "macs: a static entry is not a learned one" "$m0" "$(status_macs)"
rtlp_sw bridge fdb add 02:00:00:00:00:a2 dev lan5 master dynamic vlan 1 || exit 1
sw_cmd "ingress 4t" >/dev/null	# parks port 4: it is a member of its parking VLAN now
rtlp_sw bridge fdb add 02:00:00:00:00:a3 dev lan4 master static vlan "$(pk 4)" || exit 1
rows=$(l2 rows)
assert_contains "a static entry: type s, port 4" "02:00:00:00:00:a1 1 s 3" "$rows"
assert_contains "a dynamic entry: type l, port 5" "02:00:00:00:00:a2 1 l 4" "$rows"
[[ $rows != *02:00:00:00:00:a3* ]] || { echo "an entry of a parking VLAN is listed"; exit 1; }
assert_eq "...the table still equals the kernel's, which has the parking VLAN entry" "$(kernel)" "$rows"
assert_contains "...it is there" "02:00:00:00:00:a3" "$(rtlp_sw bridge fdb show br swbr)"
assert_eq "macs: the learned entries of each port" "$(for n in 1 2 3 4 5 6; do printf '%s ' "$(kcount lan$n '')"; done | sed 's/ $//')" "$(status_macs)"
read -r -a m <<<"$m0"
assert_eq "macs: a dynamic entry on port 5 counts" "${m[0]} ${m[1]} ${m[2]} ${m[3]} $((m[4] + 1)) ${m[5]}" "$(status_macs)"
assert_eq "stat: the MACs column of port 5" "$((m[4] + 1))" "$(sw_cmd stat | tr -d '\r' | awk -F'\t' '$1 == 5 {print $NF + 0}')"

# ---- delete by index ----
i2=$(l2 idx "$h2" 1)
[[ $i2 != none ]] || { echo "host 2 has no index"; exit 1; }
before=$(l2 rows)
assert_eq "l2_del.json deletes the entry of that slot" '{"result":1}' "$(l2del "$i2")"
assert_eq "...it is gone from the kernel's table" 0 "$(rtlp_sw bridge fdb show br swbr | grep -c "^$h2 dev lan2 vlan 1 master swbr")"
assert_eq "...and only it" "$(grep -v "^$h2 " <<<"$before")" "$(l2 rows)"
assert_eq "the slot is empty now: nothing deleted" '{"result":0}' "$(l2del "$i2")"
assert_eq "...and no neighbour went with it" "$(grep -v "^$h2 " <<<"$before")" "$(l2 rows)"
assert_eq "a slot beyond the table" '{"result":0}' "$(l2del 4095)"
assert_eq "an index beyond the table" '{"result":0}' "$(l2del 4096)"
iown=$(l2 idx "$own" 1)
assert_eq "the switch's own MAC cannot be deleted" '{"result":0}' "$(l2del "$iown")"
assert_contains "...it is still in the kernel's table" "$own dev" "$(rtlp_sw bridge fdb show br swbr | grep permanent)"
assert_contains "...and listed" "$own 1 s 9" "$(l2 rows)"
isa=$(l2 idx 02:00:00:00:00:a1 1)
assert_eq "a static entry can be deleted" '{"result":1}' "$(l2del "$isa")"
assert_eq "...gone from the kernel" 0 "$(rtlp_sw bridge fdb show br swbr | grep -c '^02:00:00:00:00:a1 ')"

# ---- l2 forget: the learned entries go, the static ones stay ----
rtlp_sw bridge fdb add 02:00:00:00:00:a4 dev lan6 master static vlan 1 || exit 1
assert_eq "l2 forget prints nothing" "" "$(sw_cmd 'l2 forget')"
rows=$(l2 rows)
assert_eq "only static entries are left" "" "$(grep -v ' s [0-9]*$' <<<"$rows")"
assert_contains "...the static one" "02:00:00:00:00:a4 1 s 5" "$rows"
assert_contains "...and the own one" "$own 1 s 9" "$rows"
assert_eq "...the table equals the kernel's" "$(kernel)" "$rows"
assert_eq "macs are 0 again" "0 0 0 0 0 0" "$(status_macs)"
assert_contains "the parking VLAN's entry was not part of the table" "02:00:00:00:00:a3" "$(rtlp_sw bridge fdb show br swbr)"
sw_cmd "ingress 4a" >/dev/null
in_host 1 busybox ping -c 1 -W 2 192.0.2.12 >/dev/null 2>&1
assert_contains "learning goes on after a forget" "$h1 1 l 0" "$(l2 rows)"

# ---- the management MAC and VLAN: the own entry follows them ----
assert_contains "mac" "MAC set to: 00:11:22:33:44:55" "$(sw_cmd 'mac 00:11:22:33:44:55')"
rows=$(l2 rows)
assert_contains "the own entry has the new address" "00:11:22:33:44:55 1 s 9" "$rows"
[[ $rows != *"$own 1 s 9"* ]] || { echo "the old own entry is still listed"; exit 1; }
assert_eq "...and the table equals the kernel's" "$(kernel 00:11:22:33:44:55)" "$rows"
assert_eq "the own entry still cannot be deleted" '{"result":0}' "$(l2del "$(l2 idx 00:11:22:33:44:55 1)")"
sw_cmd "vlan 20 6" >/dev/null
assert_eq "vlan 20 mgmt" "Management VLAN set to 0x0014" "$(sw_cmd 'vlan 20 mgmt')"
assert_contains "the own entry is in VLAN 20 now" "00:11:22:33:44:55 20 s 9" "$(l2 rows)"
out=$(sw_cmd "vlan 4090 mgmt")
assert_contains "a parking VLAN cannot manage the switch" "Error: VLAN 4090 is reserved for a parked port" "$out"
assert_eq "...the bridge keeps its untagged entry of VLAN 20" "swbr 1 20:P:U" "$(bv | grep '^swbr ')"
assert_contains "...the shared command reports the VLAN it is left with" "Management VLAN set to 0x0014" "$out"
assert_contains "...the management VLAN is still 20" '{"mgmt":20,' "$(sw_get vlanlist)"
assert_contains "...and so is the own entry" "00:11:22:33:44:55 20 s 9" "$(l2 rows)"
sw_cmd "vlan 1 mgmt" >/dev/null
own=00:11:22:33:44:55	# `mac` refuses the locally administered default the bridge had, so it stays

# ---- paging while entries come and go (app.js mirror; Review Focus 5) ----
for i in $(seq 1 100); do printf '02:00:00:00:%02x:%02x\n' $((i % 7)) "$i"; done > "$RUN/stay"
sed 's/^/fdb replace /; s/$/ dev lan2 master static vlan 1/' "$RUN/stay" > "$RUN/stay.batch"
for i in $(seq 1 50); do printf 'fdb %s 02:00:00:00:c%x:%02x dev lan1 master static vlan 1\n' ADD $((i % 16)) "$i"; done > "$RUN/churn.tpl"
sed 's/ADD/replace/' "$RUN/churn.tpl" > "$RUN/churn.add"
sed 's/ADD/del/; s/ static//' "$RUN/churn.tpl" > "$RUN/churn.del"
rtlp_sw bridge -batch "$RUN/stay.batch" || exit 1
assert_eq "100 stable entries: a walk takes more than two pages and lists each once" "1" "$(l2 count | awk '$1 >= 100 && $2 >= 4 && $1 == $3 {print 1}')"
rm -f "$RUN/churn.stop"
( while [[ ! -e $RUN/churn.stop ]]; do
	rtlp_sw bridge -force -batch "$RUN/churn.add"
	rtlp_sw bridge -force -batch "$RUN/churn.del"
done ) >"$RUN/churn.log" 2>&1 &
churn_pid=$!
res=$(l2 churn 60 "$RUN/stay" 2>&1)
touch "$RUN/churn.stop"; wait "$churn_pid"
[[ -s $RUN/churn.log ]] && { echo "the churn loop failed: $(cat "$RUN/churn.log")"; exit 1; }
read -r _ pages churned <<<"$res"
[[ $res == ok* && $pages -ge 4 && $churned -ge 1 ]] || { echo "churn walks: '$res'"; exit 1; }
rtlp_sw bridge -force -batch "$RUN/churn.del" 2>/dev/null	# what the loop had added when it stopped
echo "churn: $res"
assert_eq "after the churn the table equals the kernel's" "$(kernel)" "$(l2 rows)"

# ---- more entries than the 4096 slots of the table ----
python3 - > "$RUN/many.batch" <<'PY'
for i in range(4200):
    print("fdb replace 02:00:01:%02x:%02x:%02x dev lan3 master static vlan 1" % (i >> 16, i >> 8 & 255, i & 255))
PY
rtlp_sw bridge -batch "$RUN/many.batch" || exit 1
# pages are cut at the size of the output buffer (29 entries), so app.js's guard of 140 pages ends a walk of a full table at ~4060 entries: the 8051 has the same limit
assert_eq "4200 static entries: the walk ends, no entry twice, none beyond the 4096 slots" 1 "$(l2 count | awk '$1 >= 4000 && $1 <= 4096 && $1 == $3 && $2 <= 141 {print 1}')"
assert_contains "...and the daemon said so once" "the bridge has more than 4096 L2 entries" "$(cat "$RUN/daemon.log")"
assert_eq "...once" 1 "$(grep -c 'more than 4096' "$RUN/daemon.log")"
sed 's/replace/del/; s/ static//' "$RUN/many.batch" > "$RUN/many.del"
rtlp_sw bridge -batch "$RUN/many.del" || exit 1
assert_eq "the table is the kernel's again" "$(kernel)" "$(l2 rows)"

# ---- an empty table: the walk of the page ends at once ----
sed 's/replace/del/; s/ static//' "$RUN/stay.batch" > "$RUN/stay.del"
rtlp_sw bridge -force -batch "$RUN/stay.del" 2>/dev/null
rtlp_sw bridge fdb del 02:00:00:00:00:a4 dev lan6 master vlan 1 || exit 1
sw_cmd "l2 forget" >/dev/null
sw_cmd "vlan 99 mgmt" >/dev/null	# VLAN 99 does not exist: the bridge has no entry of its own in it
assert_eq "an empty table: the page is []" "[]" "$(sw_get 'l2.json?idx=0')"
assert_eq "...from any index" "[]" "$(sw_get 'l2.json?idx=2000')"
assert_eq "...nothing to delete" '{"result":0}' "$(l2del 0)"
assert_eq "...the walk is empty" "0" "$(l2 count | cut -d' ' -f1)"
assert_eq "...l2 shows the header only" $'\n\tMAC\t\tVLAN\ttype\tport' "$(sw_cmd l2)"

# ---- no kernel call failed quietly ----
assert_eq "no dump failed" 0 "$(grep -c 'cannot read the L2 table' "$RUN/daemon.log")"
echo ok
