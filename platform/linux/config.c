/*
 * The startup configuration, a text file <dir>/startup-config (mode 0600, directory 0700).
 *
 * The 8051 keeps it in a NUL-terminated flash area of CONFIG_LEN bytes that the web UI replaces
 * by POSTing the whole text; here the area is `view`, a RAM image with the same shape (text, then
 * zeros up to CONFIG_LEN) that config_read() serves to execute_config() and to GET /config.
 *
 * Saving is atomic: write .new and fsync it, rename the old file to .bak, rename .new into
 * place, fsync the directory. A crash leaves either the old file or, between the two renames,
 * no startup-config with .bak (the complete old file) and .new (complete only if it was synced:
 * nothing says so). Startup never promotes .new: with that signature it restores .bak, and it
 * always deletes a leftover .new. The save that was interrupted is lost, the old file survives.
 *
 * A file of CONFIG_LEN bytes with no NUL is run as it is (the 8051 does the same; a last line
 * with no line ending is not executed there either, nor here). One longer than that is cut at
 * CONFIG_LEN, the size of the flash area. An upload must leave room for its NUL.
 *
 * Line ends: a line ends at its first CR or LF, as in execute_config().
 *
 * Passwords: only a crypt SHA-512 hash reaches the disk, and only one password line: of all the
 * lines of an upload that are passwd or a valid passwdhash only the last is written (the web UI's
 * Save adds a passwdhash line per password change, it does not know that line). Per such line:
 *   passwd <value>      -> passwdhash <hash of value>, and that becomes the login password
 *   passwd ****...      -> passwdhash <current hash>, or no line while no password is set. All
 *                          asterisks is what the history scrub leaves of a password (auth.c), which
 *                          the web UI's Save merges back in; the asterisks are never hashed.
 *                          shortcut: a real password made only of asterisks cannot be set through
 *                          an upload this way (set it on the console); the ambiguity is inherent.
 *   passwdhash <hash>   -> kept as is
 * The web UI reads a saved configuration back and insists it equals what it sent, so `view`
 * keeps the uploaded text itself, plaintext included, until the next upload or restart
 * (zeroed on replacement). It is never written anywhere. After a restart it shows the hashes.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmd_parser.h"
#include "platform.h"
#include "rtl837x_flash.h"
#include "linux.h"

extern __xdata char passwd[21];
extern __xdata char save_cmd;

#define NAME "startup-config"
#define PW_MAX 20	/* what the shared `passwd` command keeps (parse_passwd) */

#define DIR_MAX 1024
#define FILE_MAX (DIR_MAX + 32)	/* room for the name and its suffix */

static char dir[DIR_MAX];	/* empty: no usable directory, nothing can be stored */
static char view[CONFIG_LEN];

uint16_t config_read(uint16_t offset, __xdata uint8_t * __xdata buf, __xdata uint16_t len) __banked
{
	if (offset >= CONFIG_LEN)
		return 0;
	if (len > CONFIG_LEN - offset)
		len = CONFIG_LEN - offset;
	memcpy(buf, view + offset, len);
	return len;
}

static void path(char *buf, const char *suffix)
{
	snprintf(buf, FILE_MAX, "%s/" NAME "%s", dir, suffix);
}

enum kind { OTHER, DROP, STARS, PLAIN, HASH };

struct line {
	size_t len;		/* up to the first CR or LF, as execute_config() ends a command */
	enum kind kind;
	const char *val;	/* PLAIN: the password; HASH: the hash (both to the end of the line) */
	size_t vlen;
};

static bool word_at(const char *l, size_t len, size_t s, const char *w, size_t wl, size_t *vs)
{
	if (len - s < wl || memcmp(l + s, w, wl) || (len - s > wl && l[s + wl] != ' '))
		return false;
	*vs = s + wl;
	while (*vs < len && l[*vs] == ' ')
		(*vs)++;
	return true;
}

static void classify(const char *p, size_t max, struct line *ln)
{
	size_t s = 0, vs;

	for (ln->len = 0; ln->len < max && p[ln->len] != '\r' && p[ln->len] != '\n'; ln->len++)
		;
	ln->kind = OTHER;
	while (s < ln->len && p[s] == ' ')
		s++;
	if (word_at(p, ln->len, s, "passwd", 6, &vs) && vs < ln->len) {
		bool stars = true;

		for (size_t i = vs; i < ln->len; i++)
			stars &= p[i] == '*';
		ln->kind = !stars ? PLAIN : auth_hash()[0] ? STARS : DROP;
		ln->val = p + vs;
		ln->vlen = ln->len - vs;
	} else if (word_at(p, ln->len, s, "passwdhash", 10, &vs) && vs < ln->len && ln->len - vs < AUTH_HASH_MAX) {
		char h[AUTH_HASH_MAX];

		memcpy(h, p + vs, ln->len - vs);
		h[ln->len - vs] = 0;
		if (auth_hash_valid(h)) {
			ln->kind = HASH;
			ln->val = p + vs;
			ln->vlen = ln->len - vs;
		}
	}
}

