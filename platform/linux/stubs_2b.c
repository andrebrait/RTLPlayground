/*
 * What the shared code links against that this platform does not do yet: the features of
 * milestone 2b, and the hardware with no counterpart on the bench board (SFP cages, the chip
 * temperature, the flash size). A command says "not supported", a getter returns the off
 * state. The change that implements an entry deletes it from here.
 */
#include <string.h>

#include "cmd_parser.h"
#include "platform.h"

#define UNSUPPORTED() cmd_error("Not supported on this platform yet\n")

/* Hardware without a counterpart: no cages, no sensor, no flash chip. */
bool sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len) __banked { return false; }
bool sfp_print_info(uint8_t sfp) __banked { return false; }
void handle_sfp(void) __banked { }
bool sfp_present(uint8_t slot) __banked { return false; }
bool sfp_los(uint8_t slot) __banked { return false; }
bool sfp_present_live(uint8_t slot) { return false; }
int16_t chip_temp_get(uint8_t sensor) __banked { return 0; }
const char *get_flash_size_str(void) { return ""; }

/* EEE */
uint8_t port_eee_adv_get(uint8_t port) __banked { return 0; }
uint8_t port_eee_lp_get(uint8_t port) __banked { return 0; }
bool port_eee_active(uint8_t port) __banked { return false; }
void port_eee_enable_all(__xdata uint8_t speed) __banked { UNSUPPORTED(); }
void port_eee_disable_all(void) __banked { UNSUPPORTED(); }
void port_eee_status_all(void) __banked { UNSUPPORTED(); }
void port_eee_enable(__xdata uint8_t port, __xdata uint8_t speed) __banked { UNSUPPORTED(); }
void port_eee_disable(uint8_t port) __banked { UNSUPPORTED(); }
void port_eee_status(uint8_t port) __banked { UNSUPPORTED(); }

/* LLDP: `lldp on` asks for LLDP frames to be trapped to the CPU, which this platform cannot
 * do yet, so the state stays off. `lldp off` (RMA_ACT_FORWARD) has nothing to undo. */
void trap_rma_set(uint16_t rma, uint8_t act) __banked
{
	lldp_enabled = 0;
	if (act == RMA_ACT_TRAP)
		UNSUPPORTED();
}

/* Mirroring, isolation, link aggregation */
void port_mirror_set(uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked { UNSUPPORTED(); }
void port_mirror_del(void) __banked { UNSUPPORTED(); }
void port_mirror_get(void) __banked { memset(port_buf, 0, 6); }
void port_isolate(uint8_t port, __xdata uint16_t pmask) __banked { UNSUPPORTED(); }
/* Isolation is off: every port reaches every port and the CPU, as the 8051 sets it up
 * (rtl837x_port.c port_l2_setup) and as `isolate <port> off` writes it. */
uint16_t port_isolation_get(uint8_t port) __banked
{
	uint16_t members = PMASK_CPU;

	if (port > machine.max_port)
		return 0;
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++)
		members |= (uint16_t)1 << i;
	return members;
}
uint16_t port_lag_members_get(uint8_t lag) __banked { return 0; }
uint8_t port_lag_of(uint8_t port) __banked { return PORT_LAG_NONE; }
void port_lag_members_set(__xdata uint8_t lag, __xdata uint16_t members) __banked { UNSUPPORTED(); }
void port_lag_hash_set(__xdata uint8_t lag, __xdata uint8_t hash) __banked { UNSUPPORTED(); }
void port_lag_hash_get(uint8_t lag) __banked { memset(port_buf, 0, 4); }
void lacp_off(void) __banked { UNSUPPORTED(); }
void lacp_show(void) __banked { UNSUPPORTED(); }
void lacp_lag_set(uint8_t lag, __xdata uint16_t ports) __banked { UNSUPPORTED(); }

/* Rate limits */
void bandwidth_get(uint8_t port, __xdata uint8_t egress) __banked { memset(port_buf, 0, 5); }
void bandwidth_egress_disable(uint8_t port) __banked { UNSUPPORTED(); }
void bandwidth_egress_set(uint8_t port, __xdata uint32_t bw) __banked { UNSUPPORTED(); }
void bandwidth_ingress_disable(uint8_t port) __banked { UNSUPPORTED(); }
void bandwidth_ingress_drop(uint8_t port) __banked { UNSUPPORTED(); }
void bandwidth_ingress_fc(uint8_t port) __banked { UNSUPPORTED(); }
void bandwidth_ingress_set(uint8_t port, __xdata uint32_t bw) __banked { UNSUPPORTED(); }
void bandwidth_status(uint8_t port) __banked { UNSUPPORTED(); }
void storm_get(uint8_t port) __banked { memset(port_buf, 0, sizeof port_buf); }
void storm_off(uint8_t port, __xdata uint8_t type) __banked { UNSUPPORTED(); }
void storm_set(uint8_t port, __xdata uint8_t type, __xdata uint32_t rate, __xdata uint8_t pps) __banked { UNSUPPORTED(); }
void storm_show(void) __banked { UNSUPPORTED(); }

/* Spanning tree (off: every port forwards), IGMP, cable test */
uint8_t stp_port_state_get(uint8_t log_port) __banked { return 3; }
void stp_parse(void) __banked __reentrant { UNSUPPORTED(); }
void igmp_setup(void) __banked { UNSUPPORTED(); }
void igmp_enable(void) __banked { UNSUPPORTED(); }
void igmp_show(void) __banked { UNSUPPORTED(); }
void cabletest_run(void) __banked { UNSUPPORTED(); }
void cabletest_show(void) __banked { UNSUPPORTED(); }

/* DNS, NTP, syslog */
void dns_parse(void) __banked __reentrant { UNSUPPORTED(); }
void ntp_parse(void) __banked __reentrant { UNSUPPORTED(); }
void syslog_start(void) __banked { UNSUPPORTED(); }
void syslog_stop(void) __banked { UNSUPPORTED(); }
