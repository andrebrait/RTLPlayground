#!/bin/sh
# `make install DESTDIR=...` puts the daemon, swcli, the web UI (with its .gz files) and the DHCP script at
# the paths the daemon's defaults name. Run after `make PLATFORM=linux`.
OUT=$(dirname "$0")/../output
WWW=$(dirname "$0")/../../../output/html_min
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { # <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok:   $1"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi
}
make -C "$(dirname "$0")/.." install DESTDIR="$TMP/root" > "$TMP/make.log" 2>&1
check "make install" 0 $?
R=$TMP/root/usr
check "daemon" 755 "$(cmp "$OUT/rtlplayground" "$R/sbin/rtlplayground" && stat -c %a "$R/sbin/rtlplayground")"
check "swcli" 755 "$(cmp "$OUT/swcli" "$R/bin/swcli" && stat -c %a "$R/bin/swcli")"
check "udhcpc.script" 755 "$(cmp "$(dirname "$0")/../udhcpc.script" "$R/share/rtlplayground/udhcpc.script" && stat -c %a "$R/share/rtlplayground/udhcpc.script")"
n=0
for f in "$WWW"/*; do
	b=${f##*/}
	cmp -s "$f" "$R/share/rtlplayground/www/$b" || { echo "FAIL: www/$b differs or is missing"; fail=1; }
	n=$((n + 1))
done
check "every file of the web UI is installed (count)" "$n" "$(ls "$R/share/rtlplayground/www" | wc -l)"
check "login.html is installed, its gzip copy is not" "login.html" "$(ls "$R/share/rtlplayground/www" | grep '^login')"
check "app.js.gz is installed" "app.js.gz" "$(ls "$R/share/rtlplayground/www" | grep '^app.js.gz$')"
check "the installed daemon runs" "usage:" "$("$R/sbin/rtlplayground" --bogus 2>&1 | cut -c1-6 | head -1)"
exit $fail