/* Copies in[0, len) to out, applying the password rules above, and sets *outlen. A line ends at
 * its first CR or LF, exactly as in execute_config() ("\r\r\n" is a line, then two
 * empty ones); the ending bytes are copied unchanged. Only the LAST password line (passwd, or a
 * well-formed passwdhash) is kept: it is the one that counts when the file runs, and the web UI's
 * Save adds one every time the password changed (it does not know passwdhash). newhash gets the
 * hash the live login becomes (else ""). false if a hash could not be made or the result needs
 * more than `limit` bytes. */
static bool rewrite(const char *in, size_t len, size_t limit, char *out, size_t *outlen, char newhash[AUTH_HASH_MAX])
{
	size_t o = 0, p = 0, last = (size_t)-1;
	struct line ln;

	newhash[0] = 0;
	for (p = 0; p < len; p += ln.len + (p + ln.len < len)) {
		classify(in + p, len - p, &ln);
		if (ln.kind >= STARS)
			last = p;
	}
	for (p = 0; p < len; p += ln.len + (p + ln.len < len)) {
		size_t ending;
		char repl[AUTH_HASH_MAX + 12];
		const char *w = in + p;
		size_t wl, el;

		classify(in + p, len - p, &ln);
		ending = p + ln.len < len;
		wl = ln.len;
		el = ending;
		if (ln.kind == DROP || (ln.kind >= STARS && p != last)) {
			wl = 0;
			el = 0;		/* the line and its ending go */
		} else if (p == last && ln.kind != HASH) {
			char pw[PW_MAX + 1] = { 0 };
			const char *h = auth_hash();

			if (ln.kind == PLAIN) {
				memcpy(pw, ln.val, ln.vlen < PW_MAX ? ln.vlen : PW_MAX);
				h = newhash;
				if (!auth_hash_password(pw, newhash)) {
					explicit_bzero(pw, sizeof pw);
					return false;
				}
				explicit_bzero(pw, sizeof pw);
			}
			wl = snprintf(repl, sizeof repl, "passwdhash %s", h);
			w = repl;
		} else if (p == last) {
			memcpy(newhash, ln.val, ln.vlen);
			newhash[ln.vlen] = 0;
		}
		if (o + wl + el > limit)
			return false;
		memcpy(out + o, w, wl);
		memcpy(out + o + wl, in + p + ln.len, el);
		o += wl + el;
	}
	*outlen = o;
	return true;
}

static void set_view(const char *text, size_t len)
{
	explicit_bzero(view, sizeof view);
	memcpy(view, text, len);
}

void config_open(const char *d)
{
	char main_[FILE_MAX], bak[FILE_MAX], new_[FILE_MAX], raw[CONFIG_LEN], out[CONFIG_LEN], hash[AUTH_HASH_MAX];
	size_t got = 0, n;
	struct stat st;
	ssize_t r = 0;
	int fd;

	if (strlen(d) >= DIR_MAX) {
		fprintf(stderr, "rtlplayground: --cfg-dir is too long; no configuration store\n");
		return;
	}
	if (mkdir(d, 0700) && errno != EEXIST) {
		fprintf(stderr, "rtlplayground: cannot create %s: %s; no configuration store\n", d, strerror(errno));
		return;
	}
	if (stat(d, &st) || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "rtlplayground: %s is not a directory; no configuration store\n", d);
		return;
	}
	if (st.st_uid != geteuid())
		fprintf(stderr, "rtlplayground: %s is owned by uid %u, not by us: its mode %04o is not changed\n",
			d, (unsigned)st.st_uid, (unsigned)(st.st_mode & 07777));
	else if ((st.st_mode & 07777) != 0700 && chmod(d, 0700))
		fprintf(stderr, "rtlplayground: cannot set the mode of %s to 0700: %s\n", d, strerror(errno));
	strcpy(dir, d);
	path(main_, "");
	path(bak, ".bak");
	path(new_, ".new");
	/* Interrupted between the two renames of a save: restore the old file; .new may be cut short. */
	if (access(main_, F_OK) && errno == ENOENT && !access(bak, F_OK) && !access(new_, F_OK)) {
		if (rename(bak, main_))
			fprintf(stderr, "rtlplayground: cannot restore %s: %s\n", main_, strerror(errno));
		else
			fprintf(stderr, "rtlplayground: an interrupted configuration save was dropped, the old file is back\n");
	}
	if (unlink(new_) && errno != ENOENT)
		fprintf(stderr, "rtlplayground: cannot remove %s: %s\n", new_, strerror(errno));

	fd = open(main_, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT)
			fprintf(stderr, "rtlplayground: cannot read %s: %s\n", main_, strerror(errno));
		return;
	}
	while (got < sizeof raw && (r = read(fd, raw + got, sizeof raw - got)) > 0)
		got += r;
	close(fd);
	if (r < 0) {
		fprintf(stderr, "rtlplayground: cannot read %s: %s\n", main_, strerror(errno));
		return;
	}
	n = strnlen(raw, got);	/* the text ends at its first NUL, as on the 8051 */
	if (!rewrite(raw, n, CONFIG_LEN, out, &n, hash)) {
		fprintf(stderr, "rtlplayground: %s does not fit %d bytes: ignored\n", main_, CONFIG_LEN);
		return;
	}
	set_view(out, n);
	explicit_bzero(raw, sizeof raw);
	explicit_bzero(out, sizeof out);
	/* a plaintext `passwd` in a hand-edited file is a hash in view, the login of the daemon */
	if (hash[0])
		auth_set_hash(hash);
}

