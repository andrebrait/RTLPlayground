#!/bin/bash
# Hostile HTTP: an over-long header, a request dribbled one byte at a time, a body that never
# completes, a POST without a length, a command without a session. The daemon must answer as
# httpd.c does, never hang, never let one client block the console, the CLI socket or the others.
. "$(dirname "$0")/lib.sh"
sw_start
sw_cmd "passwd bench-pw" >/dev/null
SW_PASS=bench-pw
sw_get status.json >/dev/null || exit 1
cookie=$(awk -F'\t' '$6 == "session" { print $7 }' "$RUN/cookies")
[[ $cookie =~ ^[0-9a-f]{32}$ ]] || { echo "no session cookie in the jar"; exit 1; }

alive() { kill -0 "$(cat "$RUN/pid")" 2>/dev/null && [[ $(sw_cmd foo) == *"Unknown command"* ]] && [[ $(sw_console foo) == *"Unknown command"* ]]; }

# raw <timeout>: stdin goes to port 80 inside rtlp-sw as it is, then the reply is read until the server closes.
raw() { ip netns exec rtlp-sw bash -c 'exec 5<>/dev/tcp/127.0.0.1/80; cat >&5; timeout "$1" cat <&5' _ "$1"; }
first_line() { head -n1 | tr -d '\r'; }

# ---- a 64 KiB header line: 400, the daemon lives ----
t0=$SECONDS
out=$(printf 'GET /%065536d HTTP/1.1\r\n\r\n' 0 | raw 8 | first_line)
assert_eq "64 KiB request line" "HTTP/1.1 400 Bad Request" "$out"
(( SECONDS - t0 < 6 )) || { echo "the 400 took $((SECONDS - t0)) s"; exit 1; }
alive || { echo "daemon not responding after the long header"; exit 1; }
# many small header lines past the limit are the same
out=$( { printf 'GET / HTTP/1.1\r\n'; for i in $(seq 700); do printf 'X-Pad-%04d: 0123456789\r\n' "$i"; done; printf '\r\n'; } | raw 8 | first_line)
assert_eq "headers over 8 KiB in total" "HTTP/1.1 400 Bad Request" "$out"
assert_eq "not HTTP" "HTTP/1.1 400 Bad Request" "$(printf 'FOO / HTTP/1.1\r\n\r\n' | raw 8 | first_line)"
assert_eq "a NUL in the header" "HTTP/1.1 400 Bad Request" "$(printf 'GET / HTTP/1.1\r\nX: a\0b\r\n\r\n' | raw 8 | first_line)"

