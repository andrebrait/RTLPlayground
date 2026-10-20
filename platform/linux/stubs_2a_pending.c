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
