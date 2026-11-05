#!/usr/bin/env python3
"""Exercise the production IGMP command branch over the actual ASIC backend."""
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
start = src.index('} else if (cmd_compare(0, "igmp")) {')
start = src.index('{', start) + 1
branch = src[start:src.index('\n\t\t} else if', start)]
# The native dispatcher closes its outer SOC_FAMILY guard after this branch.
branch = branch.removesuffix('\n#endif')
harness = r'''
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_igmp.h"
#include "cmd_parser.h"
#include "machine.h"
#include <test/hw_mock.h>
#define N_WORDS 15
uint8_t cmd_words_len, cmd_words_b[N_WORDS], igmpEnabled;
void print_sfr_data(void) {}
'''
tests = r'''
static void command(const char *text) {
    strcpy((char *)cmd_buffer, text);
    cmd_tokenize();
    if (err_status == ERR_OK) parse_igmp();
}
int main(void) {
    const char *invalid[] = {"igmp on extra", "igmp off extra", "igmp show extra",
                            "igmp", "igmp onx", "igmp offx", "igmp showx"};
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        hw_reset();
        hw_reg_set(RTL837X_L2_CTRL, 0xa55a);
        igmpEnabled = i == 1;
        uint8_t before = igmpEnabled;
        command(invalid[i]);
        if (err_status != ERR_INVALID_ARGUMENT || igmpEnabled != before || hw_reads || hw_writes) {
            printf("FAIL %s: status=%u enabled=%u writes=%lu\n", invalid[i], err_status, igmpEnabled, hw_writes);
            failures++;
        }
    }
    hw_reset();
    command("igmp on");
    if (err_status != ERR_OK || !igmpEnabled ||
        !reg_bit_test(RTL837X_L2_CTRL, L2_CTRL_LUT_IPMC_HASH) ||
        hw_reg_get(RTL837X_IGMP_PORT_CFG + (MACHINE_MIN_PORT << 2)) !=
        (IGMP_MAX_GROUP | IGMP_PROTOCOL_ENABLE | IGMP_TRAP)) failures++;
    unsigned long writes = hw_writes;
    command("igmp show");
    if (err_status != ERR_OK || !igmpEnabled || hw_writes != writes) failures++;
    command("igmp off");
    if (err_status != ERR_OK || igmpEnabled ||
        reg_bit_test(RTL837X_L2_CTRL, L2_CTRL_LUT_IPMC_HASH) ||
        hw_reg_get(RTL837X_IGMP_PORT_CFG + (MACHINE_MIN_PORT << 2)) !=
        (IGMP_MAX_GROUP | IGMP_PROTOCOL_ENABLE | IGMP_FLOOD)) failures++;
    printf("IGMP CLI: malformed commands preserve state; valid on/off/show: %u failures\n", failures);
    return failures != 0;
}
'''
out = root / 'test/build'
out.mkdir(exist_ok=True)
source = out / 'igmp_cli.c'
source.write_text(harness + '\n'.join(functions) + '\nstatic void parse_igmp(void) {\n' + branch + '\n}\n' + tests)
binary = out / 'test_igmp_cli'
subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) + [
    '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-Wno-pointer-sign', '-Wno-unknown-pragmas', '-DRTLP_HOST_TEST',
    '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-include', str(root / 'test/sdcc_shim.h'), '-I' + str(root / 'test/stub'),
    '-I' + str(root), '-I' + str(root / 'uip'), '-I' + str(root / 'httpd'),
    str(source), str(root / 'test/support.c'), str(root / 'test/env_tables.c'),
    str(root / 'test/hw_mock.c'), str(root / 'rtl837x_igmp.c'),
    '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
    '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
