#!/usr/bin/env python3
"""Exercise the production UART writer and idle/tick loop without hardware."""
import os
import pathlib
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / 'rtlplayground.c').read_text()


def function(name):
    start = src.index(name)
    opening = src.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]


preamble = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define __bit bool
#define __xdata
'''
uart = preamble + r'''
#define TCP_OUTBUF_SIZE 32
#define CMD_TRUNCATED "..."
static bool uart_dead, tx_empty, complete_tx;
static unsigned polls, writes;
#define LOGBUF_SIZE 32
static char logbuf[LOGBUF_SIZE];
static struct {
    bool enabled, line_available;
    uint16_t writeptr, readptr;
} syslog_state;
static uint8_t serial, outbuf[TCP_OUTBUF_SIZE], cmd_capture;
static uint16_t slen;
/* Model the TX interrupt when the previous character completes. */
static bool *empty_flag(void) {
    polls++;
    if (complete_tx) tx_empty = true;
    return &tx_empty;
}
static uint8_t *uart_buffer(void) { writes++; return &serial; }
#define tx_buf_empty (*empty_flag())
#define SBUF (*uart_buffer())
''' + function('static bool uart_ready(void)') + '\n' + function('void write_char_no_syslog(char c)') + '\n' + function('void write_char(char c)') + r'''
int main(void) {
    complete_tx = true;
    write_char_no_syslog('x');
    assert(writes == 1 && serial == 'x');
    write_char_no_syslog('\n');
    assert(writes == 3 && serial == '\n'); /* CR and LF */
    complete_tx = tx_empty = false;
    writes = polls = 0;
    cmd_capture = 1;
    write_char_no_syslog('a');
    assert(uart_dead && polls == 0xffff && !writes);
    write_char_no_syslog('b');
    assert(polls == 0xffff && !writes); /* timeout latches, no more waits */
    assert(slen == 2 && outbuf[0] == 'a' && outbuf[1] == 'b');
    syslog_state.enabled = true;
    write_char('s');
    write_char('\n');
    assert(logbuf[0] == 's' && logbuf[1] == '\n' && syslog_state.line_available);
    assert(slen == 4 && outbuf[2] == 's' && outbuf[3] == '\n' && !writes);
    /* A CR was accepted, but its completion interrupt never arrives. */
    uart_dead = false;
    tx_empty = true;
    writes = 0;
    write_char_no_syslog('\n');
    assert(uart_dead && writes == 1 && serial == '\r');
    puts("UART ready, CRLF, both bounded waits, dead latch and HTTP capture passed");
}
'''

idle = preamble + r'''
#define RTL836X 1
#define RTL837X 2
#define STP_CATCH_UP 8
#define health_phase(x) ((void)0)
/* No timer is due within the three ticks below: the bench is about RX, not scheduling. */
struct timer { int unused; };
static struct timer second, sfp_poll, stp_step;
static int timer_expired(struct timer *t) { (void)t; return 0; }
static void timer_reset(struct timer *t) { (void)t; }
static uint8_t arp_age_secs, PCON;
static uint32_t uptime_secs;
static bool stp_enabled, lacpEnabled, evflags = true, tick_pending, link_irq;
static bool rx_irq, EX1, cmd_available;
static unsigned rx_calls, acks;
#define ERR_OK 0
static uint8_t err_status;
static void handle_button(void) {}
static void uip_arp_timer(void) {}
static void handle_sfp(void) {}
static void handle_tx(void) {}
static void stp_timers(void) {}
static void lacp_timers(void) {}
static void lldp_tick(void) {}
static void cabletest_tick(void) {}
static void health_loop_start(void) {}
static void check_links(void) {}
static uint16_t link_changed;
static void lacp_link_change(uint16_t bounced) { (void)bounced; }
static void handle_rx(void) { rx_calls++; }
static void nic_rx_ack(void) { acks++; }
static bool nic_rx_pending(void) { return false; }
static void cmd_tokenize(void) {}
static void cmd_parser(void) {}
static void print_cmd_prompt(void) {}
''' + function('static void handle_tick(void)') + '\n' + function('void idle(void)') + r'''
int main(void) {
    for (unsigned n = 0; n < 3; n++) {
        tick_pending = true;
        idle(); /* no INT1: only Timer 2 sets tick_pending */
    }
#if SOC_FAMILY == RTL836X
    assert(rx_calls == 3 && acks == 3 && EX1);
#else
    assert(!rx_calls && !acks); /* the RTL837x remains interrupt-driven */
#endif
    puts("per-tick RX fallback and interrupt rearm passed");
}
'''

with tempfile.TemporaryDirectory(prefix='rtlplayground-safety-') as tmp:
    for name, code, flags in [('uart', uart, []), ('idle836x', idle, ['-DSOC_FAMILY=1']),
                               ('idle837x', idle, ['-DSOC_FAMILY=2'])]:
        source = pathlib.Path(tmp) / (name + '.c')
        binary = pathlib.Path(tmp) / name
        source.write_text(code)
        subprocess.run(shlex.split(os.environ.get('CC', 'gcc')) + [
            '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
            '-fsanitize=address,undefined', *flags, str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)
