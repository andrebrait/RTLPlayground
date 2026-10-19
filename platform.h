#ifndef _PLATFORM_H_
#define _PLATFORM_H_

/*
 * The hardware-facing interface of the shared CLI and web-page code
 * (cmd_parser.c, cmd_editor.c, httpd/page_impl.c).
 *
 * Everything those files need from the platform is declared here, and nothing
 * register-level is: register addresses, SFR declarations and the
 * reg_*()/phy_*()/sds_*() accessors stay in rtl837x_regs.h, rtl837x_sfr.h and
 * rtl837x_common.h. On the 8051 each function below is implemented by the
 * RTL837x firmware modules and bound at link time; a host build binds the same
 * names to a mock (test/platform_mock.c). `make -C test seam_check` compiles
 * the three shared files with the register headers poisoned to prove they
 * reach the hardware only through this header.
 *
 * rtl837x_common.h, rtl837x_port.h, sfp.h and rtl837x_flash.h include this
 * header, so every 8051 translation unit sees these declarations once.
 */

#include <stdint.h>
#include <stdbool.h>
#include "machine.h"

#define SYS_TICK_HZ 200

#define CPU_PORT        9
#define MAC_ADDR_LEN    6
#define NUL			'\0'
#define PMASK_CPU	((uint16_t)(1 << CPU_PORT))

/* Buffer for serial input, SBUF_SIZE must be power of 2 < 256
 * Writing to this buffer is under the sole control of the serial ISR
 * Note that key-presses such as <cursor-left> can create multiple
 * keys (3 to 4) being sent via the serial line, so this must be
 * sufficiently large */
#define SBUF_SIZE 16
#define SBUF_MASK (SBUF_SIZE - 1)

extern __xdata volatile uint8_t sbuf_ptr;
extern __xdata uint8_t sbuf[SBUF_SIZE];

// Define the command buffer size, Must be 2^x and <= 128
#define CMD_BUF_SIZE 128

// Size of the TCP Output buffer
#define TCP_OUTBUF_SIZE 2500

// Size of the port name, including the terminating null byte
#define PORT_NAME_SIZE 32

// Size of the memory area dedicated to VLAN-names
#define VLAN_NAMES_SIZE 1024

// Size of the flash buffer used for writing to flash, must be a multiple of the flash page size (0x100)
#define FLASH_BUF_SIZE 512

// Errors for commands
#define ERR_OK			0
#define ERR_TOO_MANY_ARGUMENTS	1
#define ERR_CMD_TOO_LONG	2
#define ERR_INVALID_ARGUMENT	3

#define CONFIG_START 0x70000
#define CONFIG_LEN 0x1000

// Constants for the circular command buffer, the size must be 2^n
#define CMD_HISTORY_SIZE 0x800
#define CMD_HISTORY_MASK (CMD_HISTORY_SIZE - 1)

enum sfp_speeds {
	SFP_SPEED_AUTO = 0,
	SFP_SPEED_100M,
	SFP_SPEED_1G,
	SFP_SPEED_2G5,
	SFP_SPEED_5G,
	SFP_SPEED_10G
};

/**
 * Representation of a 48-bit Ethernet address.
 */
struct uip_eth_addr {
   uint8_t addr[6];
};

struct flash_region_t {
    uint32_t addr;
    uint16_t len;
};

/*
 * Port EEE settings
 */
#define EEE_100 	0x01
#define EEE_1000	0x04
#define EEE_2G5		0x10
#define EEE_5G		0x20
#define EEE_10G		0x40
#define EEE_NORESET	0x80

// Possible values for ingress filter type. The values are the hardware
// encoding; rtl837x_port.h checks them against INGR_ALLOW_* in rtl837x_regs.h.
typedef enum {
	VLAN_ALL = 0,
	VLAN_TAGGED = 1,
	VLAN_UNTAGGED = 2,
	VLAN_INVALID = 3
} vlan_ingress_mode_e;

// Since we lack a way to force enum to be 1 byte,
// This is a typedef for the VLAN ingress filter type
// which holds vlan_ingress_mode_e values
typedef uint8_t vlan_ingress_mode_t;

struct vlan_settings {
	uint16_t vlan;
	uint16_t members;
	uint16_t tagged;
};

#define PORT_LAG_NONE 0xff

extern __code const struct machine machine;

extern __xdata struct vlan_settings vlan_settings;
extern __xdata char port_names[9][PORT_NAME_SIZE];
extern __xdata uint16_t lldp_logical_port_status;

extern __xdata bool stp_enabled;

/* System hostname (device identity). Set via `hostname <text>` and the System
 * Settings page, reported in /information.json. Other modules (e.g. LLDP, which
 * advertises it as the System Name TLV) read it from here. */
extern __xdata char hostname[24];

