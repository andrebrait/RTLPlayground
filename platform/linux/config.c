/*
 * The startup configuration, a text file <dir>/startup-config (mode 0600, directory 0700).
 *
 * The 8051 keeps it in a NUL-terminated flash area of CONFIG_LEN bytes that the web UI replaces
 * by POSTing the whole text; here the area is `view`, a RAM image with the same shape (text, then
 * zeros up to CONFIG_LEN) that config_read() serves to execute_config() and to GET /config.
 *
 * Saving is atomic: write .new and fsync it, rename the old file to .bak, rename .new into
 * place, fsync the directory. A crash leaves either the old file or, between the two renames,
 * .new next to .bak; config_open() completes the second case and drops a stale .new otherwise.
 *
 * Passwords: only a crypt SHA-512 hash reaches the disk. Per `passwd` line of an upload:
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

/* Copies in[0, len) to out line by line, applying the password rules above, and sets *outlen. A
 * plaintext password that became the one to use is hashed into newhash (else ""). false if a
 * hash could not be made or the result needs CONFIG_LEN bytes or more (the text and its NUL
 * must fit the area). */
static bool rewrite(const char *in, size_t len, char *out, size_t *outlen, char newhash[AUTH_HASH_MAX])
{
	size_t o = 0, p = 0;

	newhash[0] = 0;
	while (p < len) {
		const char *line = in + p, *nl = memchr(line, '\n', len - p);
		size_t end = nl ? (size_t)(nl - line) : len - p;		/* the line, without its '\n' */
		size_t eol = end;				/* ... and without a '\r' before it */
		size_t s = 0, vs;
		char repl[AUTH_HASH_MAX + 12];
		const char *keep = line;
		size_t keeplen = end + !!nl, prefix = 0;

		if (eol && line[eol - 1] == '\r')
			eol--;
		while (s < eol && line[s] == ' ')
			s++;
		vs = s + 6;
		if (eol - s >= 6 && !memcmp(line + s, "passwd", 6) && (vs == eol || line[vs] == ' ')) {
			size_t vlen;
			bool stars = true;

			while (vs < eol && line[vs] == ' ')
				vs++;
			vlen = eol - vs;
			for (size_t i = 0; i < vlen; i++)
				stars &= line[vs + i] == '*';
			if (vlen) {
				const char *h = newhash[0] ? newhash : auth_hash();

				if (stars && !h[0]) {
					keeplen = 0;	/* nothing to keep it as: drop the line */
				} else {
					if (!stars) {
						char pw[PW_MAX + 1] = { 0 };

						memcpy(pw, line + vs, vlen < PW_MAX ? vlen : PW_MAX);
						h = newhash;
						if (!auth_hash_password(pw, newhash)) {
							explicit_bzero(pw, sizeof pw);
							return false;
						}
						explicit_bzero(pw, sizeof pw);
					}
					prefix = snprintf(repl, sizeof repl, "passwdhash %s", h);
					keep = line + eol;	/* the line ending stays */
					keeplen -= eol;
				}
			}
		}
		if (o + prefix + keeplen > CONFIG_LEN - 1)
			return false;
		memcpy(out + o, repl, prefix);
		memcpy(out + o + prefix, keep, keeplen);
		o += prefix + keeplen;
		p += end + !!nl;
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
	strcpy(dir, d);
	path(main_, "");
	path(bak, ".bak");
	path(new_, ".new");
	/* Interrupted between the two renames of a save: the .new is complete (it was synced first). */
	if (access(main_, F_OK) && errno == ENOENT && !access(bak, F_OK) && !access(new_, F_OK)) {
		if (rename(new_, main_))
			fprintf(stderr, "rtlplayground: cannot finish the interrupted save: %s\n", strerror(errno));
		else
			fprintf(stderr, "rtlplayground: finished an interrupted configuration save\n");
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
	if (!rewrite(raw, n, out, &n, hash)) {
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
 * Still pending stubs (stubs_2b.c: they say "Not supported on this platform yet"): vlan
 * (vlan_create), pvid (port_pvid_set), `vlan 1 mgmt` (port_l2_static_mgmt), `ip dhcp` (dhcp_start). */
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

	for (const char *p = view; *p; p++)
		if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
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
	if (len >= CONFIG_LEN || !rewrite((const char *)text, len, disk, &dlen, hash)) {
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
	if (rename(new_, main_)) {
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
