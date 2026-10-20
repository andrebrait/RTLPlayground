/*
 * What the shared code links against that milestone 2a still has to implement. Every entry
 * reports its feature as absent: getters return the off or empty state, commands say so.
 * The task that implements an entry deletes it from here.
 */
#include <string.h>

#include "cmd_parser.h"
#include "platform.h"
#include "rtl837x_flash.h"

#define UNSUPPORTED() cmd_error("Not supported on this platform yet\n")

/* L2 table: empty. */
bool l2_entry_next(void) __banked { return false; }
uint8_t l2_entry_delete(uint16_t idx) __banked { return 0; }
uint8_t port_l2_forget(void) __banked { UNSUPPORTED(); return 0; }
void port_l2_learned(void) __banked { UNSUPPORTED(); }
