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
void parse_eee(void);
void parse_poe(void);
const struct machine machine = {
    .phys_to_log_port = {0, 1, 2, 3, 4, 5, 6, 7, 0xff},
    .log_to_phys_port = {1, 2, 3, 4, 5, 6, 7, 8, 0}
};
bool lldp_enabled;
uint16_t lldp_logical_port_status;
static uint16_t registers[8][32];
static uint16_t eee_advertised[8], eee_config[8];
static uint16_t gpio_out;
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
void itoa(uint8_t n) { print_byte(n); }
void print_short(uint16_t n) { char s[8]; snprintf(s, sizeof(s), "%u", n); print_string(s); }
void print_phys_port(uint8_t n) { print_byte(n + 1); }
void lldp_shutdown(uint16_t ports) { (void)ports; }

void phy_read(uint8_t port, uint8_t dev, uint16_t reg) {
    (void)dev;
    if (port >= 8 || (reg >= 32 && reg != RTL836X_OCP_EEE_ADV)) abort();
    SFR_DATA_U16 = reg == RTL836X_OCP_EEE_ADV ? eee_advertised[port] : registers[port][reg];
    SFR_DATA_8 = SFR_DATA_U16 >> 8;
    SFR_DATA_0 = SFR_DATA_U16;
}
void phy_write(uint8_t port, uint8_t dev, uint16_t reg, uint16_t value) {
    (void)dev;
    if (port >= 8 || (reg >= 32 && reg != RTL836X_OCP_EEE_ADV)) abort();
    if (reg == RTL836X_OCP_EEE_ADV) eee_advertised[port] = value;
    else registers[port][reg] = value;
    writes++;
}
void reg_read(uint16_t reg) {
    if (reg == RTL836X_REG_GPIO_67C_O_X0) SFR_DATA_U16 = gpio_out;
    else if (reg >= RTL836X_REG_PORT0_EEE_CFG && reg <= RTL836X_REG_PORT0_EEE_CFG + 7 * 32 &&
        (reg - RTL836X_REG_PORT0_EEE_CFG) % 32 == 0)
        SFR_DATA_U16 = eee_config[(reg - RTL836X_REG_PORT0_EEE_CFG) / 32];
    else {
        if (reg < RTL836X_REG_PORT0_STATUS || reg >= RTL836X_REG_PORT0_STATUS + 8) abort();
        SFR_DATA_U16 = 0x0016; /* linked, full duplex, 1G */
    }
    SFR_DATA_8 = SFR_DATA_U16 >> 8;
    SFR_DATA_0 = SFR_DATA_U16;
}
void reg_write(uint16_t reg) {
    if (reg == RTL836X_REG_GPIO_67C_O_X0)
        gpio_out = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
    else {
        if (reg < RTL836X_REG_PORT0_EEE_CFG || reg > RTL836X_REG_PORT0_EEE_CFG + 7 * 32 ||
            (reg - RTL836X_REG_PORT0_EEE_CFG) % 32) abort();
        eee_config[(reg - RTL836X_REG_PORT0_EEE_CFG) / 32] = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
    }
    writes++;
}

#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); failures++; } } while (0)

static void reset(void) {
    memset(registers, 0, sizeof(registers));
    for (unsigned i = 0; i < 8; i++) {
        eee_advertised[i] = 0xa001;
        eee_config[i] = 0x6040;
    }
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
    if (strncmp(s, "eee ", 4) == 0) parse_eee();
    else if (strncmp(s, "poe ", 4) == 0) parse_poe();
    else parse_port();
}
static void uplink_unchanged(void) {
    CHECK(registers[7][0] == 0x1140 && registers[7][4] == 0x0de1 && registers[7][9] == 0x1600,
          "commands for port 1 never change the uplink PHY");
    CHECK(eee_advertised[7] == 0xa001 && eee_config[7] == 0x6040,
          "EEE tests leave uplink settings untouched");
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
    const char *pause_commands[] = {"port 1 flowcontrol off", "port 1 flowcontrol on",
        "port 1 flowcontrol tx", "port 1 flowcontrol rx"};
    const uint16_t pause_values[] = {0x01e1, 0x05e1, 0x09e1, 0x0de1};
    for (unsigned i = 0; i < 4; i++) {
        reset(); registers[0][0] |= 0x0800;
        command(pause_commands[i]);
        CHECK(err_status == ERR_OK && registers[0][4] == pause_values[i],
              "pause advertisement preserves speed abilities and encodes direction");
        CHECK((registers[0][0] & 0x1a00) == 0x1a00,
              "pause changes restart negotiation without re-enabling a disabled PHY");
        uplink_unchanged();
    }
    reset(); command("eee on 1 100m");
    CHECK(err_status == ERR_OK && eee_advertised[0] == 0xa003 && eee_config[0] == 0x6840,
          "100M EEE sets both PHY advertisement and MAC capability while preserving unrelated bits");
    command("eee on 1");
    CHECK(err_status == ERR_OK && eee_advertised[0] == 0xa007 && eee_config[0] == 0x6c40,
          "default EEE enables 100M and 1G, never 2.5G");
    command("eee off 1");
    CHECK(err_status == ERR_OK && eee_advertised[0] == 0xa001 && eee_config[0] == 0x6040,
          "EEE off clears both native capability masks");
    uplink_unchanged();
    reset(); command("port 1 off"); command("eee on 1");
    CHECK(err_status == ERR_OK && (registers[0][0] & 0x0800),
          "EEE changes never re-enable a disabled PHY");
    const char *bad_controls[] = {"port 1 flowcontrol", "port 1 flowcontrol invalid",
        "port 1 flowcontrol on extra", "eee on 1 2g5", "eee on 8x", "eee on banana",
        "eee on 100m 1", "eee on 9", "eee off 0"};
    for (unsigned i = 0; i < sizeof(bad_controls)/sizeof(bad_controls[0]); i++) {
        reset(); command(bad_controls[i]);
        CHECK(err_status != ERR_OK && writes == 0, bad_controls[i]);
    }
    gpio_out = 0xffff;
    command("poe port 2 off");
    CHECK(err_status == ERR_OK && gpio_out == 0xffdf,
          "PoE port 2 clears only GPIO 5, preserving controller reset and LED pins");
    command("poe port 2 on");
    CHECK(err_status == ERR_OK && gpio_out == 0xffff, "PoE port 2 restores only its enable pin");
    command("poe port 3 off");
    CHECK(err_status == ERR_OK && gpio_out == 0xfeff, "PoE port 3 maps to GPIO 8");
    command("poe port 3 on");
    CHECK(err_status == ERR_OK && gpio_out == 0xffff, "PoE port 3 can be re-enabled independently");
    command("poe port 1 off");
    CHECK(err_status == ERR_OK && gpio_out == 0xfffd, "physical port 1 maps to GPIO 1");
    command("poe port 1 on");
    command("poe port 4 off");
    CHECK(err_status == ERR_OK && gpio_out == 0xfdff, "physical port 4 maps to GPIO 9");
    command("poe port 4 on");
    const char *bad_poe[] = {"poe port 0 off", "poe port 5 off", "poe port 8 off",
        "poe port 2 bogus", "poe port 2 off extra", "poe global off"};
    for (unsigned i = 0; i < sizeof(bad_poe)/sizeof(bad_poe[0]); i++) {
        writes = 0; command(bad_poe[i]);
        CHECK(err_status != ERR_OK && writes == 0 && gpio_out == 0xffff, bad_poe[i]);
    }

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
