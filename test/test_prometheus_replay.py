#!/usr/bin/env python3
"""Real tokenizer, the "prometheus" dispatch of cmd_parser.c, the startup
Flash replay and metrics_cli.c. Only Flash and console edges are mocked."""
from pathlib import Path
import os, shlex, subprocess
r = Path(__file__).resolve().parent.parent
src = (r / 'cmd_parser.c').read_text()

def function(signature):
    a = src.index(signature); b = src.index('{', a); depth = 1; i = b + 1
    while depth:
        depth += (src[i] == '{') - (src[i] == '}'); i += 1
    return src[a:i] + '\n'

branch = src.split('else if (cmd_compare(0, "prometheus")) {', 1)[1].split('#endif', 1)[0]
code = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "cmd_parser.h"
#include "metrics.h"
#define N_WORDS 15
#define PASSWORD "test-default"
#define FLASH_READ_BURST_SIZE 0x100
void clear_command_history(void) {}
uint8_t cmd_buffer[CMD_BUF_SIZE], cmd_words_len, cmd_words_b[N_WORDS], err_status, flash_buf[256];
char save_cmd, passwd[21], image[CONFIG_LEN];
struct flash_region_t flash_region;
uint8_t prometheus_enabled;
void flash_read_bulk(uint8_t *d) { memcpy(d, image + (flash_region.addr - CONFIG_START), flash_region.len); }
void print_string(const char *s) { (void)s; }
void print_string_x(char *s) { (void)s; }
void write_char(char c) { (void)c; }
uint16_t strtox(uint8_t *d, const char *s) { strcpy((char *)d, s); return strlen(s); }
'''
code += function('uint8_t cmd_compare(') + function('void cmd_tokenize(')
code += 'void cmd_parser(void){cmd_tokenize();err_status=ERR_OK;if(0){}else if (cmd_compare(0, "prometheus")) {' + branch + '}\n'
code += function('void execute_config(') + r'''
static void boot(const char *text) { memset(image, 0, sizeof image); strcpy(image, text); execute_config(); }
int main(void)
{
	boot("hostname lab\r\nprometheus on\r\n");
	assert(prometheus_enabled == 1);
	boot("prometheus on\nprometheus off\n");
	assert(prometheus_enabled == 0);
	boot("prometheus off\nprometheus on");
	assert(prometheus_enabled == 1);
	strcpy((char *)cmd_buffer, "prometheus off extra");
	cmd_parser();
	assert(err_status != ERR_OK && prometheus_enabled == 1);
	puts("PASS startup replay restores \"prometheus on|off\", the last line wins, extra words are refused");
	return 0;
}
'''
out = r / 'test/build/prometheus-replay'; out.mkdir(parents=True, exist_ok=True)
c = out / 'replay.c'; c.write_text(code)
cc = shlex.split(os.environ.get('CC', 'cc')); exe = out / 'test'
args = cc + ['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unknown-pragmas',
             '-Wno-pointer-sign', '-DPROMETHEUS', '-DRTLP_HOST_TEST', '-include', 'test/sdcc_shim.h', '-Itest/stub', '-Itest',
             '-I.', '-Ihttpd', '-Iuip', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), 'metrics_cli.c',
             '-o', str(exe)]
if 'clang' not in subprocess.check_output([cc[0], '--version'], text=True):
    args.append('-Wno-builtin-declaration-mismatch')
subprocess.run(args, cwd=r, check=True)
subprocess.run([str(exe)], check=True)
