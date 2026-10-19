#ifndef _CMD_PARSER_H_
#define _CMD_PARSER_H_

#include <stdint.h>

#include "platform.h"

extern __xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
extern __bit __at(0x03) cmd_available;
extern __xdata uint8_t err_status;

/* Tokenized command state and helpers shared with platform command handlers. */
#define N_WORDS 15
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[N_WORDS];
extern __xdata uint8_t hexvalue[4];
extern __xdata uint8_t atoi_results_u8;
uint8_t cmd_compare(uint8_t start, __code const uint8_t *cmd);
uint8_t atoi_hex(uint8_t idx);
uint8_t atoi_byte(uint8_t idx);
void cmd_error(__code const char *msg);

void cmd_tokenize(void) __banked;
void cmd_parser(void) __banked;
void execute_config(void) __banked;
void execute_commands(__xdata uint8_t *p) __banked;
void print_ip(__xdata uint8_t *ptr) __banked;
void print_mac(__xdata uint8_t *ptr) __banked;
void print_sw_version(void) __banked;
void clear_command_history(void) __banked;

#endif
