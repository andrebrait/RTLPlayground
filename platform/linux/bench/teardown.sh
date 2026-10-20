#!/bin/sh
# Stops the daemon lib.sh started and deletes the namespaces setup.sh created (deleting a
# namespace removes the veth ends in it, and with them the peer). Idempotent; touches nothing
# else. Always exits 0.
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
if [ -r "$RUN/pid" ]; then
	pid=$(cat "$RUN/pid")
	# Only kill what the pidfile still names as our daemon (the pid may be reused).
	[ "$(cat /proc/$pid/comm 2>/dev/null)" = rtlplayground ] && kill -KILL "$pid" 2>/dev/null
fi
rm -rf "$RUN"
for ns in sw host1 host2 host3 host4 host5 host6; do
	ip netns del $ns 2>/dev/null
done
exit 0
