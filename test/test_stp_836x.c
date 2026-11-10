/* Native STP register boundary; the shared engine and native port layer are real. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_stp.h"
#include "rtl837x_sfr.h"
#include "uip.h"
#include "machine.h"

uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE], sfr_data[4], err_status;
uint16_t uip_len, management_vlan = 7;
bool stp_enabled, lldp_enabled;
volatile uint32_t ticks;	/* timer.c's clock; stp_setup() arms stp_step from it */
struct uip_eth_addr uip_ethaddr = {.addr = {2, 1, 2, 3, 4, 5}};
const struct machine machine = {
    .phys_to_log_port = {0, 1, 2, 3, 4, 5, 6, 7, 0xff},
    .log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 0}
};
struct machine_runtime machine_detected;
volatile uint8_t SFR_DATA_0, SFR_DATA_8, SFR_DATA_16, SFR_DATA_24;
volatile uint16_t SFR_DATA_U16;
static uint16_t registers[0x4000];
static unsigned failures;
static uint8_t protected_ports, flushed_ports, last_tx[80];
static unsigned blocked_writes, tx_count[8], last_tx_len;

void print_string(const char *s) {(void)s;}
void print_byte(uint8_t v) {(void)v;}
void print_short(uint16_t v) {(void)v;}
void print_long(uint32_t v) {(void)v;}
void print_phys_port(uint8_t v) {(void)v;}
void write_char(char c) {(void)c;}
void itoa(uint8_t v) {(void)v;}
void print_reg(uint16_t r) {(void)r;}
void tcpip_output(void) {
    const uint8_t *p = uip_buf + RTL_FRAME_DESC_SIZE;
    last_tx_len = uip_len;
    memcpy(last_tx, p, uip_len < sizeof(last_tx) ? uip_len : sizeof(last_tx));
    for (unsigned port = 0; port < 8; port++)
        if (p[19] & (1u << port)) tx_count[port]++;
    uip_len = 0;
}
void reg_read(uint16_t r) {
    SFR_DATA_U16 = registers[r];
    SFR_DATA_8 = registers[r] >> 8;
    SFR_DATA_0 = registers[r];
}
void reg_write(uint16_t r) {
    uint16_t value = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
    if (r == 0x0a00)
        for (unsigned p = 0; p < 8; p++)
            if ((protected_ports & (1u << p)) && ((value >> (2 * p)) & 3) != 3)
                blocked_writes++;
    if (r == 0x0a36) flushed_ports |= value;
    registers[r] = value;
}
void reg_read_m(uint16_t r) {
    reg_read(r);
    sfr_data[0] = sfr_data[1] = 0;
    sfr_data[2] = SFR_DATA_8;
    sfr_data[3] = SFR_DATA_0;
}
void reg_write_m(uint16_t r) {
    SFR_DATA_24 = sfr_data[2];
    SFR_DATA_16 = sfr_data[3];
    reg_write(r);
}
#define CHECK(c, label) do { if (!(c)) {printf("FAIL %s\n", label); failures++;} else printf("PASS %s\n", label); } while (0)

static void tick(unsigned n) {
    while (n--) stp_timers();
}

/* The native NIC normalizes RX by inserting RTL and VID 0 tags before this body. */
struct bpdu_in {
    uint8_t destination[6], source[6];
    struct rtl_tag rtl_tag;
    struct vlan_tag vlan;
    uint16_t length;
    uint8_t dsap, ssap, control;
    uint16_t protocol;
    uint8_t version, type, flags;
    struct bridge root;
    uint32_t cost;
    struct bridge sender;
    uint8_t port_priority, port;
    uint16_t age, max_age, hello, forward_delay;
    uint8_t version1_length;
};
_Static_assert(sizeof(struct bpdu_in) == 65, "normalized RSTP BPDU is 65 bytes");

static void receive_bpdu(uint8_t port) {
    struct bpdu_in p = {0};
    memcpy(p.destination, "\x01\x80\xc2\x00\x00\x00", 6);
    memcpy(p.source, "\x02\xaa\xbb\xcc\xdd\xee", 6);
    p.rtl_tag.pmask = HTONS(port);
    p.vlan.svlan = HTONS(0x8100);
    p.length = HTONS(39);
    p.dsap = p.ssap = 0x42;
    p.control = 3;
    p.version = p.type = 2;
    p.flags = 0x3c;
    p.root.prio = p.sender.prio = 0x10;
    memcpy(p.root.mac, p.source, 6);
    memcpy(p.sender.mac, p.source, 6);
    p.port_priority = 0x80;
    p.port = 1;
    p.max_age = 20;
    p.hello = 2;
    p.forward_delay = 15;
    memcpy(uip_buf, &p, sizeof(p));
    uip_len = sizeof(p);
    stp_in();
}

