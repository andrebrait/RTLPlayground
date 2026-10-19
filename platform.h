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

// The shared code reads the startup configuration in pages of this size (a uint8_t index walks one)
#define CONFIG_PAGE_SIZE 0x100

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

/* One unicast entry of the L2 table (see l2_entry_next). idx is the table slot:
 * on a call it is where the search starts, on return the slot of the entry found. */
struct l2_entry {
	uint16_t idx;
	uint16_t vid;
	uint8_t mac[6];
	uint8_t port;		// logical port, 0..15
	bool is_static;
};

#define PORT_LAG_NONE 0xff

extern __code const struct machine machine;

extern __xdata struct vlan_settings vlan_settings;
extern __xdata struct l2_entry l2_buf;
extern __xdata char port_names[9][PORT_NAME_SIZE];
extern __xdata uint16_t lldp_logical_port_status;

extern __xdata bool lldp_enabled;
extern __xdata uint8_t lacpEnabled;
extern __xdata bool stp_enabled;

/* Spanning tree, link aggregation and LLDP state the three protocol pages read.
 * The 8051 protocol engines (rtl837x_stp.c, rtl837x_lacp.c, rtl837x_lldp.c) fill
 * it; a platform without them fills it in the *_status_refresh() hooks below. */

/* Tick rate of stp_timers(), also used by the web UI. */
#define STP_HZ 50

#define STP_PORTS	(CPU_PORT + 1)
#define STP_LAG_BASE	STP_PORTS
#define STP_LAG_COUNT	4
#define STP_ENTITIES	(STP_LAG_BASE + STP_LAG_COUNT)

/* Bridge identifier as carried in a BPDU (priority, extension, MAC). */
struct bridge {
	uint8_t prio;
	uint8_t ext;
	uint8_t mac[6];
};

extern __xdata uint8_t  stp_prio;
extern __xdata uint8_t  stp_hello_s;
extern __xdata uint8_t  stp_maxage_s;
extern __xdata uint8_t  stp_fwddelay_s;
extern __xdata uint8_t  stp_rstp;
extern __xdata uint8_t  stp_txhold;

/* Per-port config/status flags (stp_pflags[]) */
#define STP_PF_ENABLED	0x01	/* port participates in STP (default on)     */
#define STP_PF_ADMEDGE	0x02	/* admin edge: forwarding immediately        */
#define STP_PF_AUTOEDGE	0x04	/* auto edge: forward after 3 s without BPDU */
#define STP_PF_BPDUGUARD 0x08	/* disable port if a BPDU arrives            */
#define STP_PF_ROOTGUARD 0x10	/* never accept a better root on this port   */
#define STP_PF_FILTER	0x20	/* neither send nor accept BPDUs             */
#define STP_PF_OPEREDGE	0x40	/* runtime: port went forwarding as an edge  */
#define STP_PF_TRIPPED	0x80	/* runtime: disabled by BPDU guard           */

extern __xdata uint8_t  stp_pflags[STP_ENTITIES];
extern __xdata uint32_t stp_pcost[STP_ENTITIES];	/* path cost; 0 = auto (20000)      */
extern __xdata uint8_t  stp_pprio[STP_ENTITIES];
extern __xdata uint8_t  stp_pp2p[STP_ENTITIES];
extern __xdata uint8_t  stp_ent_of[STP_PORTS];	/* the STP entity a port answers to: itself, or its lag */
extern __xdata uint16_t stp_lag_mask[STP_LAG_COUNT];	/* member ports of each lag; 0 = not part of STP */

/* Last-heard designated info per port (from received BPDUs); consult
 * stp_bpdu_age to decide whether it is still current. */
extern __xdata struct bridge stp_dbridge[STP_ENTITIES];
extern __xdata uint16_t stp_dpid[STP_ENTITIES];
extern __xdata uint32_t stp_dcost[STP_ENTITIES];
extern __xdata uint16_t stp_bpdu_age[STP_ENTITIES];	/* ticks since a BPDU was heard */

extern __xdata struct bridge root_bridge;
extern __xdata uint32_t root_bridge_cost;
extern __xdata uint8_t  stp_root_port;
extern __xdata uint16_t stp_tc_count;

#define LACP_NUM_LAGS		4

extern __xdata uint8_t  lacp_actor_state[10];
extern __xdata uint8_t  lacp_partner_state[10];
extern __xdata uint8_t  lacp_rx_state[10];
extern __xdata uint8_t  lacp_partner_sys[10][MAC_ADDR_LEN];
extern __xdata uint16_t lacp_rx_count[10];
extern __xdata uint8_t  lacp_port_lag[10];
extern __xdata uint16_t lacp_lag_ports[LACP_NUM_LAGS];
extern __xdata uint8_t  lacp_agg_sys[LACP_NUM_LAGS][MAC_ADDR_LEN];
extern __xdata uint8_t  lacp_agg_valid[LACP_NUM_LAGS];
extern __xdata uint16_t lacp_members_last[LACP_NUM_LAGS];

/* 2-bit MSTP state of a logical port (3 = forwarding), or of the STP entity it answers to. */
uint8_t stp_port_state_get(uint8_t log_port) __banked;

/* Called first by send_stp/send_lacp/send_lldp. Nothing to do on the 8051, whose
 * protocol engines keep the globals above current; a platform without them
 * provides functions here instead. */
