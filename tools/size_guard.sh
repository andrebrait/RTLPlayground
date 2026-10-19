#!/bin/sh
# Fails when HEAD (committed) is larger than <base-ref> on a guarded build:
# ROM/EPROM/FLASH grew or the stack headroom shrank. EXTERNAL RAM growth is
# allowed (only the .bin size is bound; XRAM has room), so it is reported with
# an "(xram grew, allowed)" note and does not fail.
# Both commits are built in clean worktrees, so local edits do not skew sizes.
# Sizes come from output/<MACHINE>/rtlplayground.mem. Needs sdcc on PATH, as CI does.
# Usage: tools/size_guard.sh <base-ref>
set -eu

[ $# -eq 1 ] || { echo "usage: $0 <base-ref>" >&2; exit 2; }

ROOT=$(git rev-parse --show-toplevel)
BASE_SHA=$(git -C "$ROOT" rev-parse --verify --quiet "$1^{commit}") \
	|| { echo "size_guard: unknown ref '$1'" >&2; exit 2; }
HEAD_SHA=$(git -C "$ROOT" rev-parse --verify HEAD)

# Worktrees live next to the checkout (inside the workspace) and are removed on exit.
TMP=$(mktemp -d "$(dirname "$ROOT")/size-guard.XXXXXX")
BASE_TREE="$TMP/base"
HEAD_TREE="$TMP/head"
cleanup() {
	git -C "$ROOT" worktree remove --force "$BASE_TREE" 2>/dev/null || true
	git -C "$ROOT" worktree remove --force "$HEAD_TREE" 2>/dev/null || true
	rm -rf "$TMP"
}
trap 'exit 130' INT
trap 'exit 143' TERM
trap cleanup EXIT
git -C "$ROOT" worktree add --detach --quiet "$BASE_TREE" "$BASE_SHA"
git -C "$ROOT" worktree add --detach --quiet "$HEAD_TREE" "$HEAD_SHA"

# Size column of a row in the "Other memory" table of the sdcc .mem file.
# Rows carry two hex addresses before Size; the row name can contain spaces.
mem_size() { # <mem-file> <row-name>
	awk -v row="$2" '$0 ~ ("^ *" row " +0x") { n = 0
		for (i = 1; i <= NF; i++) if ($i ~ /^0x/ && ++n == 2) { print $(i + 1); exit } }' "$1"
}

# Integer N from "... with N bytes available." in the stack line.
stack_free() { # <mem-file>
	sed -n 's/.* with \([0-9][0-9]*\) bytes available\..*/\1/p' "$1" | head -n 1
}

# measure <tree> <machine> [make-args...] sets ROM, XRAM and STACK.
measure() {
	tree=$1; machine=$2; shift 2
	(cd "$tree" && make clean && make CI=1 MACHINE="$machine" "$@") >"$TMP/build.log" 2>&1 || {
		cat "$TMP/build.log" >&2
		echo "size_guard: build failed in $tree: MACHINE=$machine $*" >&2
		exit 1
	}
	mem="$tree/output/$machine/rtlplayground.mem"
	ROM=$(mem_size "$mem" "ROM/EPROM/FLASH")
	XRAM=$(mem_size "$mem" "EXTERNAL RAM")
	STACK=$(stack_free "$mem")
	for v in "$ROM" "$XRAM" "$STACK"; do
		case $v in ''|*[!0-9]*) echo "size_guard: cannot parse sizes from $mem" >&2; exit 1 ;; esac
	done
}

status=0

# check <label> <machine> [make-args...] prints one line and records a regression.
check() {
	label=$1; machine=$2; shift 2
	measure "$BASE_TREE" "$machine" "$@"
	base_rom=$ROM; base_xram=$XRAM; base_stack=$STACK
	measure "$HEAD_TREE" "$machine" "$@"
	note=
	[ "$XRAM" -gt "$base_xram" ] && note=' (xram grew, allowed)'
	printf '%s rom %s->%s xram %s->%s stack %s->%s%s\n' "$label" \
		"$base_rom" "$ROM" "$base_xram" "$XRAM" "$base_stack" "$STACK" "$note"
	if [ "$ROM" -gt "$base_rom" ] || [ "$STACK" -lt "$base_stack" ]; then
		status=1
	fi
}

check KP_9000_6XHML_X2 KP_9000_6XHML_X2
check KP_9000_9XHML_X_V3_1 KP_9000_9XHML_X_V3_1
check SWTGW218AS SWTGW218AS
check "KP_9000_6XHML_X2 HEALTH=1" KP_9000_6XHML_X2 HEALTH=1

if [ "$status" -ne 0 ]; then
	echo "size_guard: HEAD is larger than $1 on at least one guarded build" >&2
fi
exit "$status"
