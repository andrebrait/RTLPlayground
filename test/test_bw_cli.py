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
                  'void cmd_tokenize(', 'void parse_bw(', 'void parse_storm('):
    start = src.index(signature)
    functions.append(src[start:src.index('\n}', start) + 2])
start = src.index('static __code const char * __code const storm_types')
functions.insert(0, src[start:src.index(';', start) + 1])
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_bandwidth.h"
#include "rtl837x_storm.h"
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
    if (err_status == ERR_OK) {
        if (cmd_compare(0, "bw")) parse_bw();
        else if (cmd_compare(0, "storm")) parse_storm();
    }
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
    hw_reset();
    storm_set(2, 0, 128, 0);
    storm_set(3, 1, 64, 1);
    const uint8_t meter = STORM_METER(2, 0);
    const unsigned long writes = hw_writes;
    const char *storm_invalid[] = {
        "storm 3 bcast 8 nope", "storm 3 nope 8 kbps",
        "storm 3 bcast 0 kbps", "storm 3 bcast 8 kbps extra"
    };
    for (unsigned i = 0; i < sizeof storm_invalid / sizeof storm_invalid[0]; i++) {
        command(storm_invalid[i]);
        assert(err_status == ERR_INVALID_ARGUMENT && hw_writes == writes);
        assert(hw_reg_get(RTL837X_STORM_CTRL) == 4 && hw_reg_get(RTL837X_STORM_CTRL + 4) == 8);
        assert(hw_reg_get(RTL837X_METER_RATE + (meter << 2)) == 128);
    }
    command("storm");
    assert(err_status == ERR_OK && hw_writes == writes);
    command("storm 3 bcast 256 kbps");
    assert(err_status == ERR_OK && hw_reg_get(RTL837X_METER_RATE + (meter << 2)) == 256);
    command("storm 3 bcast 512 pps");
    assert(err_status == ERR_OK && hw_reg_get(RTL837X_METER_RATE + (meter << 2)) == 512);
    assert(reg_bit_test(RTL837X_METER_MODE + ((meter >> 5) << 2), meter & 31));
    command("storm 3 bcast off");
    assert(err_status == ERR_OK && hw_reg_get(RTL837X_STORM_CTRL) == 0 && hw_reg_get(RTL837X_STORM_CTRL + 4) == 8);
    puts("PASS rate-control CLI: invalid commands preserve limits/meters; valid bandwidth and storm commands work");
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
    str(source), str(root / 'test/hw_mock.c'), str(root / 'rtl837x_bandwidth.c'), str(root / 'rtl837x_storm.c'),
    '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