# ---- dribbled one byte at a time: served ----
out=$(ip netns exec rtlp-sw bash -c '
	exec 5<>/dev/tcp/127.0.0.1/80
	req=$'"'"'GET /login.html HTTP/1.1\r\nHost: x\r\n\r\n'"'"'
	for ((i = 0; i < ${#req}; i++)); do printf "%s" "${req:i:1}" >&5; sleep 0.03; done
	timeout 8 cat <&5')
assert_contains "dribbled request" "HTTP/1.1 200 OK" "$out"
assert_contains "dribbled request gets the page" "<title>Switch Login</title>" "$out"

# ---- Content-Length 100, 10 bytes sent, then silence: closed after the read timeout, others unharmed meanwhile ----
t0=$(date +%s.%N)
printf 'POST /login HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 100\r\n\r\npwd=abcdef' | raw 12 > "$RUN/stall.out" &
stall=$!
sleep 0.5
alive || { echo "console or CLI blocked by a stalled request"; exit 1; }
assert_eq "web page while another request stalls" 200 "$(ip netns exec rtlp-sw curl -s -m 2 -o /dev/null -w '%{http_code}' "$SW_URL/login.html")"
wait $stall
el=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%d", b - a }')
(( el >= 4 && el <= 8 )) || { echo "the stalled request ended after ${el} s, expected about 5"; exit 1; }
assert_eq "a stalled request gets no answer" "" "$(cat "$RUN/stall.out")"
alive || { echo "daemon not responding after the stalled request"; exit 1; }
# ... and the same with the client closing: no hang, no leaked slot (20 > the 8 slots)
for i in $(seq 20); do
	printf 'POST /login HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 100\r\n\r\npwd=ab' | ip netns exec rtlp-sw bash -c 'exec 5<>/dev/tcp/127.0.0.1/80; cat >&5; exec 5>&-'
done
assert_eq "web page after 20 abandoned requests" 200 "$(ip netns exec rtlp-sw curl -s -m 2 -o /dev/null -w '%{http_code}' "$SW_URL/login.html")"

# ---- the connection table full of stuck clients: console and CLI socket still answer ----
ip netns exec rtlp-sw bash -c 'for fd in 10 11 12 13 14 15 16 17 18 19; do eval "exec $fd<>/dev/tcp/127.0.0.1/80"; printf "GET /" >&$fd; done; sleep 3' &
holders=$!
sleep 0.5
alive || { echo "console or CLI blocked by 10 stuck HTTP clients"; exit 1; }
wait $holders
assert_eq "web page once the stuck clients are gone" 200 "$(ip netns exec rtlp-sw curl -s -m 5 -o /dev/null -w '%{http_code}' "$SW_URL/login.html")"

# ---- POST without a length: 411 ----
assert_eq "login without Content-Length" "HTTP/1.1 411 Length Required" "$(printf 'POST /login HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n' | raw 8 | first_line)"
assert_eq "cmd without Content-Length" "HTTP/1.1 411 Length Required" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nCookie: session=%s\r\n\r\n' "$cookie" | raw 8 | first_line)"
assert_eq "cmd with Content-Length 0" "HTTP/1.1 411 Length Required" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nCookie: session=%s\r\n\r\n' "$cookie" | raw 8 | first_line)"
assert_eq "cmd body over the command buffer" "HTTP/1.1 400 Bad Request" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 128\r\nCookie: session=%s\r\n\r\n' "$cookie" | raw 8 | first_line)"
assert_eq "Content-Length that is not a number" "HTTP/1.1 400 Bad Request" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 1x\r\nCookie: session=%s\r\n\r\n' "$cookie" | raw 8 | first_line)"
assert_eq "POST without Content-Type" "HTTP/1.1 404 Not found" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Length: 3\r\n\r\nmtu' | raw 8 | first_line)"

# ---- an answer that comes before the body: the client keeps sending 3 MB and still gets to read the 501 ----
out=$( { printf 'POST /upload HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=x\r\nContent-Length: 3000000\r\nCookie: session=%s\r\n\r\n' "$cookie"; head -c 3000000 /dev/zero; } | raw 8 | first_line)
assert_eq "unread upload body" "HTTP/1.1 501 Not Implemented" "$out"
assert_eq "config upload that is not multipart" "HTTP/1.1 400 Bad Request" "$(printf 'POST /config HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 3\r\nCookie: session=%s\r\n\r\nabc' "$cookie" | raw 8 | first_line)"
alive || { echo "daemon not responding after the big upload"; exit 1; }

# ---- a multipart upload without a usable boundary is a bad request, not a parse of nothing ----
# (the body is shaped so that a missing or empty boundary would still find a "filename" part)
mp() { # <Content-Type> <body>: POST /config with the session
	printf 'POST /config HTTP/1.1\r\nContent-Type: %s\r\nContent-Length: %d\r\nCookie: session=%s\r\n\r\n%s' "$1" "${#2}" "$cookie" "$2" | raw 8 | first_line
}
assert_eq "multipart without a boundary parameter" "HTTP/1.1 400 Bad Request" "$(mp 'multipart/form-data' $'filename\r\n\r\nhello--')"
assert_eq "multipart with an empty boundary" "HTTP/1.1 400 Bad Request" "$(mp 'multipart/form-data; boundary=' $'--\r\nContent-Disposition: form-data; name=a; filename=b\r\n\r\nhello\r\n----')"
assert_eq "multipart with a boundary and a file part is taken (stored by the config store, not there yet: 500)" "HTTP/1.1 500 Internal Server Error" \
	"$(mp 'multipart/form-data; boundary=XX' $'--XX\r\nContent-Disposition: form-data; name=a; filename=b\r\n\r\nhello\r\n--XX--')"

# ---- the connection table full: more clients wait in the kernel backlog, none is accepted and dropped ----
# 7 of the 8 slots are held by silent clients; the daemon is stopped while 3 more connect and send a
# request, so that it finds them pending together. They must all be served, as the holders time out.
ip netns exec rtlp-sw python3 - "$(cat "$RUN/pid")" <<'PY' || exit 1
import os, signal, socket, sys, time
pid = int(sys.argv[1])
def conn():
    s = socket.create_connection(("127.0.0.1", 80)); return s
holders = [conn() for _ in range(7)]
time.sleep(0.5)
os.kill(pid, signal.SIGSTOP)
try:
    clients = []
    for _ in range(3):
        s = conn(); s.sendall(b"GET /login.html HTTP/1.1\r\n\r\n"); clients.append(s)
finally:
    os.kill(pid, signal.SIGCONT)
bad = 0
for s in clients:
    s.settimeout(12)
    data = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            data += d
    except OSError:
        pass
    if not data.startswith(b"HTTP/1.1 200 OK"):
        bad += 1
if bad:
    print("%d of 3 clients were not served while the table was full" % bad); sys.exit(1)
PY
alive || { echo "daemon not responding after the full table"; exit 1; }

# ---- /cmd without a session: 401 ----
assert_eq "cmd without a cookie" "HTTP/1.1 401 Unauthorized" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 8\r\n\r\nmtu show' | raw 8 | first_line)"
assert_eq "cmd with a wrong cookie" "HTTP/1.1 401 Unauthorized" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nCookie: session=%032d\r\n\r\nmtu show' 0 | raw 8 | first_line)"
assert_eq "cmd with the cookie among others" "HTTP/1.1 200 OK" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nCookie: a=b; session=%s; c=d\r\n\r\nmtu show' "$cookie" | raw 8 | first_line)"
assert_eq "a cookie that only ends in session" "HTTP/1.1 401 Unauthorized" "$(printf 'POST /cmd HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nCookie: xsession=%s\r\n\r\nmtu show' "$cookie" | raw 8 | first_line)"
alive || { echo "daemon not responding at the end"; exit 1; }
