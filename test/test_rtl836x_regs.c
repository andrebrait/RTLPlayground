/*
 * RTL836x register primitives (rtl836x_regs.c) against a model of the RTL8370N
 * register controller: 16-bit registers, write data in SFR a4:a5, read data
 * in a6:a7, and a status that reads idle until the command has settled.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t mock_status(void);
static void mock_settle(void);
#define __asm__(text) mock_settle()

static uint16_t SFR_REG_ADDR_U16;
static uint8_t SFR_EXEC_GO, SFR_DATA_24, SFR_DATA_16, SFR_DATA_8, SFR_DATA_0;
#define SFR_DATA_U16 ((uint16_t)(SFR_DATA_8 << 8 | SFR_DATA_0))
#define SFR_EXEC_STATUS mock_status()
#define SFR_EXEC_READ_REG 1
#define SFR_EXEC_WRITE_REG 3

uint8_t sfr_data[4];

#include "../rtl836x_regs.c"

static uint16_t registers[65536];
static uint8_t written[4];	/* SFR a4..a7 when the last write executed */
static int settled;

static void mock_settle(void)
{
	settled = 1;
}

/* Until the command has settled the status reads idle and nothing happened yet */
static uint8_t mock_status(void)
{
	if (!settled)
		return 0;
	if (SFR_EXEC_GO == SFR_EXEC_READ_REG) {
		SFR_DATA_8 = registers[SFR_REG_ADDR_U16] >> 8;
		SFR_DATA_0 = registers[SFR_REG_ADDR_U16];
	} else if (SFR_EXEC_GO == SFR_EXEC_WRITE_REG) {
		registers[SFR_REG_ADDR_U16] = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
		written[0] = SFR_DATA_24;
		written[1] = SFR_DATA_16;
		written[2] = SFR_DATA_8;
		written[3] = SFR_DATA_0;
	}
	SFR_EXEC_GO = 0;
	settled = 0;
	return 0;
}

int main(void)
{
	printf("[test] REG_SET puts the value in a4:a5 and zeroes a6:a7\n");
	SFR_DATA_8 = SFR_DATA_0 = 0x55;
	REG_SET(0x1d1d, 0x1234);
	assert(registers[0x1d1d] == 0x1234);
	assert(written[0] == 0x12 && written[1] == 0x34 && written[2] == 0 && written[3] == 0);

	printf("[test] reg_read_m maps a6:a7 to sfr_data[2..3], [0..1] = 0\n");
	registers[0x1300] = 0x6368;
	SFR_DATA_24 = 0xde;	/* a4:a5 hold the last write data, not read data */
	SFR_DATA_16 = 0xad;
	sfr_data[0] = sfr_data[1] = 0xff;
	reg_read_m(0x1300);
	assert(sfr_data[0] == 0 && sfr_data[1] == 0 && sfr_data[2] == 0x63 && sfr_data[3] == 0x68);

	printf("[test] reg_read leaves the value in a6:a7\n");
	registers[0x1a04] = 0x0102;
	reg_read(0x1a04);
	assert(SFR_DATA_8 == 0x01 && SFR_DATA_0 == 0x02);

	printf("[test] reg_write_m writes sfr_data[2..3] and ignores [0..1]\n");
	sfr_data[0] = 0xaa;
	sfr_data[1] = 0xbb;
	sfr_data[2] = 0x09;
	sfr_data[3] = 0x60;
	reg_write_m(0x1b03);
	assert(registers[0x1b03] == 0x0960);
	assert(written[0] == 0x09 && written[1] == 0x60 && written[2] == 0 && written[3] == 0);

	printf("[test] PHY access: OCP bits 15:10 in GPHY_OCP_MSB_0, the rest in the 0x2000 window\n");
	registers[0x1d15] = 0xab3f;	/* bits above the MSB field survive */
	phy_write(3, 0x1f, 0xa4b4, 0x0210);
	assert(registers[0x1d15] == (0xab00 | 0xa4b4 >> 10));
	assert(registers[0x2000 | 0x2 << 8 | 3 << 5 | 0x1a] == 0x0210);

	printf("[test] PHY access: MII register r is OCP 0xa400 + 2r, the result is in a6:a7\n");
	registers[0x1d15] = 0;
	registers[0x2000 | 2 << 5 | 4] = 0x0de1;
	phy_read(2, 0, 4);
	assert(registers[0x1d15] == 0x29 && SFR_DATA_U16 == 0x0de1);

	printf("[test] phy_modify clears the mask and sets the bits in the same register\n");
	registers[0x2000 | 5 << 5] = 0x1940;	/* BMCR: power down, autonegotiation */
	phy_modify(5, 0, 0xa400, 0x0800, 0x0200);
	assert(registers[0x2000 | 5 << 5] == 0x1340);

	printf("[test] phy_write_mask writes every PHY in the mask\n");
	phy_write_mask(0x81, 0, 9, 0x0200);
	assert(registers[0x2000 | 0 << 5 | 9] == 0x0200 && registers[0x2000 | 7 << 5 | 9] == 0x0200);
	assert(registers[0x2000 | 1 << 5 | 9] == 0);

	printf("PASS\n");
	return 0;
}