extern __xdata struct uip_eth_addr uip_ethaddr;

extern __xdata uint8_t sfp_buf[16];
extern __xdata uint8_t sfp_pins_last;
extern __xdata char sfp_module_vendor[2][17];
extern __xdata char sfp_module_model[2][17];
extern __xdata char sfp_module_serial[2][17];
extern __xdata uint8_t sfp_options[2];
extern __xdata uint8_t sfp_speed[2];

// Console output and text helpers (HOME/BANK0)
void print_string(__code const char *p);
void print_string_x(__xdata char *p);
void print_long(uint32_t a);
void print_short(uint16_t a);
void print_byte(uint8_t a);
void itoa(uint8_t v);
void itoa_short(uint16_t v);
void print_cmd_prompt(void);
void print_phys_port(uint8_t port);
void write_char(char c);
uint16_t strtox(__xdata uint8_t *dst, __code const char *s);
void reset_chip(void);
/* Firmware implementations that shadow libc names. Host unit-test builds
 * (RTLP_HOST_TEST) hide these prototypes so they don't clash with glibc;
 * argument order matches libc, so on-host callers transparently use the
 * C library. See test/. */
#ifndef RTLP_HOST_TEST
void sleep(uint16_t t);
void memcpy(__xdata void * __xdata dst, __xdata const void * __xdata src, uint16_t len);
void memset(__xdata uint8_t *dst, __xdata uint8_t v, uint8_t len);
int memcmp(__xdata const void *a, __xdata const void *b, uint16_t len);
uint16_t strlen(__code const char *s);
uint16_t strcpy(__xdata uint8_t *dst, const char *s);
char strcmp(__xdata const uint8_t *a, __code const uint8_t *b);
#endif

// SFP modules
bool sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len) __banked __reentrant;
bool sfp_print_info(uint8_t sfp) __banked;
void handle_sfp(void) __banked;

// Flash
void flash_read_uid(void);
void flash_read_jedecid(void);
void flash_read_security(void);
void flash_read_bulk(__xdata uint8_t *dst);
__code const char* get_flash_size_str(void);

// Ports and VLANs
#define STAT_COUNTER_TX_PKTS	46
#define STAT_COUNTER_RX_PKTS	47
#define STAT_COUNTER_ERR_PKTS	48

// 0 = link down, else the RTL837x speed code + 1 (what app.js reads)
uint8_t port_link_get(uint8_t port) __banked;
bool port_phy_enabled(uint8_t port) __banked;
// bit 5..0 = 2500F, 1000F, 100F, 100H, 10F, 10H
uint8_t port_phy_adv_get(uint8_t port) __banked;
// 8 bytes, big-endian, of the 64-bit MIB counter
void port_counter_get(uint8_t port, uint8_t counter, __xdata uint8_t *out8) __banked;
// 4 bytes, big-endian
void port_macs_learned_get(uint8_t port, __xdata uint8_t *out4) __banked;
uint16_t port_mtu_get(uint8_t port) __banked;
void port_mtu_set(uint8_t port, uint16_t mtu) __banked;
// bit 2..0 = 2.5G, 1G, 100M
uint8_t port_eee_adv_get(uint8_t port) __banked;
uint8_t port_eee_lp_get(uint8_t port) __banked;
bool port_eee_active(uint8_t port) __banked;
uint8_t port_l2_forget(void) __banked;
void port_l2_learned(void) __banked;
void port_stats_print(void) __banked;
void port_media_show(uint8_t port) __banked;
int8_t vlan_get(uint16_t vlan) __banked;
__xdata uint16_t vlan_name(uint16_t vlan) __banked;
void vlan_name_remove(uint16_t vlan) __banked;
void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked;
uint16_t port_pvid_get(uint8_t port) __banked;
void vlan_create(void) __banked;
void vlan_delete(uint16_t vlan) __banked;
void vlan_dump(void) __banked;
void port_mirror_set(uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked;
void port_mirror_del(void) __banked;
bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked;
uint16_t port_lag_members_get(uint8_t lag) __banked;
uint8_t port_lag_of(uint8_t port) __banked;
void port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked;
void port_lag_hash_set(__xdata uint8_t lag, __xdata uint8_t hash) __banked;
void port_eee_enable_all(__xdata uint8_t speed) __banked;
void port_eee_disable_all(void) __banked;
void port_eee_status_all(void) __banked;
void port_eee_enable(__xdata uint8_t port, __xdata uint8_t speed) __banked;
void port_eee_disable(uint8_t port) __banked;
void port_eee_status(uint8_t port) __banked;
void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked;
void port_isolate(uint8_t port, __xdata uint16_t pmask) __banked;
uint16_t port_isolation_get(uint8_t port) __banked;
void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked;

#endif
