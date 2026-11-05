#!/usr/bin/env python3
"""Replay real cable-command logic/controller over a modeled powered PHY."""
import os
import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / 'cmd_parser.c').read_text()
functions = []
for signature in ('uint8_t cmd_compare(', 'static void cmd_error(', 'void cmd_tokenize('):
    start = src.index(signature)
    functions.append(src[start:src.index('\n}', start) + 2])
start = src.index('} else if (cmd_compare(2, "cabletest")) {')
start = src.index('{', start) + 1
branch = src[start:src.index('\n\t} else if', start)].removesuffix('\n#endif')
if 'cmd_error(' not in branch:
    functions = [f for f in functions if not f.startswith('static void cmd_error(')]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_phy.h"
#include "rtl837x_cabletest.h"
#include "phy.h"
#include "machine.h"
#include "cmd_parser.h"
#define N_WORDS 15
uint8_t cmd_words_len, cmd_words_b[N_WORDS];
volatile uint32_t ticks;
struct phy_settings phy_settings;
struct machine_runtime machine_detected = { .isRTL8373 = 1 };
extern uint8_t ct_state, ct_port, ct_have;
extern uint16_t ct_bmcr;
static uint16_t bmcr[9], power[9];
static unsigned phy_reads, phy_writes;
void phy_read(uint8_t port, uint8_t dev, uint16_t reg) {
    assert(port < 9 && dev == PHY_MMD31);
    phy_reads++;
    if (reg == 0xa610) SFR_DATA_U16 = power[port];
    else { assert(reg == PHY_MMD31_FEDCR); SFR_DATA_U16 = bmcr[port]; }
}
void phy_write(uint8_t port, uint8_t dev, uint16_t reg, uint16_t value) {
    assert(port < 9 && dev == PHY_MMD31 && reg == PHY_MMD31_FEDCR);
    phy_writes++; bmcr[port] = value;
}
void itoa_short(uint16_t value) { char text[6]; snprintf(text, sizeof text, "%u", (unsigned)value); print_string(text); }
void print_phys_port(uint8_t port) { itoa_short(port + 1); }
'''
tests = r'''
static void command(const char *text) {
    strcpy((char *)cmd_buffer, text);
    cmd_tokenize();
    if (err_status == ERR_OK && cmd_compare(2, "cabletest")) parse_cable();
}
static void ready(void) {
    phy_settings.port = 4; ct_port = 4; ct_state = 0; ct_have = 1;
    bmcr[4] = 0x1140; power[4] = 0; phy_reads = phy_writes = 0;
}
int main(void) {
    const char *invalid[] = {"port 5 cabletest nonsense", "port 5 cabletest show extra"};
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        ready(); command(invalid[i]);
        if (err_status != ERR_INVALID_ARGUMENT || ct_state || !ct_have ||
            bmcr[4] != 0x1140 || phy_reads || phy_writes) {
            printf("FAIL %s: status=%u diagnostic_state=%u PHYwrites=%u\n",
                   invalid[i], err_status, ct_state, phy_writes);
            failures++;
        }
    }
    ready(); command("port 5 cabletest");
    if (err_status != ERR_OK || !ct_state || ct_port != 4 || ct_have ||
        ct_bmcr != 0x1140 || !(bmcr[4] & 0x0800)) failures++;
    uint16_t running_bmcr = bmcr[4];
    phy_reads = phy_writes = 0; command("port 5 cabletest show");
    if (err_status != ERR_OK || !ct_state || bmcr[4] != running_bmcr || phy_reads || phy_writes) failures++;
    ready(); command("port 5 cabletest show");
    if (err_status != ERR_OK || ct_state || !ct_have || bmcr[4] != 0x1140 || phy_reads || phy_writes) failures++;
    printf("Cable CLI: invalid commands preserve PHY/result state; valid run/show: %u failures\n", failures);
    return failures != 0;
}
'''
out = root / 'test/build'
out.mkdir(exist_ok=True)
source = out / 'cable_cli.c'
source.write_text(harness + '\n'.join(functions) + '\nstatic void parse_cable(void) {\n' + branch + '\n}\n' + tests)
binary = out / 'test_cable_cli'
subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) + [
    '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-Wno-pointer-sign', '-Wno-unknown-pragmas', '-DRTLP_HOST_TEST',
    '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-include', str(root / 'test/sdcc_shim.h'), '-I' + str(root / 'test/stub'),
    '-I' + str(root), '-I' + str(root / 'uip'), '-I' + str(root / 'httpd'),
    str(source), str(root / 'test/support.c'), str(root / 'test/hw_mock.c'),
    str(root / 'rtl837x_cabletest.c'), str(root / 'timer.c'),
    '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
    '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
