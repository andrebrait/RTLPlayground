#RTL8372/3 features

The following hardware features of the RTL8372/3 is supported:
- Clock generation, including different divider settings
- Interrupt control for timer, serial, external irqs 0, 1
- Serial console via SFRs
- Flash operations via SFRs
- Bank switching via SFRs
- Access to Switch registers via SFRs
  - LED setup
  - Reset
  - Some switch settings such as MAC configuration
  - GPIO to detect SFP module insert/removal/RX-LOS (depending on device/module support)
  - I2C to read SFP EEPROM on 1 and 2 SFP slot devices
  - NIC setup
  - L2 learning table access, L2 table flushing
  - VLAN setup/configuration
  - Port mirroring
- Access to PHYs via MDIO (clause 45 via SFR):
  - Internal PHYs of RTL8372 and RTL8373
  - RTL8221 (1x2.5GBit port on devices with 5 ports)
  - RTL8224 (4x2.5GBit ports on devices with 8 ports)
- SerDes settings of SoC via SFR:
  - Configure SFPs with 10Gbit/2.5Gbit/1Gbit (Ethernet and Fiber SFP(+) tested)
  - RTL8221, RTL8224
- NIC TX and RX of packets via SFRs
  - send and receive Ethernet frames via SFRs and Switch registers
  - RTL-tags and VLAN ingress-tag decoding for CPU-port

Ethernet frame RX IRQ via IRQ1 is conceptually understood, but not activated. RX is
currently done via polling, which allows ping-times of <10ms.

The RTL8372/3 have 256 bytes of internal RAM (INTMEM) accessible through MOV
instructions, which are used for the stack and important globals. Some of
these are bit-adressable, e.g. for storing global flags.

Additionally, 64kB of extended RAM (XMEM) is built in, which is accessed
through the MOVX instruction. It is used for global variables, for most
of the function argument passing that is not done using the 8 registers
R0-R7 or registers A/B, and for local variables (which requires extremely
careful planning). The flash memory is transparently accessible for code
being executed and can be used to store configuration. Access is done through
the MOVC instruction, possibly setting the bank register before and
resetting it to access the entire 4MB space. Code is prefetched from flash
and cached in a small RAM automatically by the HW.

The peripheral functions are accessed through 2 different mechanisms:
- Special Function Registers (SFRs, 0x80-0xff) for banking, timers, UART, access to
  switch registers, MDIO, SPI (flash) and NIC transfers. Some SFRs are not
  used for HW purposes and can be used as RAM. Some SFRs are bit-adressable,
  allowing for very tight event wait loops (a single 2-byte instruction).
- 0x10000 switch registers, which appear to be very similar to the registers
  of the RTL838x, for which source code and datasheets are available. This
  controls clock dividers, GPIO/LEDs and general  switch functionality. 

The playground image shows access to the different types of memory using the
SDCC compiler. Any support of Linux or e.g. Zephyr would require porting gcc.
There are FreeRTOS ports to 8051 processors using sdcc, however.

## RTL836x (TL-SG108PE V5): temperature

