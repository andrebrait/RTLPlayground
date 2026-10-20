#!/bin/bash
# The daemon starts in `sw`, lan1..lan6 exist and are up, the console answers.
. "$(dirname "$0")/lib.sh"
sw_start
for n in 1 2 3 4 5 6; do
	st=$(ip -n sw -br link show lan$n 2>&1) || assert_eq "lan$n exists" present "absent: $st"
	assert_contains "lan$n is up" " UP " "$st"
done
assert_contains "foo is unknown" "Unknown command" "$(sw_cmd foo)"
