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

/* Capture: from console_capture_begin() until console_capture_end(), everything the shared code
 * prints is collected (newline stays "\n") instead of going to the console. End returns the
 * collected bytes (valid until the next begin); truncated is set when CAPTURE_MAX was hit. */
#define CAPTURE_MAX 65536
void console_capture_begin(void);
const char *console_capture_end(size_t *len, bool *truncated);

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
void auth_take_plain(void);
bool auth_has_password(void);
bool auth_check_password(const char *pw);
/* A new session replaces the old one (as on the 8051); id receives its 32 hex digits. */
bool auth_login(char id[33]);
/* True if the cookie value is the current session and it has not timed out; refreshes it. */
bool auth_session_valid(const char *id);

/* The configuration upload of POST /config: the new startup-config text (not NUL-terminated).
 * false = it could not be stored. */
bool config_save(const uint8_t *text, size_t len);

#endif
