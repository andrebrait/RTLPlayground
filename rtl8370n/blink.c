/*
 * Proof-of-execution image for the TP-Link TL-SG108PE V5 (RTL8370N).
 * Runs under the TP-Link boot code, which jumps to CODE 0x1000 after
 * checking the image. Replays the TP-Link firmware's LED test, then leaves
 * the port LEDs to the hardware (green: 1000M link/act, orange: 10/100M).
 *
 * Build: see mkimage.py.
 */
#include <8051.h>
#include <stdint.h>

/* Switch register access, same protocol as the TP-Link boot code (CODE 0x0f29) */
__sfr __at(0xa0) SFR_EXEC_GO;
__sfr __at(0xa1) SFR_EXEC_STATUS;
__sfr __at(0xa2) SFR_REG_ADDRH;
__sfr __at(0xa3) SFR_REG_ADDRL;
__sfr __at(0xa4) SFR_DATA_H;
__sfr __at(0xa5) SFR_DATA_L;
__sfr __at(0xa6) SFR_RDATA_H;
__sfr __at(0xa7) SFR_RDATA_L;
__sfr __at(0x8e) CKCON;

#define SFR_EXEC_READ_REG 1
#define SFR_EXEC_WRITE_REG 3

/* Boot code CODE 0x0ec5 */
static uint16_t reg_read(uint16_t reg)
{
	SFR_REG_ADDRH = reg >> 8;
	SFR_REG_ADDRL = reg;
	SFR_EXEC_GO = SFR_EXEC_READ_REG;
	do {
		__asm__("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
	} while (SFR_EXEC_STATUS);
	return (SFR_RDATA_H << 8) | SFR_RDATA_L;
}

static void reg_write(uint16_t reg, uint16_t val)
{
	SFR_REG_ADDRH = reg >> 8;
	SFR_REG_ADDRL = reg;
	SFR_DATA_H = val >> 8;
	SFR_DATA_L = val;
	SFR_EXEC_GO = SFR_EXEC_WRITE_REG;
	do {
		__asm__("nop\n\tnop\n\tnop");
	} while (SFR_EXEC_STATUS);
}

#define GPIO_O_X0	0x1d1d	/* output value, GPIO 0-15 */
#define GPIO_OE_X0	0x1d21	/* output enable */

static void reg_bit_set(uint16_t reg, uint8_t bit)
{
	reg_write(reg, reg_read(reg) | (1 << bit));
}

/* Measured from a 240 fps video: one Timer 0 overflow every ~43 us, so ~23 per ms */
#define TICKS_PER_MS 23

static void delay_ms(uint16_t ms)
{
	while (ms--) {
		for (uint8_t n = TICKS_PER_MS; n; n--) {
			TF0 = 0;
			while (!TF0);
		}
	}
}

#define LED_SYS_CONFIG		0x1b00
#define LED_CONFIGURATION	0x1b03
#define PARA_LED_IO_EN1		0x1b24
#define PARA_LED_IO_EN2		0x1b25
#define CPU_FORCE_LED0_CFG0	0x1b08	/* 2 bits per port: 2 = off, 3 = on */
#define CPU_FORCE_LED1_CFG0	0x1b0a
#define CPU_FORCE_LED2_CFG0	0x1b0c

static void leds_force(uint16_t g0, uint16_t g1, uint16_t g2)
{
	reg_write(CPU_FORCE_LED0_CFG0, g0);
	reg_write(CPU_FORCE_LED1_CFG0, g1);
	reg_write(CPU_FORCE_LED2_CFG0, g2);
}

#define ON	0xffff
#define OFF	0xaaaa

void main(void)
{
	EA = 0;	/* boot code ISRs also use the register-access SFRs */

	CKCON = 0;
	TMOD = (TMOD & 0xf0) | 0x01;
	TR0 = 1;

	/* TP-Link LED test (bank 11, b11_bff7), with 2 s per step */
	reg_write(LED_SYS_CONFIG, 0x54f2);
	reg_write(LED_CONFIGURATION, 0x0096);
	/* b12_6307(0, 7), b12_6360(1, 7): GPIO 7 (pin 72) output, high; boot code left it an input */
	reg_bit_set(GPIO_OE_X0, 7);
	reg_bit_set(GPIO_O_X0, 7);
	reg_write(PARA_LED_IO_EN1, 0xffff);
	reg_write(PARA_LED_IO_EN2, 0xffff);
	reg_write(LED_SYS_CONFIG, 0x1472);
	reg_write(CPU_FORCE_LED0_CFG0, ON);
	reg_write(CPU_FORCE_LED1_CFG0, ON);	/* expected green */
	delay_ms(2000);
	reg_write(CPU_FORCE_LED1_CFG0, OFF);
	reg_write(CPU_FORCE_LED2_CFG0, ON);	/* expected orange */
	delay_ms(2000);
	leds_force(OFF, OFF, OFF);
	delay_ms(2000);

	/* TP-Link normal operation: LED0 off, LED1 Spd1000/Act, LED2 Spd100(10)/Act */
	reg_write(LED_CONFIGURATION, 0x0960);
	leds_force(0, 0, 0);
	while (1);
}
