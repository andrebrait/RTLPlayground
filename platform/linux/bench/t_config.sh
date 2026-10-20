#!/bin/bash
# The startup-config store: POST /config writes <cfg-dir>/startup-config atomically with only password
# hashes on disk, the web UI's read-back equals what it sent, a restart applies the file, an
# interrupted save leaves the old file, an empty or missing file gets the built-in default.
. "$(dirname "$0")/lib.sh"
SW_PASS=bench-pw
jar=$RUN/cookies
cfg=$SW_CFG/startup-config

swcurl() { ip netns exec rtlp-sw curl -s -m "$SW_TIMEOUT" --max-redirs 0 "$@"; }
# login <pw>: 0 if the daemon gave a session cookie (cookie jar $jar), after waiting out the pause
# that a failed login causes
login() {
	rm -f "$jar"
	swcurl -c "$jar" --data-urlencode "pwd=$1" -o /dev/null "$SW_URL/login"
	local rc=1
	grep -qs $'\tsession\t' "$jar" && rc=0
	[[ $rc == 0 ]] || sleep 1.1
	return $rc
}
# post_cfg <file>: POST it the way the web UI does (multipart, one file part); prints the HTTP status
post_cfg() { swcurl -b "$jar" -F "configuration=@$1;filename=config.txt;type=application/octet-stream" -o /dev/null -w '%{http_code}' "$SW_URL/config"; }
get_cfg() { swcurl -b "$jar" "$SW_URL/config"; }
restart() { sw_stop; sw_start; }

# ---- the first password cannot be stored: it is set, the reason is printed, the history keeps it ----
RTLP_TEST_FAIL_INSTALL=1 sw_start
assert_eq "no file yet" "" "$(ls "$SW_CFG")"
out=$(sw_cmd "passwd failpw")
assert_contains "a password that could not be stored says so" "could not be saved" "$out"
assert_eq "nothing was stored" "" "$(ls "$SW_CFG")"
login failpw || { echo "the unsaved password is not a login"; exit 1; }
assert_eq "the history keeps the masked command" "passwd ****" "$(swcurl -b "$jar" "$SW_URL/cmd_log")"
sw_stop
rm -rf "$SW_CFG"

# ---- first start: no file; the default applies; the 8051's "1234" is not a login ----
sw_start
assert_eq "cfg dir mode" 700 "$(stat -c %a "$SW_CFG")"
assert_contains "no file: the default runs the shared 'vlan 1 mgmt'" "Management VLAN set to 0x0001" "$REPLY_RAW"
assert_eq "startup commands are not in the history" "" "$(sw_cmd history)"
sw_cmd version >/dev/null	# a command is what makes the daemon hash a plaintext password left over
login 1234 && { echo "the compiled-in default password 1234 is a login"; exit 1; }
assert_eq "no password in the file: no password set" 401 "$(swcurl -o /dev/null -w '%{http_code}' "$SW_URL/information.json")"
udhcpc_up() { local p n=0; for p in /proc/[0-9]*; do [[ $(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) == *"udhcpc -i swbr"* ]] && n=$((n + 1)); done; echo $n; }
assert_eq "the default started DHCP" 1 "$(udhcpc_up)"
sw_cmd "passwd bench-pw" >/dev/null
# the first password is stored at once, on its own line, and leaves nothing unsaved behind
assert_eq "the first password is on disk" 1 "$(grep -c '^passwdhash \$6\$' "$cfg")"
assert_eq "and it is the only line" 1 "$(wc -l < "$cfg")"
assert_eq "file mode" 600 "$(stat -c %a "$cfg")"
grep -q bench-pw "$cfg" && { echo "plaintext password on disk"; exit 1; }
assert_eq "the console history holds no password command" 0 "$(sw_cmd history | grep -c passwd)"
login bench-pw || { echo "login with the console password failed"; exit 1; }
assert_eq "/cmd_log holds none: nothing is unsaved" 0 "$(swcurl -b "$jar" "$SW_URL/cmd_log" | grep -c passwd)"
# a /reset keeps the login and, as the configuration only holds the password, runs the default again
pid=$(cat "$RUN/pid")
assert_contains "/reset answers" "Restarting" "$(swcurl -b "$jar" "$SW_URL/reset")"
for _ in $(seq 100); do login bench-pw && [[ $(udhcpc_up) == 1 ]] && break; sleep 0.1; done
login bench-pw || { echo "the first password is no login after /reset"; exit 1; }
assert_eq "the default ran after /reset: DHCP is on" 1 "$(udhcpc_up)"
assert_eq "same process" "$pid" "$(cat "$RUN/pid")"
assert_eq "/cmd_log is still empty" "" "$(swcurl -b "$jar" "$SW_URL/cmd_log")"
assert_eq "the file is unchanged" 1 "$(wc -l < "$cfg")"
# the effective configuration: the built-in default, then the file's lines (here only the password)
dflt=$'vlan 1 1 2 3 4 5 6\npvid 1 1\npvid 2 1\npvid 3 1\npvid 4 1\npvid 5 1\npvid 6 1\nvlan 1 mgmt\nip dhcp'
assert_eq "GET /config is the default followed by the passwdhash line" "$dflt"$'\n'"$(cat "$cfg")" "$(get_cfg)"

