#!/bin/sh
# Drives the daemon's stdin/stdout console. The daemon reads its input from a file and writes
# to a file, so its own exit status is what `$?` holds (not a pipeline's last stage), and runs
# under `timeout -s KILL 5`: a hang is killed (status 137) and fails, as does any status but 0.
# Run after `make PLATFORM=linux`.
#
# `mtu show` is only counted here (one `Port N` line per board port): the values come from
# sysfs, which the namespace bench tests (Task 5), not this script.
BIN=${RTLP_BIN:-$(dirname "$0")/../output/rtlplayground}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { # <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok:   $1"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi
}

# run <input>: console output with CRs removed in $out, state dump included; the daemon's exit
# status in $rc.
run() {
	printf "$1" > "$TMP/in"
	timeout -s KILL 5 "$BIN" --console --dump-state --http 127.0.0.1:0 --www "$TMP" --cli-socket "$TMP/cli.sock" < "$TMP/in" > "$TMP/raw"
	rc=$?
	out=$(tr -d '\r' < "$TMP/raw")
}
count() { printf '%s\n' "$out" | grep -c "$1"; }
countx() { printf '%s\n' "$out" | grep -cx "$1"; }

run 'foo\n'
check "foo: daemon exits 0" 0 "$rc"
check "foo: unknown command reported once" 1 "$(count 'Unknown command')"

run 'mtu show\n'
check "mtu show: daemon exits 0" 0 "$rc"
check "mtu show lists six ports" 6 "$(count '^Port [0-9]')"

# Two lines in one write: the second must not overwrite the first while it runs.
run 'foo\nmtu show\n'
check "piped lines: daemon exits 0" 0 "$rc"
check "piped lines run in order" "1 6" "$(count 'Unknown command') $(count '^Port [0-9]')"

# LLDP needs a trap the platform cannot set: the command must say so and the state stay off.
# (The shared parser prints "LLDP enabled" before it asks, so the state dump is the truth.)
run 'lldp on\n'
check "lldp on: daemon exits 0" 0 "$rc"
check "lldp on: not supported" 1 "$(count 'Not supported on this platform yet')"
check "lldp on: state stays off" 1 "$(countx 'lldp_enabled=0')"
run 'lldp off\n'
check "lldp off: no error" 0 "$(count 'Not supported')"

# `passwd` leaves plaintext in the shared global; the platform hashes it and clears it after the command.
run 'passwd secret\n'
check "passwd: daemon exits 0" 0 "$rc"
check "passwd: hashed" 1 "$(countx 'has_password=1')"
check "passwd: plaintext cleared" 1 "$(countx 'passwd_plain_left=0')"
run 'foo\n'
check "no passwd: no password" 1 "$(countx 'has_password=0')"

# Escape sequences cmd_edit() does not handle would stall it for good, and bytes of one that
# leak into the line corrupt it: each is typed in the middle of "foo", which must be echoed
# exactly as `> foo` and run as one command.
while read -r name seq; do
	run "fo${seq}o\n"
	check "escape $name: daemon exits 0" 0 "$rc"
	check "escape $name: echoed line is '> foo'" 1 "$(countx '> foo')"
	check "escape $name: line runs" 1 "$(count 'Unknown command')"
done <<'SEQS'
CSI-Z \033[Z
CSI-1;5C \033[1;5C
SS3-F1 \033OP
CSI-overlong \033[12;34;56;78;90;12;34;56;78;90;12;34;56~
Alt-x \033x
SEQS
# ESC then Enter: the Enter must still end the line.
run 'foo\033\n'
check "ESC Enter: daemon exits 0" 0 "$rc"
check "ESC Enter: echoed line is '> foo'" 1 "$(countx '> foo')"
check "ESC Enter: line runs" 1 "$(count 'Unknown command')"
# A sequence the editor handles still works: cursor left, then insert.
run 'fo\033[Do\n'
check "cursor-left: daemon exits 0" 0 "$rc"
check "cursor-left: line runs" 1 "$(count 'Unknown command')"

exit $fail
