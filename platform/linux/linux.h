#ifndef _LINUX_H_
#define _LINUX_H_

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Netdev name per logical port, NULL for a logical port the board does not have. Defined
 * by boards/<BOARD>.c together with `machine`. */
extern const char *const board_ifnames[];

/* Console output: where print_string()/write_char() write. Output is buffered; the poll loop
 * flushes before it sleeps, console_set_fd() flushes what is pending to the old fd. */
void console_set_fd(int fd);
void console_flush(void);

/* Runs NUL-terminated commands (one per line) for a web or CLI-socket client and returns what
 * they printed (newline stays "\n"; never NULL, valid until the next call); truncated is set
 * when CAPTURE_MAX was hit. The console user's half-typed line is kept, a `passwd <pw>` is hashed
 * (auth_take_plain) and wiped from `cmds`. The result of the last command is in err_status. */
#define CAPTURE_MAX 65536
const char *console_run(uint8_t *cmds, size_t *len, bool *truncated);

/* Console input: the bytes of a terminal or pipe, handed to the line editor the way the
 * serial ISR does on the 8051. Every complete line runs as a command. */
void console_feed(const uint8_t *buf, size_t len);

/* Milliseconds on the monotonic clock. */
int64_t now_ms(void);

/* The sockets the poll loop in main.c serves. Each has a fixed number of pollfd slots: fill()
 * sets them (a slot with nothing to wait for has fd -1) and lowers *wait_ms to the nearest
 * connection deadline; service() runs after poll() on the same slots. */
#define HTTP_SLOTS 9
#define CLI_SLOTS 5
int http_open(const char *addr_port, const char *www_dir);
void http_fill(struct pollfd *fds, int64_t now, int *wait_ms);
void http_service(struct pollfd *fds, int64_t now);
bool http_restart_requested(void);	/* GET /reset has been answered: re-exec the daemon */
int cli_open(const char *path);
void cli_close(void);
void cli_fill(struct pollfd *fds, int64_t now, int *wait_ms);
void cli_service(struct pollfd *fds, int64_t now);

/* Web login (auth.c). The password exists only as a crypt(3) SHA-512 hash; the shared `passwd`
 * command leaves plaintext in passwd[], which auth_take_plain() hashes and clears: call it after
 * every run of the command parser. */
bool auth_take_plain(void);	/* true if there was a plaintext password to take */
bool auth_has_password(void);
#define AUTH_HASH_MAX 160
/* crypt(3) SHA-512 hash of pw with a fresh random salt into out; false (out untouched) on failure. */
bool auth_hash_password(const char *pw, char out[AUTH_HASH_MAX]);
bool auth_hash_valid(const char *h);	/* h is a well-formed $6$ hash that fits */
const char *auth_hash(void);	/* the current hash, "" while no password is set */
void auth_set_hash(const char *h);	/* h is below AUTH_HASH_MAX; the session survives */
bool auth_check_password(const char *pw);
/* A new session replaces the old one (as on the 8051); id receives its 32 hex digits. */
bool auth_login(char id[33]);
/* True if the cookie value is the current session and it has not timed out; refreshes it. */
bool auth_session_valid(const char *id);

/* The startup configuration (config.c): <dir>/startup-config, held in RAM as the text the web UI
 * reads back (config_read() of platform.h). config_open() creates the directory (mode 0700) if
 * needed, finishes or discards an interrupted save, and loads the file; config_startup() then
 * runs it (execute_config), or the built-in default when there is nothing to run. */
void config_open(const char *dir);
void config_startup(void);
/* The configuration upload of POST /config: the new text (not NUL-terminated). Every password
 * line is turned into a `passwdhash` line before anything reaches the disk; false = it could not
 * be stored (nothing changed). */
bool config_save(const uint8_t *text, size_t len);

/* Ports (ports.c): opens the broadcast/multicast frame counters of every port of the board and takes
 * the power-on chip temperature. Call once, before the startup configuration runs. */
void ports_init(void);

#endif
