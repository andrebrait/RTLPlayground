#ifndef _RTL837X_PORT_H_
#define _RTL837X_PORT_H_

#include <stdint.h>
#include "rtl837x_regs.h"
#include "platform.h"

#define STAT_GET(cnt, port) do { \
	REG_WRITE(RTL837X_STAT_GET, 0x00, 0x00, cnt >> 3, (cnt << 5) | (port << 1) | 1); \
	do { \
		reg_read_m(RTL837X_STAT_GET); \
	} while (sfr_data[3] & 0x1); \
	} while (0)

// The ingress filter values in platform.h are the hardware encoding, spelled out as literals
// so shared code needs no register header; this fails to compile if they ever drift apart.
typedef char vlan_ingress_mode_check[(VLAN_ALL == INGR_ALLOW_ALL && VLAN_TAGGED == INGR_ALLOW_TAGGED
				      && VLAN_UNTAGGED == INGR_ALLOW_UNTAGGED) ? 1 : -1];

void vlan_setup(void) __banked;
int8_t vlan_get(uint16_t vlan) __banked;
void port_l2mc_set(uint8_t mac_last, __xdata uint16_t vid, __xdata uint16_t pmask) __banked;
void port_l2_forget_port(uint8_t port) __banked;
void port_l2_setup(void) __banked;
bool port_ingress_vlan_filter_set(uint8_t port, __xdata bool enabled) __banked;
bool port_ingress_vlan_filter_get(uint8_t port) __banked;
vlan_ingress_mode_t port_ingress_filter_get(__xdata uint8_t port) __banked;

#endif
