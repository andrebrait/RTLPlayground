#!/bin/bash
# The web UI over HTTP: the login page before a password exists, login and session cookie, the JSON
# pages, POST /cmd, uploads, static files, /reset. Hostile requests are t_http_hostile.sh.
. "$(dirname "$0")/lib.sh"
SW_PASS=bench-pw
sw_start

swcurl() { ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 "$@"; }
status() { swcurl -o /dev/null -w '%{http_code}' "$@"; }
headers() { swcurl -i "$@" | tr -d '\r' | sed '/^$/q'; }	# the header block of a response
jar=$RUN/cookies	# sw_get's cookie jar
www=$BENCH_DIR/../../../output/html_min

# ---- no password yet: only the login page, with a notice; nothing else answers ----
for p in / /login.html; do
	page=$(swcurl "$SW_URL$p")
	assert_contains "GET $p is the login page" "<title>Switch Login</title>" "$page"
	assert_contains "GET $p tells to set a password on the console" "passwd &lt;password&gt;" "$page"
done
assert_eq "information.json before login" 401 "$(status "$SW_URL/information.json")"
assert_eq "/cmd before login" 401 "$(status -d 'mtu show' "$SW_URL/cmd")"
h=$(headers -d pwd=anything "$SW_URL/login")
assert_contains "login without a password set fails like a wrong password" "Location: login.html" "$h"
[[ $h != *Set-Cookie* ]] || { echo "a login with no password set got a cookie"; exit 1; }
assert_contains "the console line says why" "no password is set" "$(cat "$RUN/daemon.log")"

# ---- the password: no plaintext left, wrong one refused, right one gets a cookie ----
assert_eq "passwd prints nothing" "" "$(sw_cmd "passwd bench-pw")"
h=$(headers -d pwd=wrong "$SW_URL/login")
assert_contains "wrong password: back to the login page" "HTTP/1.1 302 Found" "$h"
assert_contains "wrong password: back to the login page" "Location: login.html" "$h"
[[ $h != *Set-Cookie* ]] || { echo "wrong password got a cookie"; exit 1; }
h=$(headers --data-urlencode 'pwd=bench-pw' "$SW_URL/login")
assert_contains "right password: to the front page" "Location: index.html" "$h"
[[ $h =~ Set-Cookie:\ session=[0-9a-f]{32}\; ]] || { echo "no 32-hex session cookie in: $h"; exit 1; }
page=$(swcurl "$SW_URL/login.html")
[[ $page != *"passwd &lt;password&gt;"* ]] || { echo "the notice is still shown with a password set"; exit 1; }
assert_contains "login page is served" "<title>Switch Login</title>" "$page"
assert_eq "front page without a session goes to the login page" "302 $SW_URL/login.html" "$(swcurl -o /dev/null -w '%{http_code} %{redirect_url}' "$SW_URL/")"
assert_eq "app.js without a session goes to the login page" 302 "$(status "$SW_URL/app.js")"
assert_eq "JSON without a session" 401 "$(status "$SW_URL/status.json")"
assert_eq "a made-up session" 401 "$(status -H 'Cookie: session=00000000000000000000000000000000' "$SW_URL/status.json")"

# ---- sw_get: logs in (cookie checked), fetches, fails on a non-2xx answer or a refused login ----
rm -f "$jar"
info=$(sw_get information.json) || exit 1
grep -q $'\tsession\t' "$jar" || { echo "sw_get left no session cookie"; exit 1; }
printf '%s' "$info" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert "hostname" in d' || { echo "information.json is not JSON with a hostname: $info"; exit 1; }
out=$( (sw_get nosuch.json) 2>&1) && { echo "sw_get accepted a 404"; exit 1; }
assert_contains "sw_get names the status" "HTTP 404" "$out"
out=$( (RUN=$RUN/alt; mkdir -p "$RUN"; SW_PASS=nope; sw_get status.json) 2>&1) && { echo "sw_get accepted a refused login"; exit 1; }
assert_contains "sw_get notices the refused login" "no session cookie" "$out"

