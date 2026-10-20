/*
 * Web login. The password is kept only as a crypt(3) SHA-512 hash ($6$): the shared `passwd`
 * command writes plaintext into passwd[] (as on the 8051), which auth_take_plain() hashes and
 * clears right after the command ran. There is no default password: until a hash exists
 * nobody can log in. One session at a time, as on the 8051.
 */
#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"

extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;

#define SESSION_ID_LEN 32
#define SESSION_TIMEOUT_DEFAULT 200	/* seconds, as httpd.c */

__xdata char passwd[21];
/* Seconds of inactivity after which the session cookie stops validating (`session` command). */
__xdata uint16_t session_timeout = SESSION_TIMEOUT_DEFAULT;

static char hash[AUTH_HASH_MAX];
static char session_id[SESSION_ID_LEN + 1];
static int64_t last_use_ms, login_blocked_until;

/* A failed login refuses every login (right or wrong) for this long, without running crypt:
 * a brute force cannot keep the single-threaded loop busy in crypt() and is capped at one
 * guess per second. */
#define LOGIN_PAUSE_MS 1000

bool auth_has_password(void)
{
	return hash[0] != 0;
}

/* Equal strings, in time that does not depend on where they differ. */
static bool same(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char diff = la != lb;

	for (size_t i = 0; i < la && i < lb; i++)
		diff |= a[i] ^ b[i];
	return !diff;
}

/* The shared parser records every command in the ring cmd_history (entries end with '\n', the
 * unwritten rest is NUL, cmd_history_ptr is the next write position). A command line that starts a
 * `passwd` entry is rewritten as exactly "passwd ****", whatever the password was: neither its
 * characters nor its length stay in the history, and the web UI's Save merges the line back as a
 * password line that keeps the current hash (config.c). drop removes the "passwd ****" entries
 * instead. The entries are compacted towards the newest one; the ring pointer does not move. */
#define HIST_MASKED "passwd ****"

static bool starts_passwd(const char *e, size_t n, size_t *rest)
{
	size_t i = 0;

	while (i < n && e[i] == ' ')
		i++;
	if (n - i < 7 || memcmp(e + i, "passwd ", 7))
		return false;
	for (i += 7; i < n && e[i] == ' '; i++)
		;
	*rest = i;
	return i < n;
}

static void history_rewrite(bool drop)
{
	static char lin[CMD_HISTORY_SIZE], out[CMD_HISTORY_SIZE + 16];
	size_t o = 0, skip;
	unsigned i = 0;

	for (unsigned k = 0; k < CMD_HISTORY_SIZE; k++)
		lin[k] = cmd_history[(cmd_history_ptr + k) & CMD_HISTORY_MASK];
	/* A ring that has wrapped starts inside an entry the parser overwrote the head of: the readers
	 * (page_impl.c send_cmd_log, `history`) skip to the first '\n', so does this. Without it the
	 * compaction below would put a NUL in front of that remnant and make it look like a whole entry. */
	if (!memchr(lin, 0, sizeof lin)) {
		while (i < CMD_HISTORY_SIZE && lin[i] != '\n')
			i++;
		i += i < CMD_HISTORY_SIZE;
	}
	while (i < CMD_HISTORY_SIZE) {
		unsigned start = i, end;
		char prev = i ? lin[i - 1] : lin[CMD_HISTORY_SIZE - 1];	/* the oldest byte follows the newest */
		bool term, begins;
		size_t rest;

		if (!lin[i]) {
			i++;
			continue;
		}
		for (end = i; end < CMD_HISTORY_SIZE && lin[end] && lin[end] != '\n'; end++)
			;
		term = end < CMD_HISTORY_SIZE;	/* a '\n' or NUL follows */
		i = end + term;
		begins = (!prev || prev == '\n') && starts_passwd(lin + start, end - start, &rest);
		if (begins && drop && end - start == sizeof HIST_MASKED - 1 && !memcmp(lin + start, HIST_MASKED, end - start))
			continue;
		if (begins && !drop) {
			memcpy(out + o, HIST_MASKED, sizeof HIST_MASKED - 1);
			o += sizeof HIST_MASKED - 1;
			if (term)
				out[o++] = '\n';
			continue;
		}
		memcpy(out + o, lin + start, end - start + term);
		o += end - start + term;
	}
	skip = o > CMD_HISTORY_SIZE ? o - CMD_HISTORY_SIZE : 0;	/* the oldest bytes fall off the ring */
	for (unsigned k = 0; k < CMD_HISTORY_SIZE; k++)
		cmd_history[k] = 0;
	for (size_t k = skip; k < o; k++)
		cmd_history[(cmd_history_ptr - (o - skip) + (k - skip)) & CMD_HISTORY_MASK] = out[k];
	explicit_bzero(lin, sizeof lin);
	explicit_bzero(out, sizeof out);
}

