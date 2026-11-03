#include "machine_init_defs.h"
#include "rtl836x_regs.h"

extern __xdata uint8_t sfr_data[4];

void machine_custom_init(void) __banked
{
	/* Each port LED is bi-colour between LED1 (green) and LED2 (orange) */
	REG_SET(RTL836X_REG_LED_CONFIGURATION, 0x0960);	/* LED1 Spd1000/Act, LED2 Spd100(10)/Act */
	REG_SET(RTL836X_REG_CPU_FORCE_LED0_CFG0, 0);
	REG_SET(RTL836X_REG_CPU_FORCE_LED1_CFG0, 0);
	REG_SET(RTL836X_REG_CPU_FORCE_LED2_CFG0, 0);

	/*
	 * GPIO 1, 5, 8, 9 enable PoE on ports 1-4 (high = on), 7 is the PoE MCU line, 2 the PoE LEDs.
	 * Output value, then output enable, then GPIO mode, so no line glitches low; without the
	 * mode bits these pins are LED outputs and port 2's PoE enable follows a port LED.
	 */
	for (uint16_t r = RTL836X_REG_GPIO_67C_O_X0; r <= RTL836X_REG_GPIO_MODE_67C_X0; r += 4) {
		reg_read_m(r);
		sfr_mask_data(1, 0, 0x03);
		sfr_mask_data(0, 0, 0xa6);
		reg_write_m(r);
	}
}
