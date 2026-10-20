# Sourced by the t_*.sh tests (bash). The test's own shell owns the daemon's console FIFOs, so
# sw_cmd works in the process that called sw_start.
#
# Transport: until the CLI socket exists (Task 3) the daemon runs with --console on a FIFO pair
# and sw_cmd types a line and reads the reply up to the next prompt. Task 3 swaps only sw_cmd.
BENCH_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${RTLP_BIN:-$BENCH_DIR/../output/rtlplayground}
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
SW_URL=${SW_URL:-http://127.0.0.1}	# the web UI inside `rtlp-sw`
SW_PASS=${SW_PASS:-benchpass}
SW_TIMEOUT=${SW_TIMEOUT:-5}	# seconds without a console byte (or an HTTP answer) before giving up

die() { echo "bench: $*" >&2; exit 1; }

# sw_start: runs the daemon inside `rtlp-sw`; returns once its first prompt arrived.
sw_start() {
	mkdir -p "$RUN" && mkfifo "$RUN/in" "$RUN/out" || die "cannot create $RUN fifos"
	exec 3<>"$RUN/in" 4<>"$RUN/out"	# held read-write so neither end blocks or sees EOF
	# fds 3/4 are closed in the daemon: it must not be a writer of its own input.
	ip netns exec rtlp-sw "$BIN" --console <&3 >&4 3>&- 4>&- &
	echo $! > "$RUN/pid"
	_sw_read_prompt >/dev/null || die "daemon gave no prompt"
}

# _sw_read_prompt: reads console output up to the next prompt into $REPLY_RAW (CRs removed).
# Fails after SW_TIMEOUT seconds without a byte, or 30 s in total.
# shortcut: the prompt is the 3 bytes "\n> ", so a reply that itself ends in them looks finished;
# upgrade to a framed transport with the CLI socket (Task 3).
_sw_read_prompt() {
	local c quiet=0 n=0 cap=$((SECONDS + 30))
	REPLY_RAW=
	# Per-byte work is kept minimal: bash spends about 50 us on each byte it reads.
	while :; do
		if IFS= read -r -N 1 -t 1 -u 4 c; then
			REPLY_RAW+=$c
			quiet=0
			if [[ $c == " " && ${REPLY_RAW: -3} == $'\n> ' ]]; then
				REPLY_RAW=${REPLY_RAW//$'\r'/}
				return 0
			fi
			(( ++n % 4096 )) || (( SECONDS < cap )) || return 1
		elif (( ++quiet >= SW_TIMEOUT )); then
			return 1
		fi
	done
}

# sw_cmd "<cli line>": prints the command's output (echoed input line and next prompt removed).
sw_cmd() {
	kill -0 "$(cat "$RUN/pid" 2>/dev/null)" 2>/dev/null || die "daemon is not running"
	# Drop what an earlier command left unread (a late reply) so it cannot answer this one.
	# shortcut: only bytes older than 50 ms are dropped; a reply later than that still attaches.
	while IFS= read -r -N 4096 -t 0.05 -u 4 c; do :; done
	printf '%s\n' "$1" >&3
	_sw_read_prompt || die "no reply to '$1' within ${SW_TIMEOUT}s (got: $REPLY_RAW)"
	local out=${REPLY_RAW%$'\n> '}
	out=${out#*$'\n'}	# the first line is the editor's echo of the typed line
	out=${out%$'\n'}
	[[ -z $out ]] || printf '%s\n' "$out"
	return 0
}

# sw_get <path>: GET from the web UI inside `rtlp-sw`, logged in with SW_PASS (session cookie kept
# in $RUN). Login counts only if the jar then holds a `session` cookie; any non-2xx answer fails.
# Untested until Task 3: the daemon serves no HTTP yet.
sw_get() {
	local jar=$RUN/cookies code
	if ! grep -qs $'\tsession\t' "$jar"; then
		ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 -c "$jar" -d "pwd=$SW_PASS" -o /dev/null "$SW_URL/login"
		grep -qs $'\tsession\t' "$jar" || die "web login failed: no session cookie"
	fi
	code=$(ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 -b "$jar" -o "$RUN/body" -w '%{http_code}' "$SW_URL/$1")
	[[ $code == 2?? ]] || die "GET /$1: HTTP ${code:-none}"
	cat "$RUN/body"
}

# in_host N <cmd...>: runs a command inside namespace rtlp-hN.
in_host() { local n=$1; shift; ip netns exec "rtlp-h$n" "$@"; }

# Assertions: print the failing one and exit non-zero (run.sh reports it).
assert_eq() { # <what> <expected> <got>
	[[ $2 == "$3" ]] || { echo "assert_eq failed: $1: expected '$2', got '$3'"; exit 1; }
}
assert_contains() { # <what> <needle> <haystack>
	[[ $3 == *"$2"* ]] || { echo "assert_contains failed: $1: '$2' not in '$3'"; exit 1; }
}