No readable temperature interface has been verified for this board's RTL8370N
(chip ID `0x6368`). The
[RTL8367C SDK register map](https://github.com/shiroichiheisen/Realtek-Unmanaged-Switch-Arduino-Library/blob/ccd863d61a90e90f096fbc77e3959eadce914213/API_Source/rtl8367c_reg.h)
names high/low thermal interrupt status and mask bits at `0x112e` and `0x112f`,
but those are not a temperature value or a Celsius conversion. The inspected
SDK switch API provides no temperature-reading function.

The RTL837x sensor addresses must not be reused: `0x0338`, `0x0340` and `0x0350`
are queue-weight registers in the native SDK map. The
[RTL8370NI-VB datasheet](http://kstmicro.com/upload/wp/2024/08/2208301730_Realtek-Semicon-RTL8370NI-VB-CG_C5155990.pdf)
lists temperature ratings and package thermal characteristics, not a sensor
programming interface.

On build `79d1f3ac`, a read-only HTTP check confirmed `chip_temp: "n/a"` in
`/information.json` and no SoC temperature samples in `/metrics`. No sensor,
PHY or switch-register writes were made for this investigation.

This does not establish that the silicon lacks a sensor. Implementing a
reading requires an exact-chip value register, any enable/validity sequence,
and a documented conversion or independently verified calibration. Until
those are known, the UI remains unavailable and no temperature is fabricated.

### Related-chip research

The newer RTL8367 common SDK V1.4.2 contains both C- and D-family maps in
[the same pinned source tree](https://github.com/jameywine/realtek-doc/tree/e17db159796ec389651516d68351d166988978da/sources/uboot-be550/drivers/net/rtl8367_common_V1_4_2/dal).
The complete C map still names the thermal interrupt bits but no temperature
output. The D map does define a thermal block, including power/reset controls,
a sample-toggle bit and sensor-output bits 18:16. Its addresses conflict
with unrelated functions in the native C-family map:

| Address | RTL8367D | RTL8367C/native map |
| --- | --- | --- |
| `0x1d90` | Thermal power/reset controls | Master I²C system control and pin mux |
| `0x1da4` | Sensor output bits 18:16 and sample toggle | Packet-buffer SRAM data output 0 |
| `0x1da5` | Thermal status register 1; fields not named in this map | Packet-buffer SRAM data output 1 |

These definitions are in
[rtl8367d_reg.h](https://github.com/jameywine/realtek-doc/blob/e17db159796ec389651516d68351d166988978da/sources/uboot-be550/drivers/net/rtl8367_common_V1_4_2/dal/rtl8367d/rtl8367d_reg.h)
and
[rtl8367c_reg.h](https://github.com/jameywine/realtek-doc/blob/e17db159796ec389651516d68351d166988978da/sources/uboot-be550/drivers/net/rtl8367_common_V1_4_2/dal/rtl8367c/rtl8367c_reg.h).
No D-family enable sequence should be applied to this board.

[Linux's RTL822x PHY hwmon driver](https://github.com/torvalds/linux/blob/05441d697f7f9a927264f4058cbbc6a03dfcac9d/drivers/net/phy/realtek/realtek_hwmon.c)
reads MMD vendor-2 register `0xbd84` as a signed 10-bit value in half-degrees.
That is a PHY-specific interface, not evidence that the RTL8370N's internal
Gigabit PHYs implement it.
[OpenWrt's thermal driver](https://github.com/openwrt/openwrt/blob/a3ffc9af4e3ff9d9846af4e809306c1850f0cda9/target/linux/realtek/files-6.18/drivers/thermal/realtek-thermal.c)
supports RTL8380, RTL8390, RTL9300 and RTL9607 through separate MMIO register
layouts and conversion paths; none supplies an RTL8370N mapping.

The RTL836x programming guide Rev. 1.3.12 and the RTL8367RB/MB register table
Rev. 1.0.0 were also checked; no temperature-reading interface was found.
Both are available in the
[pinned datasheet collection](https://github.com/jameywine/realtek-doc/tree/e17db159796ec389651516d68351d166988978da/datasheets).
This expanded search did not establish a safe native read path. No device
registers were read or written during it.

### Read-only probe

With approval, build `79d1f3ac` on a TL-SG108PE V5 read the RTL8367D thermal
addresses with `regget` only; nothing was written. `0x1d90`–`0x1da5` and
`0x1109` responded, `0x1da6`–`0x1dad` read zero, and no value behaved like a
temperature over 40 samples in 10 minutes: `0x1d92`, `0x1d96`–`0x1d9a`,
`0x1d9d` and `0x1d9e` never changed by a single count, while `0x1109`,
`0x1d9f` and `0x1da4` (SRAM output) jumped across most of their range between
samples. Two reads timed out (`0x1d9a` and `0x1d9d`) without a reset; avoid
repeating such scans on a production switch.

A photo of the top side of the board shows no separate temperature sensor. The
8-pin device beside the switch chip is presumably the SPI flash, and the other
ICs sit in the power and PoE circuits; their markings were not readable, and
the bottom side was not inspected. The stock firmware contains no temperature
strings. The temperature therefore stays unavailable on this board.
