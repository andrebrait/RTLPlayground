/* Production RMA action encoding against a host register file for each family. */
#include <assert.h>
#include <stdio.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_trap.h"

uint8_t sfr_data[4];
volatile uint8_t SFR_DATA_0, SFR_DATA_8, SFR_DATA_16, SFR_DATA_24;
volatile uint16_t SFR_DATA_U16;
bool lldp_enabled;
static uint32_t regs[0x7000];
static unsigned writes;
void reg_read(uint16_t r) {
    SFR_DATA_U16 = regs[r]; SFR_DATA_0 = regs[r]; SFR_DATA_8 = regs[r] >> 8;
}
void reg_write(uint16_t r) {
#if SOC_FAMILY == RTL836X
    regs[r] = ((uint16_t)SFR_DATA_24 << 8) | SFR_DATA_16;
#else
    regs[r] = ((uint32_t)SFR_DATA_24 << 24) | ((uint32_t)SFR_DATA_16 << 16) | ((uint16_t)SFR_DATA_8 << 8) | SFR_DATA_0;
#endif
    writes++;
}
void reg_read_m(uint16_t r) {
    for (unsigned i=0;i<4;i++) sfr_data[3-i] = regs[r] >> (8*i);
}
void reg_write_m(uint16_t r) {
    regs[r] = ((uint32_t)sfr_data[0]<<24) | ((uint32_t)sfr_data[1]<<16) | ((uint16_t)sfr_data[2]<<8) | sfr_data[3];
    writes++;
}
void reg_bits(uint16_t r, uint16_t mask, uint16_t value) {
    unsigned shift=0;
    while (!((mask >> shift)&1)) shift++;
    regs[r] = (regs[r] & ~mask) | ((value << shift) & mask);
    writes++;
}
int main(void) {
#if SOC_FAMILY == RTL836X
    const unsigned addresses[] = {0x0800,0x0802,0x080e};
    const uint32_t original=0x7465, mask=0x0180;
    const unsigned shift=7;
#else
    const unsigned addresses[] = {0x4ecc,0x4ed4,0x4ee8};
    const uint32_t original=0x123456c5, mask=0x30;
    const unsigned shift=4;
#endif
    const uint8_t selectors[] = {TRAP_RMA_STP,TRAP_RMA_LACP,TRAP_RMA_LLDP};
    for (unsigned i=0;i<3;i++) {
        for (unsigned action=TRAP_ACT_FORWARD;action<=TRAP_ACT_DROP;action++) {
            for (unsigned j=0;j<3;j++) regs[addresses[j]]=original;
#if SOC_FAMILY == RTL836X
            regs[0x0832]=original; regs[0x0833]=0xa5a0;
#endif
            trap_rma_set(selectors[i], action);
            assert(regs[addresses[i]] == ((original & ~mask) | (action << shift)));
            for (unsigned j=0;j<3;j++) if (j != i) assert(regs[addresses[j]] == original);
#if SOC_FAMILY == RTL836X
            if (i==2) {
                assert(regs[0x0832] == ((original & ~mask) | (action << shift)));
                assert(regs[0x0833] == 0xa5a1);
            }
#endif
        }
    }
    unsigned before=writes;
    trap_rma_set(0xff, TRAP_ACT_TRAP);
    trap_rma_set(TRAP_RMA_LLDP, 0xff);
    assert(writes==before);
#if SOC_FAMILY == RTL836X
    lldp_enabled=false;
    trap_init();
    assert((regs[0x0800]&0x180)==0 && (regs[0x0802]&0x180)==0 && (regs[0x080e]&0x180)==0);
    lldp_enabled=true;
    trap_init();
    assert((regs[0x080e]&0x180)==0x80 && (regs[0x0832]&0x180)==0x80);
#else
    trap_init();
    assert(regs[0x6724]==CPU_LOGICAL_PORT);
#endif
    puts("PASS RMA forward/trap/drop fields, unrelated bits, protocol isolation and boot state");
}
