# Sourced by the t_*.sh tests (bash). The test's own shell owns the daemon's console FIFOs, so
# sw_cmd works in the process that called sw_start.
#
# Transport: until the CLI socket exists (Task 3) the daemon runs with --console on a FIFO pair
# and sw_cmd types a line and reads the reply up to the next prompt. Task 3 swaps only sw_cmd.
BENCH_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${RTLP_BIN:-$BENCH_DIR/../output/rtlplayground}
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
SW_URL=${SW_URL:-http://127.0.0.1}	# the web UI inside `sw`
SW_PASS=${SW_PASS:-benchpass}
SW_TIMEOUT=${SW_TIMEOUT:-5}	# seconds to wait for a console reply or an HTTP answer

die() { echo "bench: $*" >&2; exit 1; }

# sw_start: runs the daemon inside `sw`; returns once its first prompt arrived.
sw_start() {
	mkdir -p "$RUN" && mkfifo "$RUN/in" "$RUN/out" || die "cannot create $RUN fifos"
	exec 3<>"$RUN/in" 4<>"$RUN/out"	# held read-write so neither end blocks or sees EOF
	# fds 3/4 are closed in the daemon: it must not be a writer of its own input.
	ip netns exec sw "$BIN" --console <&3 >&4 3>&- 4>&- &
	echo $! > "$RUN/pid"
	_sw_read_prompt >/dev/null || die "daemon gave no prompt"
}

# _sw_read_prompt: reads console output up to the next "\n> " into $REPLY_RAW (CRs removed);
# fails after SW_TIMEOUT seconds in total.
_sw_read_prompt() {
	local c end=$((SECONDS + SW_TIMEOUT))
	REPLY_RAW=
	while [[ $REPLY_RAW != *$'\n> ' ]]; do
		IFS= read -r -N 1 -t 1 -u 4 c
		if [[ $? -eq 0 ]]; then
			[[ $c == $'\r' ]] || REPLY_RAW+=$c
		elif (( SECONDS >= end )); then
			return 1
		fi
	done
}

# sw_cmd "<cli line>": prints the command's output (echoed input line and next prompt removed).
sw_cmd() {
	kill -0 "$(cat "$RUN/pid" 2>/dev/null)" 2>/dev/null || die "daemon is not running"
	printf '%s\n' "$1" >&3
	_sw_read_prompt || die "no reply to '$1' within ${SW_TIMEOUT}s (got: $REPLY_RAW)"
	local out=${REPLY_RAW%$'\n> '}
	out=${out#*$'\n'}	# the first line is the editor's echo of the typed line
	out=${out%$'\n'}
	[[ -z $out ]] || printf '%s\n' "$out"
	return 0
}

# sw_get <path>: GET from the web UI inside `sw`, logged in with SW_PASS (session cookie kept in $RUN).
sw_get() {
	local jar=$RUN/cookies
	[[ -s $jar ]] || ip netns exec sw curl -sf -m "$SW_TIMEOUT" -c "$jar" -d "pwd=$SW_PASS" -o /dev/null "$SW_URL/login" || die "web login failed"
	ip netns exec sw curl -sf -m "$SW_TIMEOUT" -b "$jar" "$SW_URL/$1"
}

# in_host N <cmd...>: runs a command inside namespace hostN.
in_host() { local n=$1; shift; ip netns exec "host$n" "$@"; }

# Assertions: print the failing one and exit non-zero (run.sh reports it).
assert_eq() { # <what> <expected> <got>
	[[ $2 == "$3" ]] || { echo "assert_eq failed: $1: expected '$2', got '$3'"; exit 1; }
}
assert_contains() { # <what> <needle> <haystack>
	[[ $3 == *"$2"* ]] || { echo "assert_contains failed: $1: '$2' not in '$3'"; exit 1; }
}
