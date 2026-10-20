#!/bin/sh
# Drives the daemon's stdin/stdout console. Every invocation is killed after 5 s, so a daemon
# that hangs fails the test instead of stalling CI, and must exit 0. Run after
# `make PLATFORM=linux`.
#
# `mtu show` is only counted here (one `Port N` line per board port): the values come from
# sysfs, which the namespace bench tests (Task 5), not this script.
BIN=${RTLP_BIN:-$(dirname "$0")/../output/rtlplayground}
fail=0
check() { # <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok:   $1"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi
}

# run <input>: console output with CRs removed in $out; the daemon's exit status in $rc.
run() {
	out=$(printf "$1" | timeout -s KILL 5 "$BIN" --console | tr -d '\r')
	rc=$?
}
count() { printf '%s\n' "$out" | grep -c "$1"; }

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

# LLDP needs a trap the platform cannot set: the command must say so. (The shared parser prints
# "LLDP enabled" before it asks; the platform answers by keeping lldp_enabled off, which only
# lldp.json shows and the HTTP tests check.)
run 'lldp on\n'
check "lldp on: daemon exits 0" 0 "$rc"
check "lldp on: not supported" 1 "$(count 'Not supported on this platform yet')"
run 'lldp off\n'
check "lldp off: no error" 0 "$(count 'Not supported')"

# Escape sequences cmd_edit() does not handle would stall it for good: each is typed in the
# middle of "foo", which must still run as one command.
while read -r name seq; do
	run "fo${seq}o\n"
	check "escape $name: daemon exits 0" 0 "$rc"
	check "escape $name: line intact" 1 "$(count 'Unknown command')"
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
check "ESC Enter: line runs" 1 "$(count 'Unknown command')"
# A sequence the editor handles still works: cursor left, then insert.
run 'fo\033[Do\n'
check "cursor-left: daemon exits 0" 0 "$rc"
check "cursor-left: line runs" 1 "$(count 'Unknown command')"

exit $fail
