#!/bin/bash
# Runs every t_*.sh on a fresh bench (setup before, teardown after, even on failure). A stale
# bench from an earlier run makes setup refuse (status 3): the run then stops without touching it;
# clean it with teardown.sh --force.
# Needs root, a built daemon (make PLATFORM=linux) and the packages in README.md.
cd "$(dirname "$0")" || exit 1
log=$(mktemp) || exit 1
fail=0
trap './teardown.sh; rm -f "$log"' EXIT
for t in t_*.sh; do
	name=${t%.sh}
	./setup.sh > "$log" 2>&1
	rc=$?
	if [ $rc = 3 ]; then
		echo "FAIL $name"; sed 's/^/     /' "$log"
		trap 'rm -f "$log"' EXIT	# the bench found is not ours to delete
		exit 1
	fi
	# Output goes to a file, not a $(...) pipe: the daemon inherits stderr and would keep the pipe open.
	if [ $rc = 0 ] && timeout -s KILL "${RTLP_TEST_TIMEOUT:-60}" bash "./$t" >> "$log" 2>&1; then
		echo "ok $name"
	else
		echo "FAIL $name"
		sed 's/^/     /' "$log"
		fail=1
	fi
	./teardown.sh
done
exit $fail
