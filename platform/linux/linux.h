#ifndef _LINUX_H_
#define _LINUX_H_

#include <stddef.h>
#include <stdint.h>

/* Netdev name per logical port, NULL for a logical port the board does not have. Defined
 * by boards/<BOARD>.c together with `machine`. */
extern const char *const board_ifnames[];

/* Console output: where print_string()/write_char() write. Output is buffered; the poll loop
 * flushes before it sleeps, console_set_fd() flushes what is pending to the old fd. */
void console_set_fd(int fd);
void console_flush(void);

/* Console input: the bytes of a terminal or pipe, handed to the line editor the way the
 * serial ISR does on the 8051. Every complete line runs as a command. */
void console_feed(const uint8_t *buf, size_t len);

#endif
