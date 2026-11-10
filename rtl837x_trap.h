#ifndef _RTL837X_TRAP_H_
#define _RTL837X_TRAP_H_

#include <stdint.h>
#include "rtl837x_common.h"

#define TRAP_RX_HDR_SIZE	(12 + RTL_TAG_SIZE)

#define TRAP_RX_BODY() \
	((uip_buf[TRAP_RX_HDR_SIZE] == 0x81 && uip_buf[TRAP_RX_HDR_SIZE + 1] == 0x00) \
	 ? TRAP_RX_HDR_SIZE + VLAN_TAG_SIZE : TRAP_RX_HDR_SIZE)

/* Last byte of the reserved destination MAC, not a chip-specific register address. */
#define TRAP_RMA_STP 0x00
#define TRAP_RMA_LACP 0x02
#define TRAP_RMA_LLDP 0x0e
#define TRAP_ACT_FORWARD 0
#define TRAP_ACT_TRAP 1
#define TRAP_ACT_DROP 2

void trap_init(void) __banked;
void trap_rma_set(uint8_t rma, __xdata uint8_t act) __banked;

#endif