# ---- POST: plaintext password hashed, file 0600, the password-only file becomes the .bak ----
printf 'hostname bench1\npasswd bench-pw\n' > "$RUN/a.txt"
assert_eq "POST /config" 200 "$(post_cfg "$RUN/a.txt")"
assert_eq "file mode" 600 "$(stat -c %a "$cfg")"
assert_eq "the first password's file is now the .bak" "startup-config.bak" "$(ls "$SW_CFG" | grep -v '^startup-config$')"
assert_contains "hostname stored" $'hostname bench1\n' "$(cat "$cfg")"
[[ $(sed -n 2p "$cfg") =~ ^passwdhash\ \$6\$[^\ ]+$ ]] || { echo "line 2 is not a passwdhash line: $(cat "$cfg")"; exit 1; }
grep -q bench-pw "$cfg" && { echo "plaintext password on disk"; exit 1; }
assert_eq "UI read-back is what was sent" "$(cat "$RUN/a.txt")" "$(get_cfg)"
cp "$cfg" "$RUN/a.disk"

# ---- second POST: the old file becomes .bak ----
printf 'hostname bench2\npasswdhash %s\n' "$(sed -n 2p "$cfg" | cut -d' ' -f2)" > "$RUN/b.txt"
assert_eq "second POST" 200 "$(post_cfg "$RUN/b.txt")"
cmp -s "$SW_CFG/startup-config.bak" "$RUN/a.disk" || { echo ".bak is not the previous file"; exit 1; }
cmp -s "$cfg" "$RUN/b.txt" || { echo "a passwdhash upload was not kept as is"; exit 1; }
assert_eq "no .new left" "" "$(ls "$SW_CFG" | grep '\.new$')"
assert_eq "file mode of the replacement" 600 "$(stat -c %a "$cfg")"
assert_eq "UI read-back (passwdhash)" "$(cat "$RUN/b.txt")" "$(get_cfg)"

# ---- restart: the file applies; the password it carries is the login ----
restart
assert_contains "hostname applied" '"hostname":"bench2"' "$(login bench-pw && sw_get information.json)"
[[ $REPLY_RAW != *"Management VLAN set to"* ]] || { echo "the default ran although the file has commands"; exit 1; }
login 1234 && { echo "1234 logs in after a restart"; exit 1; }
login bench-pw || { echo "login with the saved password failed after a restart"; exit 1; }
assert_eq "GET /config after the restart" "$(cat "$RUN/b.txt")" "$(get_cfg)"