# ---- the pages ----
assert_eq "status.json has the six ports" 6 "$(sw_get status.json | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')"
assert_eq "lldp is off before" '"on":0' "$(sw_get lldp.json | grep -o '"on":[01]')"
sw_cmd "lldp on" >/dev/null	# not supported on this platform yet: the state must stay off
assert_eq "lldp.json shows off after lldp on" '"on":0' "$(sw_get lldp.json | grep -o '"on":[01]')"
for p in mtu.json vlanlist 'vlan.json?vid=1' 'counters.json?port=1' 'l2.json?idx=0' mirror.json isolation.json cmd_log config; do
	assert_eq "GET /$p" 200 "$(status -b "$jar" "$SW_URL/$p")"
done
assert_eq "counters of a port that is not there" 400 "$(status -b "$jar" "$SW_URL/counters.json?port=99")"
assert_eq "unknown page" 404 "$(status -b "$jar" "$SW_URL/nosuch.json")"
assert_eq "no way out of the web directory" 404 "$(status -b "$jar" --path-as-is "$SW_URL/../../Makefile")"
assert_eq "no way out of the web directory, signed out" 401 "$(status --path-as-is "$SW_URL/../../Makefile")"

# ---- POST /cmd ----
assert_eq "/cmd mtu show answers what the CLI prints" "$(sw_cmd 'mtu show')" "$(swcurl -b "$jar" -d 'mtu show' "$SW_URL/cmd")"
assert_eq "six ports in /cmd mtu show" 6 "$(swcurl -b "$jar" -d 'mtu show' "$SW_URL/cmd" | grep -c '^Port [0-9]')"
assert_eq "a silent command says OK" $'200\nOK' "$(swcurl -b "$jar" -d 'hostname bench1' -w '%{http_code}\n' -o "$RUN/body" "$SW_URL/cmd"; cat "$RUN/body")"
assert_contains "and took effect" '"hostname":"bench1"' "$(sw_get information.json)"
out=$(swcurl -b "$jar" -d foo -w '\n%{http_code}' "$SW_URL/cmd")
assert_contains "a failing command is a 400 with the parser's text" "Unknown command" "$out"
assert_eq "a failing command is a 400" 400 "${out##*$'\n'}"
assert_eq "/cmd with text/plain (what app.js sends)" "$(sw_cmd 'mtu show')" "$(swcurl -b "$jar" -H 'Content-Type: text/plain;charset=UTF-8' --data-binary 'mtu show' "$SW_URL/cmd")"

# ---- uploads ----
printf 'hostname bench2\nmtu show\n' > "$RUN/cfg.txt"
assert_eq "config upload: stored text is not available yet" 500 "$(status -b "$jar" -F uploadedfile=@"$RUN/cfg.txt" "$SW_URL/config")"
assert_contains "the daemon took exactly the file's bytes" "(25 bytes) dropped" "$(cat "$RUN/daemon.log")"
assert_eq "config upload without a file part" 400 "$(status -b "$jar" -F field=value "$SW_URL/config")"
assert_eq "config upload needs a session" 401 "$(status -F uploadedfile=@"$RUN/cfg.txt" "$SW_URL/config")"
head -c 3000000 /dev/zero > "$RUN/fw.bin"
code=$(swcurl -b "$jar" -F uploadedfile=@"$RUN/fw.bin" -o "$RUN/body" -w '%{http_code}' "$SW_URL/upload")
assert_eq "firmware upload is not done here (3 MB body, the answer still arrives)" 501 "$code"
assert_eq "and says what to use" "Firmware upload is done with sysupgrade on Linux" "$(cat "$RUN/body")"
assert_eq "firmware upload needs a session" 401 "$(status -F uploadedfile=@"$RUN/fw.bin" "$SW_URL/upload")"

