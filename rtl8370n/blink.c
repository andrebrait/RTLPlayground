/*
 * Proof-of-execution image for the TP-Link TL-SG108PE V5 (RTL8370N).
 * Runs under the TP-Link boot code, which jumps to CODE 0x1000 after
 * checking the image. Blinks all port LEDs at about 1 Hz; nothing else.
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
__sfr __at(0x8e) CKCON;

#define SFR_EXEC_WRITE_REG 3

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

/* Timer 0 at 62.5 MHz / 12 overflows every ~12.6 ms */
static void delay_ticks(uint8_t n)
{
	while (n--) {
		TF0 = 0;
		while (!TF0);
	}
}

#define LED_SYS_CONFIG		0x1b00
#define PARA_LED_IO_EN1		0x1b24
#define PARA_LED_IO_EN2		0x1b25
#define CPU_FORCE_LED0_CFG0	0x1b08	/* 2 bits per port: 2 = off, 3 = on */
#define CPU_FORCE_LED1_CFG0	0x1b0a
#define CPU_FORCE_LED2_CFG0	0x1b0c

static void leds_force(uint16_t mode)
{
	reg_write(CPU_FORCE_LED0_CFG0, mode);
	reg_write(CPU_FORCE_LED1_CFG0, mode);
	reg_write(CPU_FORCE_LED2_CFG0, mode);
}

void main(void)
{
	EA = 0;	/* boot code ISRs also use the register-access SFRs */

	/* Parallel LED setup, as in the TP-Link firmware's LED self-test */
	reg_write(PARA_LED_IO_EN1, 0xffff);
	reg_write(PARA_LED_IO_EN2, 0xffff);
	reg_write(LED_SYS_CONFIG, 0x1472);

	CKCON = 0;
	TMOD = (TMOD & 0xf0) | 0x01;
	TR0 = 1;

	while (1) {
		leds_force(0xffff);
		delay_ticks(40);
		leds_force(0xaaaa);
		delay_ticks(40);
	}
}
