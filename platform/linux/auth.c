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

/* Overwrites every `passwd <pw>` entry of the shared command history (the parser records each
 * command, passwords included) with `passwd ****`. An entry starts after a '\n' or NUL. */
static void scrub_history(void)
{
	for (unsigned q = 0; q < CMD_HISTORY_SIZE; q++) {
		unsigned p = q, prev = (q - 1) & CMD_HISTORY_MASK;
		static const char word[] = "passwd";

		if (cmd_history[prev] != 0 && cmd_history[prev] != '\n')
			continue;
		while (cmd_history[p] == ' ')
			p = (p + 1) & CMD_HISTORY_MASK;
		for (size_t i = 0; i < sizeof word - 1; i++)
			if (cmd_history[(p + i) & CMD_HISTORY_MASK] != word[i])
				goto next;
		p = (p + sizeof word - 1) & CMD_HISTORY_MASK;
		if (cmd_history[p] != ' ')
			continue;
		while (cmd_history[p] == ' ')
			p = (p + 1) & CMD_HISTORY_MASK;
		for (unsigned n = 0; n < CMD_HISTORY_SIZE && cmd_history[p] != 0 && cmd_history[p] != '\n'; n++) {
			cmd_history[p] = '*';
			p = (p + 1) & CMD_HISTORY_MASK;
		}
next:		;
	}
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
	if (!passwd[0])
		return false;
	scrub_history();
	memset(cmd_buffer, 0, CMD_BUF_SIZE);
	if (auth_hash_password(passwd, hash))
		session_id[0] = 0;	/* whoever was logged in with the old password is out */
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

/* passwdhash <$6$...>: sets the hash directly, how a saved configuration restores the password
 * without plaintext. The only platform command so far. */
bool platform_cmd(void) __banked
{
	const char *arg, *last;
	size_t n;

	if (!cmd_compare(0, "passwdhash"))
		return false;
	if (cmd_words_len != 2) {
		cmd_error("passwdhash <$6$...>\n");
		return true;
	}
	arg = (const char *)cmd_buffer + cmd_words_b[1];
	n = strlen(arg);
	/* $6$[rounds=N$]salt$ followed by the 86 characters of the digest */
	last = strrchr(arg, '$');
	if (strncmp(arg, "$6$", 3) || n >= AUTH_HASH_MAX || last == arg + 2 || n - (last + 1 - arg) != 86 ||
	    strspn(arg + 3, "$./=0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != n - 3) {
		cmd_error("not a SHA-512 crypt hash\n");
		return true;
	}
	memcpy(hash, arg, n + 1);
	session_id[0] = 0;
	return true;
}
