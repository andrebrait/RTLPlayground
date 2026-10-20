#!/bin/sh
# Drives the daemon's stdin/stdout console: an unknown word is rejected once,
# `mtu show` lists one line per board port. Run after `make PLATFORM=linux`.
BIN=${RTLP_BIN:-$(dirname "$0")/../output/rtlplayground}
fail=0
check() { # <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok:   $1"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi
}

out=$(printf 'foo\n' | "$BIN" --console | tr -d '\r')
check "unknown command reported once" 1 "$(printf '%s\n' "$out" | grep -c 'Unknown command')"

out=$(printf 'mtu show\n' | "$BIN" --console | tr -d '\r')
check "mtu show lists six ports" 6 "$(printf '%s\n' "$out" | grep -c '^Port [0-9]')"

# Two lines in one write: the second must not overwrite the first while it runs.
out=$(printf 'foo\nmtu show\n' | "$BIN" --console | tr -d '\r')
check "piped lines run in order" "1 6" \
	"$(printf '%s\n' "$out" | grep -c 'Unknown command') $(printf '%s\n' "$out" | grep -c '^Port [0-9]')"

# An escape sequence the editor does not know would stall it for good; the console must survive it.
out=$(printf '\033[Zfoo\n' | timeout 5 "$BIN" --console | tr -d '\r')
check "unknown escape sequence does not hang" 1 "$(printf '%s\n' "$out" | grep -c 'Unknown command')"

exit $fail
