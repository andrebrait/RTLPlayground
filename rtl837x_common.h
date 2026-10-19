#ifndef _RTL837X_STDIO_H_
#define _RTL837X_STDIO_H_

#include "uip/uip-conf.h"
#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

/* Timers of the main loop count system ticks, not loop passes: a pass is as
 * long as the packets it handles, and since #483 the loop no longer sleeps
 * while packets wait, so passes can outrun the tick as well as lag it. `last`
 * is the low byte of ticks when the timer last ran; the byte arithmetic wraps
 * every 256 ticks, so a pass longer than 1.28 s loses time rather than
 * running the timer 64 times to catch up. */
#define TICKS_DUE(last, period)	((uint8_t)((uint8_t)ticks - (last)) >= (period))

#define PORT_BIT(p)     ((uint16_t)1 << (p))

// Define Port-masks for 9-port devices and 6-port devices
#define PMASK_9		0x1ff
#define PMASK_6		0x1f8

// Defines a port mask for dropping all packets on Lookup-miss
#define LOOKUP_MISS_DROP_6  0x00015540
#define LOOKUP_MISS_DROP_9  0x00015555
#define LOOKUP_MISS_FLOOD   0x00000000

// Appended when a captured command wrote more than the output buffer holds,
// so a clipped listing is visibly clipped instead of silently short.
#define CMD_TRUNCATED "\n[output truncated]\n"

// For RX data, a propriatary RTL FRAME is inserted. Instead of 0x0800 for IPv4,
// the RTL_FRAME_TAG_ID is used as part of an 8-byte tag. When VLAN is activated,
// the VLAN tag is inserted after the RTL tag
// See here for the RTL tag: https://github.com/torvalds/linux/commit/1521d5adfc2b557e15f97283c8b7ad688c3ebc40
struct rtl_tag {
	uint16_t tag;		// This is 0x8899 for the RTL837X
	uint8_t version;	// Version is 4
	uint8_t reason;
	uint16_t flags;
	uint16_t pmask;		// A bit mask for a TX pkt, 4-bit port-number for RX
};

struct vlan_tag {
	uint16_t svlan;		// Service VLAN
	uint16_t vlan;
};

#define RTL_TAG_SIZE		(sizeof (struct rtl_tag))
#define VLAN_TAG_SIZE		(sizeof (struct vlan_tag))
#define RTL_FRAME_TAG_ID	0x8899
#define RTL_FRAME_TAG_VERSION	0x04
/* Bits of the tag's `flags` word, see doc/CpuPort.md. */
#define RTL_TAG_LEARN_DIS	0x0020	/* do not learn the source address from this frame */
#define RTL_TAG_KEEP		0x0080	/* keep the frame's 802.1Q tag format as injected */

// For TX, an 8 byte (plus 4 byte padding when when VLAN is enabled)
// header describing the frame to be moved to the Asic is used
#define RTL_FRAME_DESC_SIZE	12

// This is the standard size of an Ethernet frame header
#define ETHER_HEADER_SIZE	14

#define DEFAULT_CONFIG_START 0x6f000
#define CODE0_SIZE 0x4000
#define CODE_BANK_SIZE 0xc000

// Store update image after running image
#define FIRMWARE_UPLOAD_START 0x80000

extern __xdata uint8_t uip_buf[UIP_CONF_BUFFER_SIZE+2];

// Headers for calls in the common code area (HOME/BANK0)
void print_string_no_syslog(__code const char *p);
void print_string_newline_no_syslog(__code const char *p);
void print_sfr_data(void);
void print_phy_data(void);
void phy_write_mask(uint16_t phy_mask, uint8_t dev_id, uint16_t reg, uint16_t v);
void phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t v);
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg);
void phy_modify(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t mask, uint16_t set);
void reg_read(uint16_t reg_addr);
void reg_read_m(uint16_t reg_addr);
void reg_write(uint16_t reg_addr);
void reg_write_m(uint16_t reg_addr);
void sds_read(uint8_t sds_id, uint8_t page, uint8_t reg);
void sds_write_v(uint8_t sds_id, uint8_t page, uint8_t reg, uint16_t v);
void delay(uint16_t t);
void write_char_no_syslog(char c);
void print_reg(uint16_t reg);
void reg_bit_set(uint16_t reg_addr, char bit);
void reg_bit_clear(uint16_t reg_addr, char bit);
uint8_t reg_bit_test(uint16_t reg_addr, char bit);
void sfr_mask_data(uint8_t n, uint8_t mask, uint8_t set);
void sfr_set_zero(void);
void sfr_data_copy(__xdata uint8_t *dst);
void memcpyc(__xdata uint8_t *dst, __code const uint8_t *src, uint16_t len);
uint16_t strlen_x(__xdata const char *s);
bool strstart(__xdata const uint8_t *a, __code const uint8_t *b);
bool strstart_x(__xdata const uint8_t *a, __xdata const uint8_t *b);
void tcpip_output(void);
uint8_t read_flash(uint8_t bank, __code const uint8_t *addr);
void get_random_32(void);
void read_reg_timer(__xdata uint32_t * tmr);
bool gpio_pin_test(uint8_t pin);
void set_sys_led_state(uint8_t state);
void sds_read(uint8_t sds_id, uint8_t page, uint8_t reg);
void sds_write_v(uint8_t sds_id, uint8_t page, uint8_t reg, uint16_t v);
void flash_default_config(void);
#endif
