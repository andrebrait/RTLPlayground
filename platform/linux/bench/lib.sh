# Sourced by the t_*.sh tests (bash). The test's own shell owns the daemon's console FIFOs, so
# sw_console works in the process that called sw_start.
#
# Two ways to type a command: sw_cmd runs it through the CLI socket with swcli (one connection per
# command, the reply is exactly the command's output); sw_console types it into the daemon's
# --console over a FIFO pair and reads up to the next prompt, which is what a serial console user sees.
BENCH_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${RTLP_BIN:-$BENCH_DIR/../output/rtlplayground}
SWCLI=${RTLP_SWCLI:-$(dirname "$BIN")/swcli}
WWW=${RTLP_WWW:-$BENCH_DIR/../../../output/html_min}	# the web UI the daemon serves (--www)
DHCP_SCRIPT=${RTLP_DHCP_SCRIPT:-$BENCH_DIR/../udhcpc.script}	# the udhcpc script (--dhcp-script)
RUN=${RTLP_BENCH_RUN:-/run/rtlp-bench}
SW_CFG=${SW_CFG:-$RUN/cfg}	# the daemon's --cfg-dir (inside $RUN, so teardown removes it)
SW_URL=${SW_URL:-http://127.0.0.1}	# the web UI inside `rtlp-sw`
SW_PASS=${SW_PASS:-benchpass}
SW_TIMEOUT=${SW_TIMEOUT:-5}	# seconds without a console byte (or a CLI/HTTP answer) before giving up

die() { echo "bench: $*" >&2; exit 1; }

# sw_start: runs the daemon inside `rtlp-sw` (HTTP on its default port 80, CLI socket $RUN/cli.sock,
# its stderr in $RUN/daemon.log); returns once its first prompt arrived, which it prints after
# both sockets are listening.
sw_start() {
	mkdir -p "$RUN" && mkfifo "$RUN/in" "$RUN/out" || die "cannot create $RUN fifos"
	exec 3<>"$RUN/in" 4<>"$RUN/out"	# held read-write so neither end blocks or sees EOF
	# fds 3/4 are closed in the daemon: it must not be a writer of its own input.
	ip netns exec rtlp-sw "$BIN" --console --cli-socket "$RUN/cli.sock" --www "$WWW" --cfg-dir "$SW_CFG" --dhcp-script "$DHCP_SCRIPT" <&3 >&4 2>>"$RUN/daemon.log" 3>&- 4>&- &
	echo $! > "$RUN/pid"
	_sw_read_prompt >/dev/null || die "daemon gave no prompt"
}

# sw_stop: kills the daemon sw_start ran and removes its fifos, so that sw_start can run again (a
# restart on the same $SW_CFG). Exported variables before sw_start (VAR=x sw_start) reach the daemon.
sw_stop() {
	local pid; pid=$(cat "$RUN/pid") && kill -KILL "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	exec 3>&- 4>&-
	rm -f "$RUN/in" "$RUN/out" "$RUN/pid" "$RUN/cli.sock"
}

# _sw_read_prompt: reads console output up to the next prompt into $REPLY_RAW (CRs removed).
# Fails after SW_TIMEOUT seconds without a byte, or 30 s in total.
# shortcut: the prompt is the 3 bytes "\n> ", so a reply that itself ends in them looks finished;
# that is why commands go through the CLI socket (sw_cmd), whose reply ends at the connection close.
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

# sw_console "<cli line>": prints the command's output (echoed input line and next prompt removed).
sw_console() {
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

# sw_cmd "<cli line>": runs it through the CLI socket and prints its output.
sw_cmd() {
	local out
	kill -0 "$(cat "$RUN/pid" 2>/dev/null)" 2>/dev/null || die "daemon is not running"
	out=$(timeout -s KILL "$SW_TIMEOUT" "$SWCLI" -s "$RUN/cli.sock" "$1") || die "swcli failed for '$1'"
	[[ -z $out ]] || printf '%s\n' "$out"
}

# sw_get <path>: GET from the web UI inside `rtlp-sw`, logged in with SW_PASS (session cookie kept
# in $RUN). Login counts only if the jar then holds a `session` cookie; any non-2xx answer fails.
# A failure calls die, which ends only the shell it runs in: call it as `x=$(sw_get p) || exit 1`.
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