# ---- the UI's Save: the text carries the scrubbed `passwd ********` and a new password ----
oldhash=$(sed -n 2p "$cfg" | cut -d' ' -f2)
printf 'hostname ui\npasswd ********\npasswd newpw\nmtu show\n' > "$RUN/c.txt"
assert_eq "UI-style POST" 200 "$(post_cfg "$RUN/c.txt")"
assert_eq "UI read-back equals what it sent" "$(cat "$RUN/c.txt")" "$(get_cfg)"
grep -qE 'newpw|\*' "$cfg" && { echo "plaintext or asterisks on disk: $(cat "$cfg")"; exit 1; }
# only the last password line is written: the asterisks line goes, newpw's hash stays
assert_eq "one passwdhash line on disk" 1 "$(grep -c '^passwdhash ' "$cfg")"
assert_eq "line 1 and 3 are the others" $'hostname ui\nmtu show' "$(sed -n '1p;3p' "$cfg")"
newline=$(sed -n 2p "$cfg")
[[ $newline =~ ^passwdhash\ \$6\$ && $newline != "passwdhash $oldhash" ]] || { echo "line 2 is not the new hash: $(cat "$cfg")"; exit 1; }
assert_eq "the session survived the password change" 200 "$(swcurl -b "$jar" -o /dev/null -w '%{http_code}' "$SW_URL/information.json")"
login bench-pw && { echo "the old password still logs in"; exit 1; }
login newpw || { echo "the new password does not log in"; exit 1; }
restart
assert_eq "after a restart GET /config shows hashes" "hostname ui
$newline
mtu show" "$(login newpw && get_cfg)"
login bench-pw && { echo "the old password logs in after a restart"; exit 1; }
login newpw || { echo "newpw does not log in after a restart"; exit 1; }

# ---- an upload that is too large (also after hashing) or has no file is refused, nothing changes ----
before=$(cat "$cfg")
# 4094 bytes in, and the 20-byte passwd line becomes a 117-byte passwdhash line: 4191 bytes out
{ for i in $(seq 291); do echo "hostname fill"; done; echo "passwd abcdefghijkl"; } > "$RUN/big.txt"
assert_eq "an upload that grows past the area" 500 "$(post_cfg "$RUN/big.txt")"
assert_eq "file unchanged after a refused upload" "$before" "$(cat "$cfg")"
assert_eq "no .new left after a refused upload" "" "$(ls "$SW_CFG" | grep '\.new$')"