static void lifecycle(void) {
    registers[0x0a00] = 0xffff;
    registers[0x0a01] = 0xa5a5;
    registers[RTL836X_REG_PORT0_STATUS + 2] = 0x16; /* 1G full, link up */
    registers[RTL836X_REG_PORT0_STATUS + 7] = 0x16;
    stp_defaults();
    for (unsigned p = 0; p < 8; p++)
        stp_pflags[p] &= ~STP_PF_ENABLED;
    stp_pflags[2] |= STP_PF_ENABLED;
    protected_ports = 0xfb; /* every port except port 3 stays outside STP */
    stp_enabled = true;
    stp_setup();
    CHECK(stp_port_state(2) == 1 && stp_port_state(7) == 3 && !blocked_writes,
          "startup blocks only the participating port, never the excluded uplink");
    CHECK(registers[0x0a01] == ((0xa5a5 & ~0x30) | 0x30),
          "CPU 10 forwards and its register's unrelated bits survive startup");
    CHECK((registers[0x0800] & 0x180) == 0x80, "BPDUs trap to the CPU");
    tick(4 * STP_HZ);
    CHECK(stp_port_state(2) == 3 && (stp_pflags[2] & STP_PF_OPEREDGE),
          "BPDU silence automatically restores forwarding after the edge delay");
    unsigned other_tx = 0;
    for (unsigned p = 0; p < 8; p++) if (p != 2) other_tx += tx_count[p];
    CHECK(tx_count[2] && !other_tx && last_tx_len == 61
          && !memcmp(last_tx, "\x01\x80\xc2\x00\x00\x00", 6)
          && last_tx[18] == 0 && last_tx[19] == 4
          && last_tx[20] == 0 && last_tx[21] == 39
          && last_tx[22] == 0x42 && last_tx[27] == 2 && last_tx[28] == 2,
          "RSTP TX has the correct destination, single-port tag, LLC and BPDU type");
    registers[RTL836X_REG_PORT0_STATUS + 2] = 0;
    tick(STP_HZ);
    CHECK(stp_port_state(2) == 1 && !(stp_link_prev & 4) && (flushed_ports & 4),
          "carrier loss blocks the native port and flushes its learned addresses");
    registers[RTL836X_REG_PORT0_STATUS + 2] = 0x10; /* 10M half, link up */
    tick(4 * STP_HZ);
    CHECK(stp_port_state(2) == 3 && (stp_link_prev & 4), "carrier recovery re-enters forwarding automatically");
    receive_bpdu(2);
    tick(1);
    CHECK(stp_root_port == 2 && root_bridge_cost == 2000000UL && stp_cnt[STP_CNT_RX][2],
          "native normalized RX elects the superior root with the 10M path cost");
    stp_pflags[2] |= STP_PF_BPDUGUARD;
    receive_bpdu(2);
    CHECK(stp_port_state(2) == 0 && (stp_pflags[2] & STP_PF_TRIPPED),
          "BPDU guard disables only the participating port in the ASIC");
    stp_off();
    stp_enabled = false;
    CHECK(registers[0x0a00] == 0xffff && (registers[0x0a01] & 0x30) == 0x30
          && (registers[0x0800] & 0x180) == 0 && !blocked_writes,
          "global off restores forwarding and BPDU flooding without touching excluded ports");
    stp_bpdu_filter = 1;
    stp_off();
    CHECK((registers[0x0800] & 0x180) == 0x100, "BPDU filtering uses the native drop action while STP is off");
    stp_bpdu_filter = 0;
    stp_off();
}

static void aggregation(void) {
    protected_ports = 0;
    port_lag_members_set(0, 0x0005);
    port_lag_members_set(1, 0x0030);
    stp_defaults();
    stp_enabled = true;
    stp_setup();
    CHECK(STP_LAG_COUNT == 2 && stp_ent_id(STP_LAG_BASE) == 101 && stp_ent_id(STP_LAG_BASE + 1) == 102
          && stp_ent_of[0] == STP_LAG_BASE && stp_ent_of[2] == STP_LAG_BASE
          && stp_ent_of[4] == STP_LAG_BASE + 1 && stp_ent_of[5] == STP_LAG_BASE + 1,
          "both fixed ASIC groups map to exactly two STP aggregation entities");
    stp_port_admin(STP_LAG_BASE, 0);
    CHECK(stp_port_state(0) == 3 && stp_port_state(2) == 3
          && stp_port_state(4) == 1 && stp_port_state(5) == 1,
          "one aggregation entity changes every member but not the other group");
    stp_off();
    stp_enabled = false;
    port_lag_members_set(0, 0);
    port_lag_members_set(1, 0);
}

int main(void) {
    registers[0x0a00] = 0xffff;
    registers[0x0a01] = 0xa5a5;
    stp_port_admin(2, 1);
    CHECK(registers[0x0a00] == 0xffdf && stp_port_state(2) == 1,
          "port 3 enters blocking without changing other front ports");
    stp_port_admin(2, 0);
    CHECK(registers[0x0a00] == 0xffff && stp_port_state(2) == 3,
          "per-port STP off restores ordinary forwarding");
    registers[0x0a00] = 0xbfff;
    CHECK(stp_port_state(7) == 2, "port 8 learning state uses bits 15:14");
    CHECK(stp_port_state(CPU_LOGICAL_PORT) == ((0xa5a5 >> 4) & 3), "logical CPU 9 reads chip port 10");
    CHECK(registers[0x0a01] == 0xa5a5, "front-port changes leave CPU and reserved bits alone");
    lifecycle();
    aggregation();
    printf("Native STP: %u failures\n", failures);
    return failures != 0;
}