#ifndef PLATFORM_HAS_STATUS_REFRESH
#define stp_status_refresh()
#define lacp_status_refresh()
#define lldp_status_refresh()
#else
void stp_status_refresh(void);
void lacp_status_refresh(void);
void lldp_status_refresh(void);
#endif

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
/* SFP cage state. sfp_present() and sfp_los() report the snapshot handle_sfp() keeps in
 * sfp_pins_last, as the status page shows it. sfp_present_live() reads the detect pin
 * itself, as the `sfp` command does, so a module pulled since the last poll shows at once. */
bool sfp_present(uint8_t slot) __banked;
bool sfp_los(uint8_t slot) __banked;
bool sfp_present_live(uint8_t slot);

// Flash
void flash_read_uid(void);
void flash_read_jedecid(void);
void flash_read_security(void);
void flash_read_bulk(__xdata uint8_t *dst);
__code const char* get_flash_size_str(void);

// Chip temperature, raw in 1/128 C. sensor 0 = now, 1 = at power-on
int16_t chip_temp_get(uint8_t sensor) __banked;

/* The startup configuration: a NUL-terminated text in a flash area of CONFIG_LEN bytes.
 * config_read() copies up to len bytes from offset into buf and returns how many it
 * read: len, clamped to the end of the area. */
uint16_t config_read(uint16_t offset, __xdata uint8_t * __xdata buf, __xdata uint16_t len) __banked;
/* Queues len bytes of the area from offset on for the web server to stream after the
 * packet being built (what did not fit the TCP buffer). */
void config_stream_rest(uint16_t offset, __xdata uint16_t len) __banked;

// Ports and VLANs
#define STAT_COUNTER_TX_PKTS	46
#define STAT_COUNTER_RX_PKTS	47
#define STAT_COUNTER_ERR_PKTS	48

// 0 = link down, else the RTL837x speed code + 1 (what app.js reads)
uint8_t port_link_get(uint8_t port) __banked;
bool port_phy_enabled(uint8_t port) __banked;
// bit 5..0 = 2500F, 1000F, 100F, 100H, 10F, 10H
uint8_t port_phy_adv_get(uint8_t port) __banked;
/* Getters that return more than a byte leave the result in port_buf, and every
 * argument after the first is __xdata. Both keep the banked call's extra parameters
 * and out-pointers out of internal data RAM, where each byte comes off the stack
 * headroom. Follow this pattern for new platform getters. */
extern __xdata uint8_t port_buf[24];
// 64-bit MIB counter -> port_buf[0..7], big-endian
void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked;
// MACs learned -> port_buf[0..3], big-endian
void port_macs_learned_get(uint8_t port) __banked;
uint16_t port_mtu_get(uint8_t port) __banked;
void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked;
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
// port_buf[0] = enabled, [1] = monitor port (logical), [2..3] = RX mask, [4..5] = TX mask, big-endian
void port_mirror_get(void) __banked;
bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked;
uint16_t port_lag_members_get(uint8_t lag) __banked;
uint8_t port_lag_of(uint8_t port) __banked;
void port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked;
void port_lag_hash_set(__xdata uint8_t lag, __xdata uint8_t hash) __banked;
// the 32-bit hash control word -> port_buf[0..3], big-endian
void port_lag_hash_get(uint8_t lag) __banked;
void port_eee_enable_all(__xdata uint8_t speed) __banked;
void port_eee_disable_all(void) __banked;
void port_eee_status_all(void) __banked;
void port_eee_enable(__xdata uint8_t port, __xdata uint8_t speed) __banked;
void port_eee_disable(uint8_t port) __banked;
void port_eee_status(uint8_t port) __banked;
void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked;
// port_buf[0] = limited, [1..3] = 20-bit rate, big-endian, [4] = flow control (ingress only)
void bandwidth_get(uint8_t port, __xdata uint8_t egress) __banked;
// per storm type t < STORM_TYPES: port_buf[t] = enabled, port_buf[4 + t] = pps (else kbps),
// port_buf[8 + 4t + 1..3] = 24-bit rate, big-endian
void storm_get(uint8_t port) __banked;
void port_isolate(uint8_t port, __xdata uint16_t pmask) __banked;
uint16_t port_isolation_get(uint8_t port) __banked;
/* VLAN table entry of a VLAN -> port_buf[0..3] as the 32-bit big-endian word the table
 * holds: bit 25 = entry valid (port_buf[0] & 0x02), bits 19..10 = ports that
 * egress untagged, bits 9..0 = member ports (the page's "members" is the word in hex).
 * Returns the valid bit. VLAN 4095 is not a table entry: port_buf then repeats the
 * last register read and the return value is not meaningful. */
bool vlan_entry_get(uint16_t vlan) __banked;
/* Reads the first unicast L2 entry at or after slot l2_buf.idx (the search wraps at the
 * end of the table) and sets l2_buf.idx to that slot. Returns true and fills the rest
 * of l2_buf for a valid entry; false when the slot is empty, then only l2_buf.idx is
 * meaningful. */
bool l2_entry_next(void) __banked;
/* Deletes the unicast entry found at or after slot idx.
 * 1 = deleted, 0 = nothing deleted (no entry, or the switch's own MAC, which is kept). */
uint8_t l2_entry_delete(uint16_t idx) __banked;
void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked;

/* Console commands that only this platform has (raw registers, flash IDs, chip reset...).
 * cmd_parser() calls it with the tokenized line (cmd_buffer, cmd_words_b, cmd_words_len) for
 * every word it does not handle itself; true = the line was one of the platform's commands
 * and has been run, false = unknown, cmd_parser() then reports it. */
bool platform_cmd(void) __banked;

#endif
