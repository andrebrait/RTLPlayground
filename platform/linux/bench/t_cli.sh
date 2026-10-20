#!/bin/bash
# The CLI socket next to the console: a command from the socket (or the web UI) must not disturb a
# half-typed console line; connections have limits (slots, lifetime) and a stuck one blocks nobody.
. "$(dirname "$0")/lib.sh"
SW_PASS=hunter2
sw_start
swcurl() { ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 "$@"; }
assert_eq "the CLI socket is private to its owner (mode 600)" 600 "$(stat -c %a "$RUN/cli.sock")"
alive() { kill -0 "$(cat "$RUN/pid")" 2>/dev/null && [[ $(sw_cmd foo) == *"Unknown command"* ]] && [[ $(sw_console foo) == *"Unknown command"* ]]; }

# ---- a console line half typed; a command through the socket; the line is finished ----
printf 'lldp of' >&3; sleep 0.3
assert_eq "swcli passwd" "" "$(sw_cmd "passwd hunter2")"
assert_eq "the half-typed line survived swcli: it ran as 'lldp off'" "LLDP disabled" "$(sw_console f)"
sleep 0.2
cookie_hdr=$(swcurl -i -d pwd=hunter2 "$SW_URL/login" | tr -d '\r')
[[ $cookie_hdr =~ session=([0-9a-f]{32}) ]] || { echo "the password set through swcli does not log in: $cookie_hdr"; exit 1; }
cookie=${BASH_REMATCH[1]}
# the same through the web UI
printf 'mtu sh' >&3; sleep 0.3
assert_contains "/cmd while the console has a half-typed line" "Unknown command" "$(swcurl -H "Cookie: session=$cookie" -d foo "$SW_URL/cmd")"
out=$(sw_console ow)
assert_eq "the half-typed line survived /cmd: it ran as 'mtu show'" 6 "$(grep -c '^Port [0-9]' <<<"$out")"
# a half-typed line with the cursor moved back in it
printf 'lldp ff\033[D\033[D' >&3; sleep 0.3
sw_cmd "version" >/dev/null
assert_eq "the cursor position survives, too: 'o' goes in front of 'ff'" "LLDP disabled" "$(sw_console o)"

# ---- an over-long line is answered, and the client reads the answer although it sent more ----
out=$(python3 - "$RUN/cli.sock" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(8); s.connect(sys.argv[1])
s.sendall(b"x" * 600)
data = b""
while True:
    d = s.recv(4096)
    if not d: break
    data += d
print(data.decode())
PY
)
assert_contains "overlong line" "Command too long" "$out"
alive || { echo "daemon not responding after the long line"; exit 1; }

# ---- swcli reading lines from stdin: a line over its buffer is refused, not sent in pieces ----
long=$(head -c 2000 /dev/zero | tr '\0' x)
out=$(printf '%s\nfoo\n' "$long" | "$SWCLI" -s "$RUN/cli.sock" 2>"$RUN/swcli.err")
assert_eq "only the short line ran" "Error: Unknown command" "$(tr -d '\r' <<<"$out" | sed '/^$/d')"
assert_contains "swcli says it did not send the long line" "line too long" "$(cat "$RUN/swcli.err")"
assert_contains "swcli --help warns about passwords as arguments" "process list" "$("$SWCLI" --help)"

# ---- lifetime: a client that dribbles a byte every 2 s is cut off after about 10 s ----
python3 - "$RUN/cli.sock" <<'PY' || exit 1
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
t0 = time.time()
while time.time() - t0 < 15:
    try:
        s.sendall(b"f")
    except OSError:
        break
    s.settimeout(2)
    try:
        if s.recv(1) == b"":
            break
    except socket.timeout:
        pass
el = time.time() - t0
if not 8 <= el <= 12:
    print("a dribbling CLI client lived %.1f s, expected about 10" % el); sys.exit(1)
PY
alive || { echo "daemon not responding after the dribbling client"; exit 1; }

# ---- connection slots: with the table full, more clients wait their turn, none is accepted and dropped ----
# 3 CLI slots are held by idle clients; the daemon is stopped while 3 more clients connect and send a
# command, so it finds all of them pending at once; it must serve them as slots free up.
python3 - "$RUN/cli.sock" "$(cat "$RUN/pid")" <<'PY' || exit 1
import os, signal, socket, sys, time
path, pid = sys.argv[1], int(sys.argv[2])
def conn():
    s = socket.socket(socket.AF_UNIX); s.connect(path); return s
holders = [conn() for _ in range(3)]
time.sleep(0.5)
os.kill(pid, signal.SIGSTOP)
try:
    clients = []
    for _ in range(3):
        s = conn(); s.sendall(b"foo\n"); clients.append(s)
finally:
    os.kill(pid, signal.SIGCONT)
bad = 0
for s in clients:
    s.settimeout(12)
    data = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            data += d
    except socket.timeout:
        pass
    if b"Unknown command" not in data:
        bad += 1
if bad:
    print("%d of 3 clients got no answer while the CLI slots were full" % bad); sys.exit(1)
PY
alive || { echo "daemon not responding at the end"; exit 1; }
