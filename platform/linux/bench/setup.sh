#!/bin/sh
# Creates the bench: namespace `rtlp-sw` (where the daemon runs) with lan1..lan6, each the veth
# peer of h<N> in its own namespace rtlp-h<N> (192.0.2.1<N>/24, TEST-NET-1). Needs root.
# Every namespace it creates is recorded in $RUN/owner, which is all teardown.sh deletes.
# Fails, creating nothing, if a previous bench is still around (run `teardown.sh --force` then) or if
# $RUN exists and is not a bench's (it has no owner marker: it is somebody's directory, which teardown
# would have to delete, so it is not adopted). (Exit status 3 means exactly that: nothing was created,
# so nothing may be torn down.)
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
if [ -e "$RUN/owner" ] || ip netns list | grep -q '^rtlp-'; then
	echo "setup: a bench already exists (stale run?); run teardown.sh --force" >&2
	exit 3
fi
if [ -e "$RUN" ]; then
	echo "setup: $RUN exists and is not a bench's run directory (no owner marker); remove it or set RTLP_BENCH_RUN" >&2
	exit 3
fi
set -e
mkdir "$RUN"
: > "$RUN/owner"
add_ns() { ip netns add "$1"; echo "$1" >> "$RUN/owner"; }
add_ns rtlp-sw
ip -n rtlp-sw link set lo up
for n in 1 2 3 4 5 6; do
	add_ns rtlp-h$n
	ip link add lan$n netns rtlp-sw type veth peer name h$n netns rtlp-h$n
	ip -n rtlp-sw link set lan$n up
	ip -n rtlp-h$n link set lo up
	ip -n rtlp-h$n addr add 192.0.2.1$n/24 dev h$n
	ip -n rtlp-h$n link set h$n up
done
