/* Real native init and EEE backend over retained switch/PHY register state. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl836x_regs.h"
#include "rtl837x_init.h"
#include "machine.h"

volatile uint8_t SFR_DATA_24, SFR_DATA_16, SFR_DATA_8, SFR_DATA_0;
volatile uint16_t SFR_DATA_U16;
uint8_t err_status, sfr_data[4];
static uint16_t registers[0x2000], mii[8][32], eee[8];

void print_string(const char *s) { (void)s; }
void print_short(uint16_t n) { (void)n; }
void write_char(char c) { (void)c; }
void reg_read(uint16_t reg) {
    assert(reg < 0x2000);
    SFR_DATA_U16 = registers[reg];
    SFR_DATA_8 = SFR_DATA_U16 >> 8; SFR_DATA_0 = SFR_DATA_U16;
}
void reg_write(uint16_t reg) {
    assert(reg < 0x2000);
    registers[reg] = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
}
void phy_read(uint8_t port, uint8_t dev, uint16_t reg) {
    (void)dev;
    assert(port < 8);
    if (reg == RTL836X_OCP_EEE_ADV) SFR_DATA_U16 = eee[port];
    else {
        if (reg == RTL836X_OCP_MII) reg = 0;
        assert(reg < 32);
        SFR_DATA_U16 = mii[port][reg];
    }
    SFR_DATA_8 = SFR_DATA_U16 >> 8; SFR_DATA_0 = SFR_DATA_U16;
}
void phy_write(uint8_t port, uint8_t dev, uint16_t reg, uint16_t value) {
    (void)dev;
    assert(port < 8);
    if (reg == RTL836X_OCP_EEE_ADV) eee[port] = value;
    else {
        if (reg == RTL836X_OCP_MII) reg = 0;
        assert(reg < 32);
        mii[port][reg] = value;
    }
}
void phy_modify(uint8_t port, uint8_t dev, uint16_t reg, uint16_t mask, uint16_t set) {
    phy_read(port, dev, reg);
    phy_write(port, dev, reg, (SFR_DATA_U16 & ~mask) | set);
}
int main(void) {
    registers[RTL836X_REG_CHIP_NUMBER] = 0x0652;
    for (unsigned port = 0; port < 8; port++) {
        eee[port] = 0xa001 | (MACHINE_DEFAULT_EEE ? 0 : 6);
        registers[RTL836X_REG_PORT0_EEE_CFG + port * 32] = 0x2040 | (MACHINE_DEFAULT_EEE ? 0 : 0x0c00);
        mii[port][0] = 0x0940;
    }
    rtl8370_init();
    assert(err_status == ERR_OK);
    for (unsigned port = 0; port < 8; port++) {
        assert(eee[port] == (0xa001 | (MACHINE_DEFAULT_EEE ? 6 : 0)));
        assert(registers[RTL836X_REG_PORT0_EEE_CFG + port * 32] == (0x2040 | (MACHINE_DEFAULT_EEE ? 0x0c00 : 0)));
        assert(mii[port][0] == 0x0340);
    }
    printf("PASS native boot EEE=%d replaces retained advertisement/MAC state and enables every PHY\n", MACHINE_DEFAULT_EEE);
    return 0;
}
