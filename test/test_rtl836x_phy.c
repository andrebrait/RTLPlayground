/* Exercise real port commands against an eight-port Clause 22 register file. */
#include <stdio.h>
#include <stdlib.h>
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
#include "rtl836x_regs.h"
#include "cmd_parser.h"
#include "machine.h"

void parse_port(void);
const struct machine machine = {
    .phys_to_log_port = {0, 1, 2, 3, 4, 5, 6, 7, 0xff}
};
bool lldp_enabled;
uint16_t lldp_logical_port_status;
static uint16_t registers[8][32];
static unsigned writes, failures, checks;
static char output[2048];
static size_t output_len;

void write_char(char c) {
    if (output_len + 1 < sizeof(output)) {
        output[output_len++] = c;
        output[output_len] = 0;
    }
}
void print_string(const char *s) { while (*s) write_char(*s++); }
void print_string_x(char *s) { print_string(s); }
void print_byte(uint8_t n) { char s[8]; snprintf(s, sizeof(s), "%u", n); print_string(s); }
void print_short(uint16_t n) { char s[8]; snprintf(s, sizeof(s), "%u", n); print_string(s); }
void print_phys_port(uint8_t n) { print_byte(n + 1); }
void lldp_shutdown(uint16_t ports) { (void)ports; }

void phy_read(uint8_t port, uint8_t dev, uint16_t reg) {
    (void)dev;
    if (port >= 8 || reg >= 32) abort();
    SFR_DATA_U16 = registers[port][reg];
    SFR_DATA_8 = SFR_DATA_U16 >> 8;
    SFR_DATA_0 = SFR_DATA_U16;
}
void phy_write(uint8_t port, uint8_t dev, uint16_t reg, uint16_t value) {
    (void)dev;
    if (port >= 8 || reg >= 32) abort();
    registers[port][reg] = value;
    writes++;
}
void reg_read(uint16_t reg) {
    if (reg < RTL836X_REG_PORT0_STATUS || reg >= RTL836X_REG_PORT0_STATUS + 8) abort();
    SFR_DATA_U16 = 0x0016; /* linked, full duplex, 1G */
    SFR_DATA_8 = 0;
    SFR_DATA_0 = 0x16;
}

#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); failures++; } } while (0)

static void reset(void) {
    memset(registers, 0, sizeof(registers));
    registers[0][0] = 0x1140;
    registers[0][4] = 0x0de1; /* pause, asymmetric pause, all 10/100 modes */
    registers[0][9] = 0x1600; /* master/slave settings and full gigabit */
    registers[7][0] = 0x1140;
    registers[7][4] = 0x0de1;
    registers[7][9] = 0x1600;
    writes = 0;
}
static void command(const char *s) {
    output_len = 0; output[0] = 0;
    snprintf((char *)cmd_buffer, CMD_BUF_SIZE, "%s", s);
    cmd_tokenize();
    parse_port();
}
static void uplink_unchanged(void) {
    CHECK(registers[7][0] == 0x1140 && registers[7][4] == 0x0de1 && registers[7][9] == 0x1600,
          "commands for port 1 never change the uplink PHY");
}
int main(void) {
    reset(); command("port 1 off");
    CHECK(err_status == ERR_OK && (registers[0][0] & 0x0800), "off powers down the selected PHY");
    CHECK(registers[0][4] == 0x0de1 && registers[0][9] == 0x1600, "off preserves advertised abilities");
    command("port 1 on");
    CHECK(err_status == ERR_OK && (registers[0][0] & 0x1800) == 0x1000, "on clears power down and enables negotiation");
    CHECK((registers[0][0] & 0x0200) != 0, "on restarts negotiation");
    uplink_unchanged();

    const struct { const char *command; uint16_t adv, gigabit; } modes[] = {
        {"port 1 10m half", 0x0c21, 0x1400},
        {"port 1 10m full", 0x0c41, 0x1400},
        {"port 1 10m", 0x0c61, 0x1400},
        {"port 1 100m half", 0x0c81, 0x1400},
        {"port 1 100m full", 0x0d01, 0x1400},
        {"port 1 100m", 0x0d81, 0x1400},
        {"port 1 1g", 0x0c01, 0x1600},
        {"port 1 auto", 0x0de1, 0x1600},
    };
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        reset(); registers[0][0] |= 0x0800;
        command(modes[i].command);
        CHECK(err_status == ERR_OK, modes[i].command);
        CHECK(registers[0][4] == modes[i].adv && registers[0][9] == modes[i].gigabit,
              "speed selection limits advertised modes without changing pause/master settings");
        CHECK((registers[0][0] & 0x1a00) == 0x1200, "speed selection powers on and restarts auto-negotiation");
        uplink_unchanged();
    }
    reset(); command("port 1 duplex full");
    CHECK(err_status == ERR_OK && registers[0][4] == 0x0d41 && registers[0][9] == 0x1600,
          "full duplex removes half-duplex advertisements while retaining supported speeds");
    reset(); command("port 1 duplex half");
    CHECK(err_status == ERR_OK && registers[0][4] == 0x0ca1 && registers[0][9] == 0x1400,
          "half duplex removes full duplex and unsupported gigabit half duplex");
    reset(); command("port 1 off"); command("port 1 duplex full");
    CHECK(err_status == ERR_OK && (registers[0][0] & 0x0800),
          "duplex changes never re-enable a disabled PHY");
    CHECK(registers[0][4] == 0x0d41, "duplex changes retain disabled-port settings");
    reset(); command("port 1 1g"); writes = 0;
    command("port 1 duplex half");
    CHECK(err_status != ERR_OK && writes == 0, "gigabit-only cannot become unsupported half duplex");

    const char *invalid[] = {"port 0 off", "port 9 off", "port 8x off", "port 1 2g5",
        "port 1 5g", "port 1 10g", "port 1 1g half", "port 1 100m bogus",
        "port 1 duplex", "port 1 duplex bogus", "port 1 off extra", "port 1 auto half"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        reset(); command(invalid[i]);
        CHECK(err_status != ERR_OK && writes == 0, invalid[i]);
    }
    reset(); command("port 1 show");
    CHECK(err_status == ERR_OK && writes == 0 && strstr(output, "1000M"), "show reports link speed without writes");
    command("port 1 name doorbell");
    CHECK(err_status == ERR_OK && writes == 0, "existing port naming remains read-only to PHYs");
    printf("RTL836x port commands: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
