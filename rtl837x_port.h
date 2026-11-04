#ifndef _RTL837X_PORT_H_
#define _RTL837X_PORT_H_

#include <stdint.h>
#include "rtl837x_regs.h"

#define STAT_COUNTER_TX_PKTS	46
#define STAT_COUNTER_RX_PKTS	47
#define STAT_COUNTER_ERR_PKTS	48

#define STAT_GET(cnt, port) do { \
	REG_WRITE(RTL837X_STAT_GET, 0x00, 0x00, cnt >> 3, (cnt << 5) | (port << 1) | 1); \
	do { \
		reg_read_m(RTL837X_STAT_GET); \
	} while (sfr_data[3] & 0x1); \
	} while (0)

// Possible values for ingress filter type
typedef enum {
	VLAN_ALL = INGR_ALLOW_ALL,
	VLAN_TAGGED = INGR_ALLOW_TAGGED,
	VLAN_UNTAGGED = INGR_ALLOW_UNTAGGED,
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

/* 
 * Port EEE settings
 */
#define EEE_100 	0x01
#define EEE_1000	0x04
#define EEE_2G5		0x10
#define EEE_5G		0x20
#define EEE_10G		0x40
#define EEE_NORESET	0x80

extern __xdata struct vlan_settings vlan_settings;

uint8_t port_l2_forget(void) __banked;
void port_l2_learned(void) __banked;
void port_stats_print(void) __banked;
void port_media_show(uint8_t port) __banked;
int8_t vlan_get(uint16_t vlan) __banked;
__xdata uint16_t vlan_name(uint16_t vlan) __banked;
void vlan_name_remove(uint16_t vlan) __banked;
void vlan_setup(void) __banked;
void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked;
uint16_t port_pvid_get(uint8_t port) __banked;
void port_l2mc_set(uint8_t mac_last, __xdata uint16_t vid, __xdata uint16_t pmask) __banked;
void port_l2_forget_port(uint8_t port) __banked;
void vlan_create(void) __banked;
void vlan_delete(uint16_t vlan) __banked;
void vlan_dump(void) __banked;
struct port_mirror {
	uint8_t enabled;
	uint8_t port;
	uint16_t rx_pmask;
	uint16_t tx_pmask;
};
bool port_mirror_set(__xdata uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked;
void port_mirror_get(__xdata struct port_mirror * __xdata m) __banked;
void port_mirror_del(void) __banked;
bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked;
void port_l2_setup(void) __banked;
/* The RTL8370N has 2 fixed groups (group 0 on ports 0-3, group 1 on 4-7) and one hash for both */
#if SOC_FAMILY == RTL836X
#define PORT_LAG_COUNT 2
#else
#define PORT_LAG_COUNT 4
#endif
uint16_t port_lag_members_get(uint8_t lag) __banked;
#define PORT_LAG_NONE 0xff
uint8_t port_lag_of(uint8_t port) __banked;
/* Whether the group exists and the hardware can take these members; writes nothing, so a caller can refuse before it changes any state */
bool port_lag_members_valid(__xdata uint8_t lag, __xdata uint16_t members) __banked;
/* false, with nothing written, for a members mask the hardware cannot take */
bool port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked;
bool port_lag_hash_set(__xdata uint8_t lag, __xdata uint8_t hash) __banked;
uint8_t port_lag_hash_get(__xdata uint8_t lag) __banked;
void port_eee_enable_all(__xdata uint8_t speed) __banked;
void port_eee_disable_all(void) __banked;
void port_eee_status_all(void) __banked;
void port_eee_enable(__xdata uint8_t port, __xdata uint8_t speed) __banked;
void port_eee_disable(uint8_t port) __banked;
void port_eee_status(uint8_t port) __banked;
void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked;
bool port_ingress_vlan_filter_set(uint8_t port, __xdata bool enabled) __banked;
bool port_ingress_vlan_filter_get(uint8_t port) __banked;
vlan_ingress_mode_t port_ingress_filter_get(__xdata uint8_t port) __banked;
void port_isolate(uint8_t port, __xdata uint16_t pmask) __banked;
uint16_t port_isolation_get(uint8_t port) __banked;
uint16_t port_mtu_get(uint8_t port) __banked;
bool port_mtu_set(uint8_t port, __xdata uint16_t len) __banked;
void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked;

#endif
