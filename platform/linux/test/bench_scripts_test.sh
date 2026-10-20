#!/bin/sh
# The bench's setup.sh and teardown.sh must never adopt or delete a directory that is not a bench's:
# setup.sh refuses a run directory without the owner marker; teardown.sh deletes one only with the
# marker and under /run/rtlp-bench* or /tmp/rtlp-bench*. Nothing here creates a namespace.
B=$(dirname "$0")/../bench
fail=0
check() { # <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok:   $1"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi
}
T=$(mktemp -d /tmp/rtlp-bench-test-XXXXXX) || exit 1
U=$(mktemp -d) || exit 1	# /tmp/tmp.XXXX: not a name a bench uses
trap 'rm -rf "$T" "$U"' EXIT

: > "$T/keep"
RTLP_BENCH_RUN=$T sh "$B/setup.sh" 2>/dev/null
check "setup.sh refuses a run directory with no owner marker (status 3)" 3 $?
check "...and leaves it alone" 1 "$(ls "$T/keep" 2>/dev/null | wc -l)"
RTLP_BENCH_RUN=$T sh "$B/teardown.sh"
check "teardown.sh does not delete a directory with no owner marker" 1 "$(ls "$T/keep" 2>/dev/null | wc -l)"
RTLP_BENCH_RUN=$T sh "$B/teardown.sh" --force
check "...not even with --force" 1 "$(ls "$T/keep" 2>/dev/null | wc -l)"

: > "$U/owner"; : > "$U/keep"
RTLP_BENCH_RUN=$U sh "$B/teardown.sh"
check "teardown.sh does not delete a marked directory outside /run/rtlp-bench* and /tmp/rtlp-bench*" 1 "$(ls "$U/keep" 2>/dev/null | wc -l)"

: > "$T/owner"
RTLP_BENCH_RUN=$T sh "$B/teardown.sh"
check "teardown.sh deletes a marked directory under /tmp/rtlp-bench*" 0 "$(ls -d "$T" 2>/dev/null | wc -l)"

L=/tmp/rtlp-bench-test-link-$$
ln -s "$U" "$L"
RTLP_BENCH_RUN=$L sh "$B/teardown.sh"
check "teardown.sh does not follow a symlink" 1 "$(ls "$U/keep" 2>/dev/null | wc -l)"
rm -f "$L"
exit $fail
