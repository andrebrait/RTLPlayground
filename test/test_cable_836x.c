/* Native RTCT: real controller over the RTL8370N's OCP/SRAM hardware boundary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sdcc_shim.h"
#undef __sfr
#undef __sfr16
#undef __sfr32
#undef __sbit
#define __sfr volatile uint8_t
#define __sfr16 volatile uint16_t
#define __sfr32 volatile uint32_t
#define __sbit bool
#include "rtl837x_sfr.h"
#include "rtl837x_common.h"
#include "rtl837x_phy.h"
#include "machine.h"
#include "../rtl837x_cabletest.c"

const struct machine machine = {0};
struct machine_runtime machine_detected = {0};
struct phy_settings phy_settings;
volatile uint32_t ticks;
static uint16_t control, bmcr, index_reg, status[4], lengths[4];
static unsigned reads, writes, bmcr_writes;
static char output[1024];
static size_t output_len;
void write_char(char c) { assert(output_len + 1 < sizeof output); output[output_len++] = c; output[output_len] = 0; }
void print_string(const char *s) { while (*s) write_char(*s++); }
void print_phys_port(uint8_t p) { write_char('1' + p); }
void itoa_short(uint16_t n) { char s[6]; snprintf(s, sizeof s, "%u", n); print_string(s); }
void phy_read(uint8_t port, uint8_t dev, uint16_t reg) {
    (void)dev; assert(port < 8); reads++;
    if (reg == 0 || reg == 0xa400) SFR_DATA_U16 = bmcr;
    else if (reg == 0xa422) SFR_DATA_U16 = control;
    else if (reg == 0xa438) {
        if (index_reg >= 0x802b && index_reg <= 0x8037 && (index_reg - 0x802b) % 4 == 0)
            SFR_DATA_U16 = status[(index_reg - 0x802b) / 4];
        else { assert(index_reg >= 0x802d && index_reg <= 0x8039 && (index_reg - 0x802d) % 4 == 0); SFR_DATA_U16 = lengths[(index_reg - 0x802d) / 4]; }
    } else { fprintf(stderr, "Unexpected PHY read %04x\n", reg); assert(0); }
}
void phy_write(uint8_t port, uint8_t dev, uint16_t reg, uint16_t value) {
    (void)dev; assert(port < 8); writes++;
    if (reg == 0 || reg == 0xa400) { bmcr = value; bmcr_writes++; }
    else if (reg == 0xa422) control = value;
    else { assert(reg == 0xa436); index_reg = value; }
}
static void reset(void) {
    ct_state = ct_have = ct_valid = 0; phy_settings.port = 4;
    ticks = 0; control = 0x9200; bmcr = 0x1140;
    reads = writes = bmcr_writes = 0; output_len = 0; output[0] = 0;
}
static void poll(void) { ticks += SYS_TICK_HZ / 20; cabletest_tick(); }
int main(void) {
    reset(); bmcr |= 0x0800; cabletest_run();
    assert(!ct_state && !writes && bmcr == 0x1940);
    reset(); phy_settings.port = 8; cabletest_run(); assert(!reads && !writes);
    reset(); cabletest_run();
    assert(ct_state && !ct_have && control == 0x12f3 && bmcr == 0x1140 && !bmcr_writes);
    unsigned start_writes = writes; cabletest_run(); assert(writes == start_writes);
    /* No stale DONE result may be consumed before a completed poll. */
    poll(); assert(ct_state && !ct_have);
    status[0] = 0x0048; lengths[0] = 120; /* 1.5m: API raw/2 in 2.5cm units. */
    status[1] = 0x0050; lengths[1] = 40;  /* 0.5m */
    status[2] = 0x0044; lengths[2] = 80;  /* 1.0m */
    status[3] = 0x0148; lengths[3] = 65535; /* Unknown full word, not OPEN. */
    control |= 0x8000; poll();
    assert(!ct_state && ct_have && ct_valid && control == 0x12f2 && !bmcr_writes && bmcr == 0x1140);
    assert(strstr(output, "Pair A: open at 1.5 m") && strstr(output, "Pair B: short at 0.5 m"));
    assert(strstr(output, "Pair C: impedance mismatch at 1.0 m") && strstr(output, "Pair D: unknown"));
    reads = writes = 0; cabletest_show(); assert(!reads && !writes);
    reset(); cabletest_run();
    /* An admin change during the test must not be overwritten at completion. */
    bmcr = 0x1940;
    for (unsigned i = 0; i < 100 && ct_state; i++) poll();
    assert(!ct_state && ct_have && !ct_valid && control == 0x12f2 && bmcr == 0x1940 && !bmcr_writes);
    assert(strstr(output, "timed out"));
    reset(); cabletest_run(); status[0] = 0x0041; status[1] = 0x0042; status[2] = 0x0060; status[3] = 0;
    control |= 0x8000; poll();
    assert(ct_result[0] == CABLE_RESULT_UNSPEC && ct_result[1] == CABLE_RESULT_MISMATCH);
    assert(ct_result[2] == CABLE_RESULT_UNSPEC && ct_result[3] == CABLE_RESULT_UNSPEC);
    puts("Native cable diagnostics: fault distances, stale DONE, timeout, admin preservation and read-only show passed");
    return 0;
}