# ---- an interrupted save: the daemon dies after writing .new; the old file applies after a restart ----
cp "$cfg" "$RUN/c.disk"
sw_stop
RTLP_TEST_CRASH_AFTER_NEW=1 sw_start
login newpw || { echo "login before the crash test failed"; exit 1; }
printf 'hostname crashed\n' > "$RUN/d.txt"
code=$(post_cfg "$RUN/d.txt")
[[ $code != 200 ]] || { echo "the POST was answered 200 although the daemon should have died"; exit 1; }
wait "$(cat "$RUN/pid")"; assert_eq "daemon exit status at the simulated power loss" 1 $?
[[ -e $SW_CFG/startup-config.new ]] || { echo "no .new after the simulated crash"; exit 1; }
cmp -s "$cfg" "$RUN/c.disk" || { echo "startup-config changed by the interrupted save"; exit 1; }
sw_stop; sw_start
assert_eq "the old config applies" '"hostname":"ui"' "$(login newpw && sw_get information.json | grep -o '"hostname":"[^"]*"')"
[[ ! -e $SW_CFG/startup-config.new ]] || { echo "the stale .new was not removed"; exit 1; }

# ---- crash between the two renames: the old file (.bak) comes back; .new may be cut short, never used ----
sw_stop
mv "$cfg" "$cfg.bak"
printf 'hostname win' > "$cfg.new"
sw_start
assert_eq "the old file applies, not the .new" ui "$(sw_cmd hostname)"
cmp -s "$cfg" "$RUN/c.disk" && [[ ! -e $cfg.new ]] || { echo "the interrupted save was not rolled back to the old file"; exit 1; }
# a lone .new (the first save died while writing) is dropped, not applied
sw_stop
rm -f "$cfg" "$cfg.bak"
printf 'hostname partial' > "$cfg.new"
sw_start
assert_contains "no file: the default ran" "Management VLAN set to 0x0001" "$REPLY_RAW"
[[ ! -e $cfg.new && ! -e $cfg ]] || { echo "a lone .new was kept or promoted"; exit 1; }
assert_eq "hostname is not the partial one" "" "$(sw_cmd hostname)"

# ---- hand-edited files: plaintext is hashed in RAM, asterisks are dropped, the file is left alone ----
sw_stop
printf 'hostname hand\npasswd hand-pw\n' > "$cfg"
sw_start
assert_eq "plaintext passwd in a file: login" 0 "$(login hand-pw; echo $?)"
assert_eq "GET /config does not show it" "hostname hand" "$(get_cfg | grep -v passwdhash)"
assert_contains "but a hash" $'passwdhash $6$' "$(get_cfg)"
sw_stop
printf 'hostname hand\npasswd ******\n' > "$cfg"
sw_start
assert_eq "asterisks with no hash: no password" 1 "$(login '******'; echo $?)"
sw_cmd "passwd x" >/dev/null
login x || { echo "login with the console password failed"; exit 1; }
assert_eq "asterisks with no hash: the line is dropped, the first password is stored" "hostname hand
passwdhash" "$(get_cfg | sed 's/^\(passwdhash\) .*/\1/')"

# ---- CRLF line ends (an editor on Windows): the password line is still found and its ending kept ----
printf 'hostname crlf\r\npasswd crlf-pw\r\n' > "$RUN/crlf.txt"
assert_eq "CRLF POST" 200 "$(post_cfg "$RUN/crlf.txt")"
grep -q crlf-pw "$cfg" && { echo "plaintext password on disk (CRLF)"; exit 1; }
[[ $(sed -n 2p "$cfg") =~ ^passwdhash\ \$6\$[^\ ]+$'\r'$ ]] || { echo "CRLF: line 2 is not a passwdhash line ending in CR LF: $(od -c "$cfg" | head -3)"; exit 1; }
get_cfg | cmp -s - "$RUN/crlf.txt" || { echo "CRLF read-back differs"; exit 1; }
restart
login crlf-pw || { echo "login with the password from a CRLF file failed"; exit 1; }
assert_eq "CRLF file: hostname applied" crlf "$(sw_cmd hostname)"

# ======== fix round 1 ========
curpw=crlf-pw	# the password the daemon has now (post_cfg needs a session for it)
newest() { login "$curpw" || { echo "login with $curpw failed (line ${BASH_LINENO[0]})"; exit 1; }; }

# ---- CR is a line end, as in execute_config: no plaintext on disk, the password works, read-back equal ----
cr_case() { # <name> <printf format of the upload> <password it sets>
	newest
	printf "$2" > "$RUN/cr.txt"
	assert_eq "$1: POST" 200 "$(post_cfg "$RUN/cr.txt")"
	grep -q "$3" "$cfg" && { echo "$1: plaintext password on disk"; exit 1; }
	get_cfg | cmp -s - "$RUN/cr.txt" || { echo "$1: read-back differs"; exit 1; }
	curpw=$3; restart
	login "$3" || { echo "$1: login with the password failed after a restart"; exit 1; }
	assert_contains "$1: the file ran" "cr" "$(sw_cmd hostname)"
}
cr_case "CR inside" 'hostname cra\rpasswd midsecret\n' midsecret
cr_case "CR only" 'hostname crb\rpasswd cronly\r' cronly
cr_case "CR CR LF" 'hostname crc\r\npasswd crcr\r\r\n' crcr
assert_eq "CR CR LF endings are kept" ' \r \r \n' "$(tail -c 3 "$cfg" | od -An -c | tr -s ' ')"
newest	# the password is crcr, not 'crcr\r'

# ---- only the last password line goes to disk ----
h1=$(openssl passwd -6 firstpw); h2=$(openssl passwd -6 secondpw)
printf 'passwdhash %s\nhostname two\npasswdhash %s\n' "$h1" "$h2" > "$RUN/two.txt"
assert_eq "two passwdhash lines: POST" 200 "$(post_cfg "$RUN/two.txt")"
assert_eq "two passwdhash lines: one on disk, the last" $'hostname two\npasswdhash '"$h2" "$(cat "$cfg")"
get_cfg | cmp -s - "$RUN/two.txt" || { echo "two passwdhash lines: read-back differs"; exit 1; }
login firstpw && { echo "the first of two passwdhash lines is a login"; exit 1; }
login secondpw || { echo "the last of two passwdhash lines is not the login"; exit 1; }
printf 'passwdhash %s\nhostname bad\npasswdhash garbage\n' "$h1" > "$RUN/bad.txt"
assert_eq "valid hash then an invalid one: POST" 200 "$(post_cfg "$RUN/bad.txt")"
assert_eq "an invalid passwdhash does not displace the valid one" 'passwdhash '"$h1"$'\nhostname bad\npasswdhash garbage' "$(cat "$cfg")"
curpw=firstpw; newest

# ---- the UI's Save after each password change adds a passwdhash line: 20 cycles stay at one ----
printf 'hostname cyc\n' > "$RUN/clean.txt"; newest; post_cfg "$RUN/clean.txt" >/dev/null	# drops the garbage line
for i in $(seq 20); do
	newest
	sw_cmd "passwd cyc$i" >/dev/null
	login "cyc$i" || { echo "cycle $i: login with the new password failed"; exit 1; }	# a password change ends the session
	{ get_cfg; echo; echo 'passwd ****'; } | tr -s '\n' > "$RUN/cyc.txt"
	assert_eq "cycle $i: POST" 200 "$(post_cfg "$RUN/cyc.txt")"
	assert_eq "cycle $i: at most one passwdhash line on disk" 1 "$(grep -c '^passwdhash ' "$cfg")"
	curpw=cyc$i; restart
done
newest
(( $(stat -c %s "$cfg") < 400 )) || { echo "the file grew to $(stat -c %s "$cfg") bytes in 20 cycles"; exit 1; }

# ---- the password is cut at 20 characters like the shared passwd command ----
newest
printf 'hostname cut\npasswd abcdefghijklmnopqrstuvwxy\n' > "$RUN/cut.txt"
assert_eq "25-character password: POST" 200 "$(post_cfg "$RUN/cut.txt")"
login abcdefghijklmnopqrstuvwxy && { echo "the full 25 characters log in: not cut"; exit 1; }
login abcdefghijklmnopqrst || { echo "the first 20 characters do not log in"; exit 1; }
curpw=abcdefghijklmnopqrst; restart
login abcdefghijklmnopqrst || { echo "the first 20 characters do not log in after a restart"; exit 1; }

# ---- the history shows `passwd ****` whatever the password was: no characters, no length ----
newest; oldpw=$curpw
cp "$cfg" "$RUN/pw.disk"
for pw in a abcdefghijklmnopqrst; do sw_cmd "passwd $pw" >/dev/null; curpw=$pw; done
cmp -s "$cfg" "$RUN/pw.disk" || { echo "a later password change was written to the file: it keeps the 8051 model (save to keep)"; exit 1; }
login "$curpw" || { echo "login after the password changes failed"; exit 1; }
log=$(swcurl -b "$jar" "$SW_URL/cmd_log")
assert_eq "web history: two changed passwords of different length, the same masked line" $'passwd ****\npasswd ****' "$(grep passwd <<< "$log")"
assert_eq "console history: the same" $'passwd ****\npasswd ****' "$(sw_cmd history | grep passwd)"
# the ring (2048 bytes) wraps: 300 pairs of a command and a password of changing length; every password
# line of what is left is the masked one, the other lines are intact, and the entries still alternate
{ for i in $(seq 300); do echo "hostname h$i"; echo "passwd pw$(printf '%*s' $((i % 17)) '' | tr ' ' x)$i"; done; } | "$SWCLI" -s "$RUN/cli.sock" >/dev/null
login "pw$(printf '%*s' $((300 % 17)) '' | tr ' ' x)300" || { echo "login with the last password of the loop failed"; exit 1; }
log=$(swcurl -b "$jar" "$SW_URL/cmd_log")
assert_eq "wrapped history: no password text left" 0 "$(grep -c 'pw[x0-9]' <<< "$log")"
assert_eq "wrapped history: every line is a hostname command or the masked passwd" 0 "$(grep -cvE '^(hostname h[0-9]+|passwd \*\*\*\*)$' <<< "$log")"
assert_eq "wrapped history: the newest entry is the last password" "passwd ****" "$(tail -n 1 <<< "$log")"
assert_eq "wrapped history: entries alternate" 0 "$(awk 'NR > 1 && (($0 ~ /^passwd/) == (prev ~ /^passwd/)) { bad++ } { prev = $0 } END { print bad + 0 }' <<< "$log")"
assert_eq "wrapped history: the newest hostname is kept" "hostname h300" "$(tail -n 2 <<< "$log" | head -n 1)"
# one POST /cmd of 12 short passwd lines into a full ring: the masked entries are longer than what was typed
# (`passwd a` 9 bytes -> 12), the rewrite must stay inside its buffer (an overflow kills the daemon, ASan sees it)
body=$(for i in $(seq 12); do echo "passwd a"; done)
assert_eq "POST /cmd with 12 passwd lines" 200 "$(swcurl -b "$jar" --data-binary "$body" -o /dev/null -w '%{http_code}' "$SW_URL/cmd")"
kill -0 "$(cat "$RUN/pid")" 2>/dev/null || { echo "the daemon died on a POST /cmd of 12 passwd lines into a full ring"; cat "$RUN/daemon.log"; exit 1; }
login a || { echo "login with the password of the last passwd line failed"; exit 1; }
log=$(swcurl -b "$jar" "$SW_URL/cmd_log")
assert_eq "full ring + 12 passwd lines: no password text" 0 "$(grep -c 'passwd [^*]' <<< "$log")"
assert_eq "full ring + 12 passwd lines: the newest entry is masked" "passwd ****" "$(tail -n 1 <<< "$log")"
assert_eq "full ring + 12 passwd lines: the older entries are intact" 0 "$(grep -cvE '^(hostname h[0-9]+|passwd \*\*\*\*)$' <<< "$log")"
assert_eq "full ring + 12 passwd lines: the 12 are there" 12 "$(tail -n 12 <<< "$log" | grep -cx 'passwd \*\*\*\*')"
sw_cmd "passwd $oldpw" >/dev/null; curpw=$oldpw; newest	# the file still holds this one: runtime changes need a save to survive a restart

# ---- failures leave the old file, no .new, the old read-back and the old password ----
nofail() { # <what>: nothing changed since $RUN/before.disk / $beforeget
	assert_eq "$1: the file is unchanged" 0 "$(cmp -s "$cfg" "$RUN/before.disk"; echo $?)"
	assert_eq "$1: no .new is left" "" "$(ls "$SW_CFG" | grep '\.new$')"
	assert_eq "$1: GET /config is still the old text" "$beforeget" "$(get_cfg)"
	login failpw && { echo "$1: the password of the failed upload is a login"; exit 1; }
	newest
}
cp "$cfg" "$RUN/before.disk"; beforeget=$(get_cfg)
printf 'hostname failed\npasswd failpw\n' > "$RUN/fail.txt"
# .bak is a directory: the old file cannot be kept
rm -f "$cfg.bak"; mkdir -p "$cfg.bak/d"
assert_eq "cannot keep .bak: POST" 500 "$(post_cfg "$RUN/fail.txt")"
nofail "cannot keep .bak"
rm -r "$cfg.bak"
# .new is a directory: it cannot be written
mkdir "$cfg.new"
assert_eq "cannot write .new: POST" 500 "$(post_cfg "$RUN/fail.txt")"
rmdir "$cfg.new"
nofail "cannot write .new"
# the install rename fails (test hook): the old file must be put back
sw_stop; RTLP_TEST_FAIL_INSTALL=1 sw_start
newest; beforeget=$(get_cfg)
assert_eq "failed install: POST" 500 "$(post_cfg "$RUN/fail.txt")"
nofail "failed install"
[[ -f $cfg ]] || { echo "failed install: startup-config is gone"; exit 1; }
sw_stop; sw_start

# ---- an existing --cfg-dir with a lax mode is tightened; a directory or file that is not trusted is not read ----
sw_stop
chmod 755 "$SW_CFG"; sw_start
assert_eq "lax directory mode (not writable by others): tightened" 700 "$(stat -c %a "$SW_CFG")"
sw_stop
# A root daemon runs what is in the file, so a file that somebody else could have written is not read: the built-in
# default runs, no password is set, nothing is stored. The fixture says `hostname planted` and has a password.
h=$(openssl passwd -6 planted-pw) || exit 1
plant() { chown 0 "$SW_CFG" "$cfg" 2>/dev/null; chmod 700 "$SW_CFG"; printf 'passwdhash %s\nhostname planted\n' "$h" > "$cfg"; chmod 600 "$cfg"; }
untrusted() { # <what>: a daemon start that must not have read the planted file
	: > "$RUN/daemon.log"; sw_start
	assert_contains "$1: said so" "not trusted" "$(cat "$RUN/daemon.log")"
	[[ $(sw_cmd hostname) != *planted* ]] || { echo "$1: the planted hostname ran"; exit 1; }
	login planted-pw && { echo "$1: the planted password is a login"; exit 1; }
	assert_contains "$1: no password is set" "No password is set" "$(swcurl "$SW_URL/login.html")"
	assert_eq "$1: the file was not touched" "passwdhash $h"$'\nhostname planted' "$(cat "$cfg")"
	sw_stop
}
plant; chown 65534 "$SW_CFG"; chmod 755 "$SW_CFG"
untrusted "directory owned by another user"
assert_eq "...its mode is untouched" 755 "$(stat -c %a "$SW_CFG")"
plant; chmod 775 "$SW_CFG"
untrusted "directory writable by its group"
plant; chmod 757 "$SW_CFG"
untrusted "directory writable by others"
plant; chown 65534 "$cfg"
untrusted "file owned by another user"
plant; chmod 660 "$cfg"
untrusted "file writable by its group"
plant; chmod 602 "$cfg"
untrusted "file writable by others"
plant; sw_start
assert_eq "control: the same file, ours and private, is read: hostname" planted "$(sw_cmd hostname | tr -d '\r' | grep -o planted)"
login planted-pw || { echo "control: the planted password is no login though the file is trusted"; exit 1; }
sw_stop

# ---- a file of exactly CONFIG_LEN bytes with no NUL runs as it is (like the 8051); more is cut ----
{ for i in $(seq 291); do echo "hostname seen"; done; echo "hostname abcdefghijkl"; } > "$cfg"	# 4074 + 22 bytes
assert_eq "test file size" 4096 "$(stat -c %s "$cfg")"
sw_start
assert_eq "a 4096-byte file without NUL ran" abcdefghijkl "$(sw_cmd hostname)"
sw_cmd "passwd p4096" >/dev/null; curpw=p4096	# the file has no password
newest
assert_eq "GET /config returns all 4096 bytes" 4096 "$(get_cfg | wc -c)"
sw_stop
echo "hostname beyond" >> "$cfg"
sw_start
assert_eq "bytes past CONFIG_LEN are not part of the configuration" abcdefghijkl "$(sw_cmd hostname)"

# ---- a file of CONFIG_LEN bytes whose last line has no ending, then the first password: the daemon must not write
# the line separator past its buffer (ASan and UBSan runs of this test catch that), and says why it did not store the hash ----
sw_stop
{ for i in $(seq 291); do echo "hostname seen"; done; printf 'hostname abcdefghijklm'; } > "$cfg"
assert_eq "test file size" 4096 "$(stat -c %s "$cfg")"
assert_eq "...and no line ending at its end" "m" "$(tail -c 1 "$cfg")"
: > "$RUN/daemon.log"; sw_start
assert_contains "the first password cannot be stored in a full file" "could not be saved" "$(sw_cmd 'passwd p4096b')"
assert_contains "...the daemon says the configuration is full" "the startup configuration is full" "$(cat "$RUN/daemon.log")"
assert_eq "...the file is as it was" 4096 "$(stat -c %s "$cfg")"
assert_contains "...and the daemon lives" "Unknown command" "$(sw_cmd foo)"
sw_stop
# one byte less of the file's text (the separator and the line still do not fit): same
{ for i in $(seq 291); do echo "hostname seen"; done; printf 'hostname abcdefghijk'; } > "$cfg"
sw_start
assert_contains "a file one byte short of CONFIG_LEN: still full" "could not be saved" "$(sw_cmd 'passwd p4095')"
sw_stop
echo "hostname a" > "$cfg"; printf 'hostname b' >> "$cfg"
sw_start
assert_eq "a short file with no line ending: the password is stored" "" "$(sw_cmd 'passwd shortpw')"
assert_eq "...on a line of its own after the last line, which got its ending" $'hostname a\nhostname b' "$(head -n 2 "$cfg")"
assert_eq "...that line is the hash, and it is the last" 3 "$(grep -c '' "$cfg")"
assert_eq "...(a hash)" 1 "$(tail -n 1 "$cfg" | grep -c '^passwdhash \$6\$')"
sw_stop

# ---- passwdhash with a rounds= field is refused: crypt would run for minutes in the poll loop ----
rm -rf "$SW_CFG"; sw_start
pad=$(head -c 86 /dev/zero | tr '\0' A)
assert_contains "passwdhash with rounds=" "not a SHA-512 crypt hash" "$(sw_cmd "passwdhash \$6\$rounds=99999\$salt\$$pad")"
assert_contains "a hash with an = elsewhere is refused too" "not a SHA-512 crypt hash" "$(sw_cmd "passwdhash \$6\$sa=lt\$$pad")"
sw_stop

# ---- the directory cannot be synced after the new file is in place: the save still succeeds ----
rm -rf "$SW_CFG"
RTLP_TEST_FAIL_SYNC_DIR=1 sw_start
assert_eq "the first password is stored although the directory cannot be synced" "" "$(sw_cmd 'passwd sync-pw')"
curpw=sync-pw; newest
printf 'hostname synced\n' > "$RUN/up.txt"
assert_eq "POST /config answers 200" 200 "$(post_cfg "$RUN/up.txt")"
assert_eq "...and the new text is what GET /config returns" "hostname synced" "$(get_cfg | head -n 1)"
assert_contains "...the failed sync is logged" "could not be synced" "$(cat "$RUN/daemon.log")"
sw_stop
sw_start
assert_eq "after a restart the saved file runs" synced "$(sw_cmd hostname | tr -d '\r' | grep -o synced)"
sw_stop
rm -rf "$SW_CFG"

# ---- the default and the UI's Save: a Save of any other change keeps the default's lines ----
# The UI's Save reads GET /config, merges the history on top (app.js mergeConf: the lines it read stay, a
# `hostname` replaces a hostname line) and POSTs the result.
sw_stop; rm -rf "$SW_CFG"; sw_start
sw_cmd "passwd save-pw" >/dev/null; curpw=save-pw; newest
assert_eq "fresh daemon: the effective configuration is the default and the hash" "$dflt"$'\n'"$(cat "$cfg")" "$(get_cfg)"
sw_cmd "hostname bench2" >/dev/null
assert_eq "the history holds the change only" "hostname bench2" "$(swcurl -b "$jar" "$SW_URL/cmd_log")"
{ get_cfg; swcurl -b "$jar" "$SW_URL/cmd_log"; } > "$RUN/save.txt"
assert_eq "UI-style Save" 200 "$(post_cfg "$RUN/save.txt")"
assert_eq "the file has the default lines, the hash and the change" "$dflt"$'\n'"$(grep '^passwdhash ' "$cfg")"$'\nhostname bench2' "$(cat "$cfg")"
assert_eq "read-back equals what was sent" "$(cat "$RUN/save.txt")" "$(get_cfg)"
restart; newest
assert_eq "after the restart: the saved file runs, DHCP is on" 1 "$(udhcpc_up)"
assert_eq "hostname" bench2 "$(sw_cmd hostname)"
assert_eq "management VLAN 1" '"mgmt":1' "$(sw_get vlanlist | grep -o '"mgmt":[0-9]*')"
for n in 1 2 3 4 5 6; do
	assert_eq "lan$n: only VLAN 1, as PVID, untagged" "lan$n 1 PVID Egress Untagged" "$(ip netns exec rtlp-sw bridge vlan show dev lan$n | tail -n +2 | tr -s ' ')"
done
