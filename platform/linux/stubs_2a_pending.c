/*
 * What the shared code links against that milestone 2a still has to implement. Every entry
 * reports its feature as absent: getters return the off or empty state, commands say so.
 * The task that implements an entry deletes it from here.
 */
#include <string.h>

#include "cmd_parser.h"
#include "platform.h"
#include "rtl837x_flash.h"
#include "dhcp.h"

#define UNSUPPORTED() cmd_error("Not supported on this platform yet\n")

/* Configuration store: the startup configuration is empty. */
uint16_t config_read(uint16_t offset, __xdata uint8_t * __xdata buf, __xdata uint16_t len) __banked { return 0; }
void config_stream_rest(uint16_t offset, __xdata uint16_t len) __banked { }

/* Platform commands: none. */
bool platform_cmd(void) __banked { return false; }

/* Ports: every port reads as down and disabled, with nothing advertised or counted. */
uint8_t port_link_get(uint8_t port) __banked { return 0; }
bool port_phy_enabled(uint8_t port) __banked { return false; }
uint8_t port_phy_adv_get(uint8_t port) __banked { return 0; }
void port_counter_get(uint8_t port, __xdata uint8_t counter) __banked { memset(port_buf, 0, 8); }
void port_macs_learned_get(uint8_t port) __banked { memset(port_buf, 0, 4); }
void port_mtu_set(uint8_t port, __xdata uint16_t mtu) __banked { UNSUPPORTED(); }
void port_stats_print(void) __banked { UNSUPPORTED(); }
void port_media_show(uint8_t port) __banked { UNSUPPORTED(); }
void phy_set_speed(void) __banked { UNSUPPORTED(); }
void phy_set_duplex(void) __banked { UNSUPPORTED(); }
void phy_show(uint8_t port) __banked { UNSUPPORTED(); }

/* VLANs: none beyond the default, no names, PVID 1. */
void vlan_create(void) __banked { UNSUPPORTED(); }
void vlan_delete(uint16_t vlan) __banked { UNSUPPORTED(); }
void vlan_dump(void) __banked { UNSUPPORTED(); }
__xdata uint16_t vlan_name(uint16_t vlan) __banked { return 0; }
void vlan_name_remove(uint16_t vlan) __banked { }
bool vlan_entry_get(uint16_t vlan) __banked { memset(port_buf, 0, 4); return false; }
void port_pvid_set(uint8_t port, __xdata uint16_t pvid) __banked { UNSUPPORTED(); }
uint16_t port_pvid_get(uint8_t port) __banked { return 1; }
bool port_ingress_filter(__xdata uint8_t port, __xdata vlan_ingress_mode_t type) __banked { return false; }
void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked { }

/* L2 table: empty. */
bool l2_entry_next(void) __banked { return false; }
uint8_t l2_entry_delete(uint16_t idx) __banked { return 0; }
uint8_t port_l2_forget(void) __banked { UNSUPPORTED(); return 0; }
void port_l2_learned(void) __banked { UNSUPPORTED(); }
/* The shared code removes the old entry, then adds the new one: say so once, on the add. */
void port_l2_static_mgmt(__xdata uint8_t *mac, __xdata uint16_t vlan, __xdata bool remove_entry) __banked
{
	if (!remove_entry)
		UNSUPPORTED();
}

/* Management IP: no DHCP client. */
void dhcp_start(void) __banked { UNSUPPORTED(); }
void dhcp_stop(void) __banked { }
