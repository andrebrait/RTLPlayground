#!/bin/sh
# Stops the daemon lib.sh started and deletes the namespaces listed in $RUN/owner (what
# setup.sh created; deleting a namespace removes the veth ends in it, and with them the peer).
# Idempotent; without a marker it does nothing. With --force it also deletes every namespace
# named rtlp-sw / rtlp-h1..6, for cleaning up a stale bench. Namespaces of any other name are
# never touched. Always exits 0.
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
if [ -r "$RUN/pid" ]; then
	pid=$(cat "$RUN/pid")
	# Only kill what the pidfile still names as our daemon (the pid may be reused).
	[ "$(cat /proc/$pid/comm 2>/dev/null)" = rtlplayground ] && kill -KILL "$pid" 2>/dev/null
fi
owned=$(cat "$RUN/owner" 2>/dev/null)
[ "$1" = --force ] && owned="$owned rtlp-sw rtlp-h1 rtlp-h2 rtlp-h3 rtlp-h4 rtlp-h5 rtlp-h6"
for ns in $owned; do
	ip netns del $ns 2>/dev/null
done
rm -rf "$RUN"
exit 0