# ---- static files: the minified build, as it is on disk, with the types of tools/fileadder.c ----
assert_eq "favicon without a session is an svg" "200 image/svg+xml; charset=UTF-8" "$(swcurl -o /dev/null -w '%{http_code} %{content_type}' "$SW_URL/favicon.ico")"
assert_eq "app.js type" "200 text/javascript; charset=UTF-8" "$(swcurl -b "$jar" -o "$RUN/body" -w '%{http_code} %{content_type}' "$SW_URL/app.js")"
cmp -s "$RUN/body" "$www/app.js" || { echo "app.js differs from $www/app.js"; exit 1; }
assert_eq "index.html at /" "200 text/html; charset=UTF-8" "$(swcurl -b "$jar" -o "$RUN/body" -w '%{http_code} %{content_type}' "$SW_URL/")"
cmp -s "$RUN/body" "$www/index.html" || { echo "/ differs from index.html"; exit 1; }
[[ $(headers -b "$jar" "$SW_URL/app.js") != *Content-Encoding* ]] || { echo "app.js is served with a Content-Encoding it does not have"; exit 1; }

# ---- passwdhash: the password restored from a hash, as a saved configuration does; bad hashes refused ----
hash=$(openssl passwd -6 other-pw)
[[ $hash == '$6$'* ]] || { echo "no sha512 hash from openssl passwd -6: $hash"; exit 1; }
assert_contains "a plaintext is not a hash" "not a SHA-512 crypt hash" "$(sw_cmd "passwdhash other-pw")"
assert_contains "a truncated hash is refused" "not a SHA-512 crypt hash" "$(sw_cmd 'passwdhash $6$')"
assert_eq "still the old password" 302 "$(status -d pwd=bench-pw "$SW_URL/login")"
[[ $(headers -d pwd=bench-pw "$SW_URL/login") == *Set-Cookie* ]] || { echo "the old password stopped working after refused hashes"; exit 1; }
assert_eq "passwdhash prints nothing" "" "$(sw_cmd "passwdhash $hash")"
[[ $(headers -d pwd=bench-pw "$SW_URL/login") != *Set-Cookie* ]] || { echo "the old password works after passwdhash"; exit 1; }
assert_eq "the session of the old password is gone" 401 "$(status -b "$jar" "$SW_URL/status.json")"
[[ $(headers -d pwd=other-pw "$SW_URL/login") == *Set-Cookie* ]] || { echo "the password of the hash does not work"; exit 1; }

# ---- session timeout (seconds of inactivity, `session`) ----
sw_cmd "session 1" >/dev/null
SW_PASS=other-pw
rm -f "$jar"
sw_get mtu.json >/dev/null || exit 1
sleep 2.2
assert_eq "a session idle past session_timeout" 401 "$(status -b "$jar" "$SW_URL/status.json")"
sw_cmd "session 200" >/dev/null
rm -f "$jar"
sw_get mtu.json >/dev/null || exit 1
assert_eq "and a fresh one works" 200 "$(status -b "$jar" "$SW_URL/status.json")"

# ---- /reset: the daemon starts over (same process); the password was never saved, so it is gone ----
pid=$(cat "$RUN/pid")
assert_contains "/reset answers" "Restarting" "$(swcurl -b "$jar" "$SW_URL/reset")"
for _ in $(seq 50); do
	[[ $(swcurl "$SW_URL/") == *"passwd &lt;password&gt;"* ]] && break
	sleep 0.1
done
assert_contains "after /reset the daemon is back, with no password" "passwd &lt;password&gt;" "$(swcurl "$SW_URL/")"
assert_eq "same process" "$pid" "$(cat "$RUN/pid")"
kill -0 "$pid" || { echo "daemon died"; exit 1; }
assert_contains "CLI works after /reset" "Unknown command" "$(sw_cmd foo)"
