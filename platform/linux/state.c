/*
 * The data the shared files (cmd_parser.c, cmd_editor.c, httpd/page_impl.c) read and write
 * as globals: on the 8051 these live in rtlplayground.c, httpd.c and the protocol modules.
 * Sizes are the 8051's, so the shared code sees the same bounds. Protocol state of features
 * this platform does not run yet (STP, LACP, LLDP, SFP) stays zero, which is their off state.
 */
#include <stdbool.h>

#include "platform.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "rtl837x_stp.h"
#include "rtl837x_lacp.h"
#include "uip/uip.h"
#include "dhcp.h"
#include "syslog.h"

/* The serial ring the console input fills (console.c) and cmd_editor.c drains. */
__xdata uint8_t sbuf[SBUF_SIZE];
__xdata volatile uint8_t sbuf_ptr;
bool cmd_available;

/* The HTTP response being built, and what is left of it to stream (httpd). */
uint8_t outbuf[TCP_OUTBUF_SIZE];
uint16_t slen;
uint16_t cont_len;
uint32_t cont_addr;

__xdata char passwd[21];
__xdata char hostname[24];
__xdata uint16_t management_vlan = 1;
__xdata uint16_t session_timeout;
__xdata uint8_t flash_buf[FLASH_BUF_SIZE];
volatile __xdata uint32_t ticks;
const uint8_t * const hex = (const uint8_t *)"0123456789abcdef";

__xdata uip_ipaddr_t uip_hostaddr, uip_netmask, uip_draddr;
__xdata struct uip_eth_addr uip_ethaddr;
__xdata struct dhcp_state dhcp_state;
__xdata struct syslog_state syslog_state;
__xdata struct phy_settings phy_settings;
__xdata struct vlan_settings vlan_settings;
__xdata struct l2_entry l2_buf;
__xdata uint8_t port_buf[24];

__xdata uint8_t sfp_buf[16], sfp_options[2], sfp_speed[2];
__xdata uint8_t sfp_pins_last;
__xdata char sfp_module_vendor[2][17], sfp_module_model[2][17], sfp_module_serial[2][17];

__xdata bool stp_enabled;
__xdata bool lldp_enabled;
__xdata uint16_t lldp_logical_port_status;
__xdata uint8_t lacpEnabled;
__xdata uint8_t stp_prio, stp_hello_s, stp_maxage_s, stp_fwddelay_s, stp_rstp, stp_txhold;
__xdata uint8_t stp_pflags[STP_ENTITIES], stp_pprio[STP_ENTITIES], stp_pp2p[STP_ENTITIES];
__xdata uint32_t stp_pcost[STP_ENTITIES], stp_dcost[STP_ENTITIES];
__xdata uint8_t stp_ent_of[STP_PORTS];
__xdata uint16_t stp_lag_mask[STP_LAG_COUNT];
__xdata struct bridge stp_dbridge[STP_ENTITIES];
__xdata uint16_t stp_dpid[STP_ENTITIES], stp_bpdu_age[STP_ENTITIES];
__xdata struct bridge root_bridge;
__xdata uint32_t root_bridge_cost;
__xdata uint8_t stp_root_port = 0xff;
__xdata uint16_t stp_tc_count;
__xdata uint8_t lacp_actor_state[10], lacp_partner_state[10], lacp_rx_state[10], lacp_port_lag[10];
__xdata uint8_t lacp_partner_sys[10][MAC_ADDR_LEN];
__xdata uint16_t lacp_rx_count[10], lacp_lag_ports[LACP_NUM_LAGS], lacp_members_last[LACP_NUM_LAGS];
__xdata uint8_t lacp_agg_sys[LACP_NUM_LAGS][MAC_ADDR_LEN], lacp_agg_valid[LACP_NUM_LAGS];