bool auth_hash_password(const char *pw, char out[AUTH_HASH_MAX])
{
	static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	char salt[3 + 16 + 1] = "$6$";
	unsigned char rnd[16];
	const char *h;

	if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd) {
		fprintf(stderr, "rtlplayground: no random bytes for a salt: password not changed\n");
		return false;
	}
	for (int i = 0; i < 16; i++)
		salt[3 + i] = alphabet[rnd[i] & 63];
	h = crypt(pw, salt);
	if (!h || h[0] == '*' || strlen(h) >= AUTH_HASH_MAX) {
		fprintf(stderr, "rtlplayground: crypt failed: password not changed\n");
		return false;
	}
	strcpy(out, h);
	return true;
}

/* Changing the password logs out whoever is logged in: the session is dropped here and by
 * `passwdhash` (safer than keeping a session that was granted for the old password). */
bool auth_take_plain(void)
{
	bool first = !hash[0];

	if (!passwd[0])
		return false;
	history_rewrite(false);
	memset(cmd_buffer, 0, CMD_BUF_SIZE);
	if (auth_hash_password(passwd, hash)) {
		session_id[0] = 0;	/* whoever was logged in with the old password is out */
		/* The first password is what makes the switch reachable at all: it is stored at once, so a
		 * restart before anyone saves cannot leave a switch with no login. Later changes follow the
		 * 8051 model (they are saved with the rest of the configuration). */
		if (first) {
			if (config_persist_password(hash))
				history_rewrite(true);	/* nothing unsaved is left in the history */
			else
				cmd_error("The password is set, but it could not be saved: it is lost at the next restart\n");
		}
	}
	memset(passwd, 0, sizeof passwd);
	return true;
}

const char *auth_hash(void)
{
	return hash;
}

/* Unlike auth_take_plain this keeps the session: the caller is the logged-in user saving a
 * configuration (POST /config), who must still be able to read it back. */
void auth_set_hash(const char *h)
{
	strcpy(hash, h);
}

bool auth_check_password(const char *pw)
{
	const char *h;
	bool ok;
	int64_t now = now_ms();

	if (!hash[0] || now < login_blocked_until)
		return false;
	h = crypt(pw, hash);
	ok = h && h[0] != '*' && same(h, hash);
	if (!ok)
		login_blocked_until = now_ms() + LOGIN_PAUSE_MS;
	return ok;
}

bool auth_login(char id[33])
{
	static const char digits[] = "0123456789abcdef";
	unsigned char rnd[SESSION_ID_LEN / 2];

	if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd)
		return false;
	for (int i = 0; i < SESSION_ID_LEN / 2; i++) {
		session_id[2 * i] = digits[rnd[i] >> 4];
		session_id[2 * i + 1] = digits[rnd[i] & 15];
	}
	session_id[SESSION_ID_LEN] = 0;
	last_use_ms = now_ms();
	strcpy(id, session_id);
	return true;
}

bool auth_session_valid(const char *id)
{
	int64_t now = now_ms();

	if (!session_id[0] || !same(id, session_id))
		return false;
	if (now - last_use_ms > (int64_t)session_timeout * 1000)
		return false;
	last_use_ms = now;
	return true;
}

bool auth_hash_valid(const char *arg)
{
	size_t n = strlen(arg);
	/* $6$[rounds=N$]salt$ followed by the 86 characters of the digest */
	const char *last = strrchr(arg, '$');

	return !strncmp(arg, "$6$", 3) && n < AUTH_HASH_MAX && last != arg + 2 && n - (last + 1 - arg) == 86 &&
	       strspn(arg + 3, "$./=0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == n - 3;
}

/* passwdhash <$6$...>: sets the hash directly, how a saved configuration restores the password
 * without plaintext. The only platform command so far. */
bool platform_cmd(void) __banked
{
	const char *arg;

	if (!cmd_compare(0, "passwdhash"))
		return false;
	if (cmd_words_len != 2) {
		cmd_error("passwdhash <$6$...>\n");
		return true;
	}
	arg = (const char *)cmd_buffer + cmd_words_b[1];
	if (!auth_hash_valid(arg)) {
		cmd_error("not a SHA-512 crypt hash\n");
		return true;
	}
	strcpy(hash, arg);
	session_id[0] = 0;
	return true;
}
