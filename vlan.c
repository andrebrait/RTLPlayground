/*
 * The chip-independent part of the VLAN support: the VLAN name table and the
 * ingress configuration listing, on top of the getters of rtl837x_port.c or
 * rtl836x_port.c.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_port.h"
#include "machine.h"

#pragma codeseg BANK1
#pragma constseg BANK1

extern __code const uint8_t * __code const hex;
extern __xdata uint16_t vlan_ptr;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];

__xdata struct vlan_settings vlan_settings;


void vlan_name_remove(uint16_t vlan) __banked
{
	static __xdata uint16_t name_pos;
	static __xdata uint16_t entry_start;
	static __xdata uint16_t pos;
	static __xdata uint16_t entry_len;
	static __xdata uint16_t move_count;
	static __xdata uint16_t j;

	name_pos = vlan_name(vlan);
	if (name_pos == 0xffff)
		return;

	entry_start = name_pos - 3;
	pos = entry_start;

	while (pos < vlan_ptr && vlan_names[pos] != ' ')
		pos++;
	if (pos >= vlan_ptr)
		return;
	pos++;

	entry_len = pos - entry_start;
	move_count = vlan_ptr - pos;

	for (j = 0; j < move_count; j++)
		vlan_names[entry_start + j] = vlan_names[pos + j];

	vlan_ptr -= entry_len;
	vlan_names[vlan_ptr] = 0;
}


__xdata uint16_t vlan_name(uint16_t vlan) __banked
{
	__xdata int16_t i = 0;
	__xdata uint8_t begin = 1;
	while (vlan_names[i]) {
		if (begin && vlan_names[i] == hex[(vlan >> 8) & 0xf] && vlan_names[i + 1] == hex[(vlan >> 4) & 0xf] && vlan_names[i + 2] == hex[vlan & 0xf])
			break;
		begin = vlan_names[i++] == ' ' ? 1 : 0;
	}
	if (vlan_names[i])
		return i + 3;

	return 0xffff;
}


void print_port_ingress_filter_mode(vlan_ingress_mode_t mode) __banked
{
	switch (mode) {
	case VLAN_UNTAGGED:
		print_string("Untag.");
		break;
	case VLAN_TAGGED:
		print_string("Tagged");
		break;
	case VLAN_ALL:
		print_string("Any");
		break;
	default:
		print_string("!!err!!");
	}
}

void print_vlan_ingress_port(uint8_t log_port) __banked
{
	print_phys_port(log_port);write_char('\t');
	print_short(port_pvid_get(log_port));write_char('\t');
	print_port_ingress_filter_mode(port_ingress_filter_get(log_port));write_char('\t');
	port_ingress_vlan_filter_get(log_port) ? print_string("Enabled") : print_string("Disabled");
	write_char('\n');
}
/*
 * Dumps the VLAN ingress configuration
 */
void vlan_dump(void) __banked
{
	print_string("Ingress VLAN configuration:\n");
	print_string("Port\tPVID\tType\tFiltering\n");
	for (uint8_t port = MACHINE_MIN_PORT; port <= MACHINE_MAX_PORT; port++) {
		print_vlan_ingress_port(port);
	}
	print_vlan_ingress_port(9);

	write_char('\n');
	print_string("Type - Which frame types are allowed: untagged, tagged or any\n");
	print_string("Filtering - Whether packets not belonging to member VLANs on that port are dropped\n");
	print_string("PVID - Assumed VLAN for untagged packets\n");
}
