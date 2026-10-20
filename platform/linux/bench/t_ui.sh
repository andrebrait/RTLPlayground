#!/bin/bash
# Every route the web UI (html/app.js) requests: derived from the source, so a route app.js gains
# must be added to the table below or this test fails. Each GET route answers 200 and, when it is
# JSON, valid JSON. POST routes are exercised here too (/cmd) or by t_http.sh (/upload, /reset).
. "$(dirname "$0")/lib.sh"
APP_JS=${RTLP_APP_JS:-$BENCH_DIR/../../../html/app.js}
sw_start
sw_cmd "passwd $SW_PASS" >/dev/null

# route as app.js spells it (query up to the "=") | what to do | value for the query
ROUTES='/status.json|json|
/information.json|json|
/mtu.json|json|
/stp.json|json|
/counters.json?port=|json|1
/vlanlist|json|
/vlan.json?vid=|json|1
/l2.json?idx=|json|0
/l2_del.json?idx=|json|4000
/mirror.json|json|
/isolation.json|json|
/lacp.json|json|
/lag.json|json|
/lldp.json|json|
/eee.json|json|
/bandwidth.json|json|
/storm.json|json|
/config|text|
/cmd_log|text|
/cmd_log_clear|text|
/cmd|post|
/upload|skip|a firmware upload would flash; t_http.sh checks the answer
/reset|skip|restarts the daemon; t_http.sh checks it
/login.html|skip|a navigation target (fetched below)'

# Every quote style, and the path may be glued to more text ("/vlan.json?vid="+id): the route is what
# follows the quote up to the query's "=".
used=$(grep -oE "[\"'\`]/[A-Za-z0-9_.-]+(\?[A-Za-z]+=)?" "$APP_JS" | tr -d "\"'\`" | sort -u)
[[ -n $used ]] || { echo "no route found in $APP_JS"; exit 1; }
known=$(printf '%s\n' "$ROUTES" | cut -d'|' -f1 | sort -u)
assert_eq "the routes app.js requests are the routes this test knows" "$known" "$used"
# A route built at run time would not show above. The request wrappers (api, fetch, getJSON, getText)
# take a literal path starting with "/" and a name everywhere except in their own definitions (six
# calls, which are known): a relative path ("new.json"), a bare "/" glued to more text ("/"+n+".json")
# or a variable all count.
nonlit=$(grep -oE "\b(getJSON|getText|api|fetch)\(([^\"'\`]|[\"'\`]([^/]|/[^A-Za-z0-9_.-]))" "$APP_JS" | wc -l)
assert_eq "calls of the request functions with a path that is not a literal route (only their definitions)" 6 "$nonlit"
# STP is off (2b) and shows the defaults of a booting 8051 (rtl837x_stp.c stp_defaults)
assert_eq "stp.json: off, with the 802.1D/w defaults" "0 1 8 2 20 15 6" "$(sw_get stp.json | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(*[d[k] for k in ("on", "rstp", "prio", "hello", "maxage", "fwd", "txhold")])')"
# the shape of the pages the UI builds its tables from
assert_eq "status.json has one entry per port with link and macs" "6 6 6" "$(sw_get status.json | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(len(d), sum("link" in p for p in d), sum("macs" in p for p in d))')"
assert_eq "stp.json lists every port" "1 2 3 4 5 6" "$(sw_get stp.json | python3 -c '
import json, sys
print(*[p["p"] for p in json.load(sys.stdin)["ports"]])')"

while IFS='|' read -r route kind val; do
	case $kind in
	skip) continue ;;
	post)
		out=$(ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" -b "$RUN/cookies" -d 'mtu show' -w '\n%{http_code}' "$SW_URL$route") || exit 1
		assert_eq "POST $route" 200 "${out##*$'\n'}"
		[[ $out == *"Port 1"* ]] || { echo "POST $route: no port line in: $out"; exit 1; }
		continue ;;
	esac
	body=$(sw_get "${route#/}$val") || exit 1	# sw_get fails on anything but 2xx
	if [[ $kind == json ]]; then	# text pages (/config, /cmd_log) may be empty
		printf '%s' "$body" | python3 -m json.tool >/dev/null 2>&1 || { echo "GET $route$val: not JSON: $body"; exit 1; }
	fi
done <<< "$ROUTES"

# the pages the front page itself loads
for p in "" app.js favicon.ico login.html; do
	sw_get "$p" >/dev/null || exit 1
done
