#!/usr/bin/env python3
"""Run the production bandwidth parser and backend over the register model."""
import os
import pathlib
import shlex
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / 'cmd_parser.c').read_text()
functions = []
for signature in ('uint8_t cmd_compare(', 'uint8_t atoi_hex(', 'uint8_t cmd_parse_port(',
                  'uint8_t cmd_parse_port_separator(', 'static void cmd_error(',
                  'void cmd_tokenize(', 'void parse_bw('):
    start = src.index(signature)
    functions.append(src[start:src.index('\n}', start) + 2])
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_bandwidth.h"
#include "cmd_parser.h"
#include "machine.h"
#include <test/hw_mock.h>
#define N_WORDS 15
uint8_t cmd_buffer[CMD_BUF_SIZE], cmd_words_len, cmd_words_b[N_WORDS], err_status;
uint8_t atoi_results_u8, hexvalue[4];
const struct machine machine = { .isRTL8373 = 1, .phys_to_log_port = {0,1,2,3,4,5,6,7,8} };
void print_string(const char *p) { (void)p; }
void print_byte(uint8_t v) { (void)v; }
void print_reg(uint16_t v) { (void)v; }
void write_char(char v) { (void)v; }
'''
tests = r'''
static void command(const char *text) {
    strcpy((char *)cmd_buffer, text);
    cmd_tokenize();
    if (err_status == ERR_OK) parse_bw();
}
int main(void) {
    const uint16_t in = RTL837X_IGBW_PORT_CTRL + 2 * 4;
    const uint16_t out = RTL837X_EGBW_PORT_CTRL + 2 * 1024;
    const char *invalid[] = {
        "bw status 3 extra", "bw in 3 off extra", "bw out 3 off extra",
        "bw in 3 drop extra", "bw in 3 fc extra", "bw in 3 0100 extra",
        "bw out 3 0200 extra", "bw in 3", "bw status"
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        hw_reset();
        hw_reg_set(in, 0x100100); hw_reg_set(out, 0x100200);
        hw_reg_set(RTL837X_IGBW_PORT_FC_CTRL, 0x55);
        command(invalid[i]);
        assert(err_status == ERR_INVALID_ARGUMENT);
        assert(hw_reg_get(in) == 0x100100 && hw_reg_get(out) == 0x100200);
        assert(hw_reg_get(RTL837X_IGBW_PORT_FC_CTRL) == 0x55 && hw_writes == 0);
    }
    command("bw status 3");
    assert(err_status == ERR_OK && hw_reg_get(in) == 0x100100 && hw_reg_get(out) == 0x100200);
    command("bw in 3 0100");
    assert(err_status == ERR_OK && hw_reg_get(in) == 0x100010 && hw_reg_get(out) == 0x100200);
    command("bw out 3 0200");
    assert(err_status == ERR_OK && hw_reg_get(out) == 0x100020 && hw_reg_get(in) == 0x100010);
    command("bw in 3 drop");
    assert(err_status == ERR_OK && hw_reg_get(RTL837X_IGBW_PORT_FC_CTRL) == 0x51);
    command("bw in 3 fc");
    assert(err_status == ERR_OK && hw_reg_get(RTL837X_IGBW_PORT_FC_CTRL) == 0x55);
    command("bw in 3 off");
    assert(err_status == ERR_OK && hw_reg_get(in) == 0x0fffff && hw_reg_get(out) == 0x100020);
    command("bw out 3 off");
    assert(err_status == ERR_OK && hw_reg_get(out) == 0x0fffff);
    puts("PASS bandwidth CLI: invalid arity preserves limits/flow control; valid status, rates, modes and disable work");
    return 0;
}
'''
out = root / 'test/build'
out.mkdir(exist_ok=True)
source = out / 'bw_cli.c'
source.write_text(harness + '\n'.join(functions) + tests)
binary = out / 'test_bw_cli'
subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) + [
    '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-pointer-sign',
    '-Wno-unknown-pragmas', '-Wno-parentheses', '-DRTLP_HOST_TEST', '-g',
    '-Wno-incompatible-pointer-types',  # SDCC permits the backend's existing byte-pointer alias.
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-include', str(root / 'test/sdcc_shim.h'), '-I' + str(root / 'test/stub'),
    '-I' + str(root), '-I' + str(root / 'uip'), '-I' + str(root / 'httpd'),
    str(source), str(root / 'test/hw_mock.c'), str(root / 'rtl837x_bandwidth.c'),
    '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
