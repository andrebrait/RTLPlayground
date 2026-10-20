#!/bin/bash
# The daemon starts in `rtlp-sw`, lan1..lan6 exist and are up, the console answers.
. "$(dirname "$0")/lib.sh"
sw_start
assert_eq "daemon runs in rtlp-sw" "$(ip netns exec rtlp-sw readlink /proc/self/ns/net)" "$(readlink /proc/$(cat "$RUN/pid")/ns/net)"
for n in 1 2 3 4 5 6; do
	st=$(ip -n rtlp-sw -br link show lan$n 2>&1) || assert_eq "lan$n exists" present "absent: $st"
	assert_contains "lan$n is up" " UP " "$st"
done
assert_contains "foo is unknown" "Unknown command" "$(sw_cmd foo)"
# A reply nobody read must not attach to the next command: bar's reply is exactly foo's.
printf 'version\n' >&3; sleep 0.3
assert_eq "stale reply is drained" "$(sw_cmd foo)" "$(sw_cmd bar)"