/* Applies the factory state with the same commands a user would type, so the same code paths run.
 * Still pending stubs (they say "Not supported on this platform yet": nine lines at every boot
 * without a configuration): `vlan 1 ...` (vlan_create, 1), one `pvid` per port (port_pvid_set, 6),
 * `vlan 1 mgmt` (port_l2_static_mgmt, 1), `ip dhcp` (dhcp_start, 1). */
static void apply_default(void)
{
	char cmds[256];
	size_t n = snprintf(cmds, sizeof cmds, "vlan 1");

	for (uint8_t i = machine.min_port; i <= machine.max_port; i++)
		n += snprintf(cmds + n, sizeof cmds - n, " %d", machine.log_to_phys_port[i]);
	n += snprintf(cmds + n, sizeof cmds - n, "\n");
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++)
		n += snprintf(cmds + n, sizeof cmds - n, "pvid %d 1\n", machine.log_to_phys_port[i]);
	snprintf(cmds + n, sizeof cmds - n, "vlan 1 mgmt\nip dhcp\n");
	save_cmd = 0;
	execute_commands((uint8_t *)cmds);
	clear_command_history();
	save_cmd = 1;
}

void config_startup(void)
{
	bool empty = true;

	for (size_t i = 0; i < sizeof view && view[i]; i++)	/* view has no NUL when the file filled it */
		if (view[i] != ' ' && view[i] != '\t' && view[i] != '\r' && view[i] != '\n')
			empty = false;
	execute_config();
	/* execute_config seeds the 8051's default password "1234" before running the file; on Linux
	 * that must not become a login. A password in the file arrived as a `passwdhash` line. */
	memset(passwd, 0, sizeof passwd);
	if (empty)
		apply_default();
}

static bool write_new(const char *file, const char *data, size_t len)
{
	size_t done = 0;
	int fd;

	if (unlink(file) && errno != ENOENT)
		return false;
	fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	while (done < len) {
		ssize_t w = write(fd, data + done, len - done);

		if (w <= 0) {
			close(fd);
			return false;
		}
		done += w;
	}
	if (fsync(fd)) {
		close(fd);
		return false;
	}
	return close(fd) == 0;
}

static bool sync_dir(void)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = fsync(fd) == 0;
	close(fd);
	return ok;
}

bool config_save(const uint8_t *text, size_t len)
{
	char main_[FILE_MAX], bak[FILE_MAX], new_[FILE_MAX], hash[AUTH_HASH_MAX];
	static char disk[CONFIG_LEN];
	size_t dlen;
	bool ok;

	if (!dir[0]) {
		fprintf(stderr, "rtlplayground: configuration upload dropped: no configuration store\n");
		return false;
	}
	len = strnlen((const char *)text, len);
	if (len >= CONFIG_LEN || !rewrite((const char *)text, len, CONFIG_LEN - 1, disk, &dlen, hash)) {
		fprintf(stderr, "rtlplayground: configuration upload rejected: too large or no hash\n");
		return false;
	}
	path(main_, "");
	path(bak, ".bak");
	path(new_, ".new");
	if (!write_new(new_, disk, dlen)) {
		fprintf(stderr, "rtlplayground: cannot write %s: %s\n", new_, strerror(errno));
		unlink(new_);
		return false;
	}
	if (getenv("RTLP_TEST_CRASH_AFTER_NEW"))
		_exit(1);	/* test hook: the power fails here */
	if (rename(main_, bak) && errno != ENOENT) {
		fprintf(stderr, "rtlplayground: cannot keep %s: %s\n", bak, strerror(errno));
		unlink(new_);
		return false;
	}
	if (getenv("RTLP_TEST_FAIL_INSTALL") ? (errno = EIO, true) : rename(new_, main_)) {	/* the hook fails the rename */
		fprintf(stderr, "rtlplayground: cannot install %s: %s\n", main_, strerror(errno));
		rename(bak, main_);	/* back to the old file if there was one */
		unlink(new_);
		return false;
	}
	set_view((const char *)text, len);
	if (hash[0])
		auth_set_hash(hash);
	explicit_bzero(disk, sizeof disk);
	ok = sync_dir();
	if (!ok)
		fprintf(stderr, "rtlplayground: cannot sync %s: %s\n", dir, strerror(errno));
	return ok;
}
