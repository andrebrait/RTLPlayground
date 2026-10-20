#ifndef _LINUX_H_
#define _LINUX_H_

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Netdev name per logical port, NULL for a logical port the board does not have. Defined
 * by boards/<BOARD>.c together with `machine`. */
extern const char *const board_ifnames[];
#define BOARD_PORTS 9		/* the entries of board_ifnames and of the struct machine arrays */
#define SWBR "swbr"		/* the bridge, which is also the management interface */

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
#define HTTP_LISTENERS 3	/* swbr, 127.0.0.1, ::1 */
#define HTTP_CONNS 8
#define HTTP_SLOTS (HTTP_LISTENERS + HTTP_CONNS)
#define CLI_SLOTS 5		/* the listener and four connections */
/* Call before the network is touched: unless any, the listener is bound to swbr (plus loopback), see
 * http.c. http_retry binds what had to wait for swbr or its address (call it after bridge_init). */
int http_open(const char *addr_port, const char *www_dir, bool any);
void http_retry(int64_t now);
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
 * needed, takes an exclusive lock on it for the life of the process (-1: another daemon holds it;
 * the other failures leave the daemon without a store and are logged, 0), finishes or discards an
 * interrupted save, and loads the file unless the directory or the file is not trusted (not owned
 * by the daemon's uid, or writable by group or others); config_startup() then runs it
 * (execute_config), or the built-in default when there is nothing to run. */
int config_open(const char *dir);
void config_startup(void);
/* The configuration upload of POST /config: the new text (not NUL-terminated). Every password
 * line is turned into a `passwdhash` line before anything reaches the disk; false = it could not
 * be stored (nothing changed). */
bool config_save(const uint8_t *text, size_t len);
/* The first password of a switch: appends `passwdhash <hash>` to the stored configuration (the file
 * as it is now, or an empty one) and saves it the way config_save does. false = not stored; the
 * reason went to stderr. */
bool config_persist_password(const char *hash);

/* The default STP configuration and port map of a booting 8051 (state.c); call after mgmt_init. */
void stp_state_init(void);

/* Ports (ports.c): takes the power-on chip temperature. Call once, before the startup
 * configuration runs. */
void ports_init(void);
/* The boot state of every board port, which the startup configuration then changes: administratively
 * up, MTU 1500, every supported link mode advertised. Call after bridge_init, before the startup
 * configuration: what a port was set to and not saved is lost at a restart, as on the 8051. */
void ports_reset(void);

/* Bridge (bridge.c): swbr holds every board port; its VLAN entries are the switch's VLAN table.
 * bridge_init creates or adopts swbr, puts the board ports in it and sets the VLAN state of a
 * booting 8051; call it before the startup configuration runs. bridge_mgmt_vlan makes the
 * bridge's own entry for VLAN vid (if that VLAN exists) the management one (active) or an
 * ordinary tagged one: 0 or -errno. */
void bridge_init(void);
/* After the startup configuration: with an adopted bridge, removes the VLANs the configuration did
 * not create and gives VLAN 1 the state of a booting 8051 unless the configuration decided it. */
void bridge_commit(void);
int bridge_mgmt_vlan(uint16_t vid, bool active);
bool bridge_vid_reserved(uint16_t vid);	/* a parking VLAN of a board port: no user VLAN, no L2 entry */
int bridge_mac_get(uint8_t mac[6]);
int bridge_mac_set(const uint8_t mac[6]);

/* Management IP and DHCP (mgmt.c). mgmt_init: after bridge_init; dhcp_script is the udhcpc script
 * (NULL: the built-in path). mgmt_apply: call after every run of commands, it applies a changed
 * ip/netmask/gw to swbr and reports a failure like a command would. mgmt_service: every poll-loop
 * wake-up. mgmt_exit: before the daemon exits or re-executes (stops the DHCP client). */
void mgmt_init(const char *dhcp_script);
void mgmt_apply(void);
void mgmt_service(int64_t now);
void mgmt_exit(void);

#endif
