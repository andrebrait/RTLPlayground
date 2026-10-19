/*
 * golden_env.c - what cmd_parser.c links against beyond env_tables.c and the
 * register mock: the subsystems it dispatches into that are not under test
 * here (PHY, flash, IGMP, LACP, cable test, LEDs, DHCP, DNS, NTP, syslog; SFP
 * and STP are the real units). Each stub prints a fixed marker, so a golden still shows that a
 * command reached its handler.
 */
#include <stdint.h>

#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "rtl837x_igmp.h"
#include "rtl837x_lacp.h"
#include "rtl837x_leds.h"
#include "rtl837x_cabletest.h"
#include "rtl837x_stp.h"
#include "sfp.h"
#include "dhcp.h"
#include "dns.h"
#include "ntp.h"
#include "syslog.h"
#include "support.h"

char                 passwd[21];
uint8_t              flash_buf[FLASH_BUF_SIZE];
struct dhcp_state    dhcp_state;
struct phy_settings  phy_settings;
extern struct flash_region_t flash_region;

static void mark(const char *name)
{
	print_string("[stub:");
	print_string(name);
	print_string("]\n");
}

void reset_chip(void)                  { mark("reset_chip"); }
void flash_read_uid(void)              { mark("flash_read_uid"); }
void flash_read_jedecid(void)          { mark("flash_read_jedecid"); }
/* prints the region it was asked for, so "flash s" pins the three addresses and the length */
void flash_read_security(void)
{
	mark("flash_read_security");
	print_long(flash_region.addr);
	write_char(' ');
	print_byte(flash_region.len);
	write_char('\n');
}
void phy_set_speed(void)               { mark("phy_set_speed"); }
void phy_set_duplex(void)              { mark("phy_set_duplex"); }
void phy_show(uint8_t port)            { mark("phy_show"); }
/* the SerDes accessors print their arguments, so "sdsget"/"sdsset" pin what they pass on */
void sds_read(uint8_t sds_id, uint8_t page, uint8_t reg)
{
	mark("sds_read");
	print_byte(sds_id); print_byte(page); print_byte(reg); write_char('\n');
}
void sds_write_v(uint8_t sds_id, uint8_t page, uint8_t reg, uint16_t v)
{
	mark("sds_write_v");
	print_byte(sds_id); print_byte(page); print_byte(reg); print_short(v); write_char('\n');
}
void igmp_setup(void)                  { mark("igmp_setup"); }
void igmp_enable(void)                 { mark("igmp_enable"); }
void igmp_show(void)                   { mark("igmp_show"); }
void lacp_off(void)                    { mark("lacp_off"); }
void lacp_show(void)                   { mark("lacp_show"); }
void lacp_lag_set(uint8_t lag, uint16_t ports) { mark("lacp_lag_set"); }
void leds_dump(void)                   { mark("leds_dump"); }
void cabletest_run(void)               { mark("cabletest_run"); }
void cabletest_show(void)              { mark("cabletest_show"); }
void dhcp_start(void)                  { mark("dhcp_start"); }
void dhcp_stop(void)                   { mark("dhcp_stop"); }
void dns_parse(void)                   { mark("dns_parse"); }
void ntp_parse(void)                   { mark("ntp_parse"); }
void syslog_start(void)                { mark("syslog_start"); }
void syslog_stop(void)                 { mark("syslog_stop"); }
