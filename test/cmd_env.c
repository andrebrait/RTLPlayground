/*
 * cmd_env.c - the command-line tokenizer and trap unit that rtl837x_stp.c links
 * against in the table tests. They never call stp_parse(), so these are inert;
 * test_golden links the real cmd_parser.c and rtl837x_trap.c instead.
 */
#include <stdint.h>

#include "rtl837x_common.h"

uint8_t cmd_words_len;
uint8_t cmd_words_b[15];
uint8_t atoi_results_u8;
uint8_t cmd_compare(uint8_t start, const uint8_t *cmd) { (void)start; (void)cmd; return 0; }
uint8_t atoi_byte(uint8_t idx) { (void)idx; return 0; }
uint8_t cmd_parse_port_separator(uint8_t idx) { (void)idx; return 0; }
void trap_rma_set(uint16_t rma, uint8_t act) __banked { (void)rma; (void)act; }
