/*
 * Prometheus text exposition of switch metrics for the RTL837x platform
 * This code is in the Public Domain
 */

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "rtl837x_pins.h"
#include "sfp.h"
#include "ntp.h"
#include "version.h"
#include "snmp_hw.h"
#include "metrics.h"

#pragma codeseg BANK4
#pragma constseg BANK4

extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;
extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];

__xdata uint8_t prometheus_enabled;
__xdata bool metrics_more;

/* Room kept free before each row: the longest row (node_uname_info, or
 * node_network_info with a fully escaped 32-character port name) plus its
 * family's "# TYPE" line. */
#define METRICS_ROW_MAX 400

static void put_char(char c)
{
	outbuf[slen++] = c;
}

/* Decimal of an 8-byte big-endian counter, by long division of a copy. */
static void put_u64(__xdata const struct snmp_counter *__xdata v)
{
	__xdata uint8_t n[8], d[20];
	__xdata uint8_t i, k = 0, nz;
	__xdata uint16_t cur;

	for (i = 0; i < 8; i++)
		n[i] = v->bytes[i];
	do {
		cur = 0;
		nz = 0;
		for (i = 0; i < 8; i++) {
			cur = (cur << 8) | n[i];
			n[i] = cur / 10;
			cur %= 10;
			nz |= n[i];
		}
		d[k++] = '0' + cur;
	} while (nz);
	while (k)
		put_char(d[--k]);
}

/* v / 10^decimals, e.g. (-1250, 3) is "-1.250". */
static void put_fixed(__xdata int32_t v, uint8_t decimals)
{
	__xdata uint8_t d[11], k = 0;
	__xdata uint32_t u = v;

	if (v < 0) {
		put_char('-');
		u = 0 - u;
	}
	do {
		d[k++] = '0' + u % 10;
		u /= 10;
	} while (u || k <= decimals);
	while (k) {
		put_char(d[--k]);
		if (k && k == decimals)
			put_char('.');
	}
}

static void put_u32(__xdata uint32_t v)
{
	__xdata struct snmp_counter c;
	__xdata uint8_t i;

	for (i = 0; i < 4; i++)
		c.bytes[i] = 0;
	for (i = 8; i > 4; i--) {
		c.bytes[i - 1] = v;
		v >>= 8;
	}
	put_u64(&c);
}

static void put_str(__code const char *s)
{
	while (*s)
		put_char(*s++);
}

/* Label value from RAM, at most max bytes, with \, " and newline escaped. */
static void put_esc(__xdata const char *s, __xdata uint8_t max)
{
	for (; max && *s; max--, s++) {
		if (*s == '\\' || *s == '"')
			put_char('\\');
		if (*s == '\n') {
			put_str("\\n");
			continue;
		}
		put_char(*s);
	}
}

static void put_hex(uint8_t b)
{
	put_char("0123456789abcdef"[b >> 4]);
	put_char("0123456789abcdef"[b & 15]);
}

/* Family table. A stat term packs selector and word: (selector << 2) | half. */
#define H 1	/* high 32-bit word */
#define L 2	/* low 32-bit word */
#define W 3	/* both words, 64-bit */
#define S(sel, half) (((sel) << 2) | (half))

enum { T_GAUGE, T_COUNTER, T_UNTYPED };
enum {
	K_UNAME, K_BOOT, K_TIME, K_CHIP, K_HWTEMP, K_ZONE,
	K_INFO, K_CARRIER, K_SPEED, K_STAT,
	K_SFP_INFO, K_SFP_LOS, K_SFP_DOM, K_SFP_EXT
};

struct family {
	__code const char *name;
	uint8_t type;
	uint8_t kind;
	uint8_t arg[3];	/* K_STAT: up to three terms that are summed; K_SFP_DOM: offset, multiplier, decimals */
};

static __code const char * __code const type_names[] = { "gauge", "counter", "untyped" };

static __code const struct family families[] = {
	{"node_uname_info", T_GAUGE, K_UNAME, {0}},
	{"node_boot_time_seconds", T_GAUGE, K_BOOT, {0}},
	{"node_time_seconds", T_GAUGE, K_TIME, {0}},
	{"node_hwmon_chip_names", T_GAUGE, K_CHIP, {0}},
	{"node_hwmon_temp_celsius", T_GAUGE, K_HWTEMP, {0}},
	{"node_thermal_zone_temp", T_GAUGE, K_ZONE, {0}},
	{"node_network_info", T_GAUGE, K_INFO, {0}},
	{"node_network_carrier", T_GAUGE, K_CARRIER, {0}},
	{"node_network_speed_bytes", T_GAUGE, K_SPEED, {0}},
	/* Linux /proc/net/dev fields, mapped as otto_stats_update_link() in the
	 * OpenWrt realtek DSA driver does. Bytes include the FCS. */
	{"node_network_receive_bytes_total", T_COUNTER, K_STAT, {S(0, W)}},
	{"node_network_receive_packets_total", T_COUNTER, K_STAT, {S(2, W), S(3, W), S(4, W)}},
	{"node_network_receive_errs_total", T_COUNTER, K_STAT, {S(15, L)}},
	{"node_network_receive_drop_total", T_COUNTER, K_STAT, {S(13, L)}},
	{"node_network_receive_frame_total", T_COUNTER, K_STAT, {S(15, L)}},
	{"node_network_receive_multicast_total", T_COUNTER, K_STAT, {S(3, W)}},
	{"node_network_transmit_bytes_total", T_COUNTER, K_STAT, {S(1, W)}},
	{"node_network_transmit_packets_total", T_COUNTER, K_STAT, {S(5, W), S(6, W), S(7, W)}},
	{"node_network_transmit_errs_total", T_COUNTER, K_STAT, {S(11, H), S(10, L)}},
	{"node_network_transmit_drop_total", T_COUNTER, K_STAT, {S(8, H)}},
	{"node_network_transmit_colls_total", T_COUNTER, K_STAT, {S(20, H)}},
	{"node_network_transmit_carrier_total", T_COUNTER, K_STAT, {S(11, H), S(10, L)}},
	{"rtlplayground_sfp_info", T_GAUGE, K_SFP_INFO, {0}},
	{"rtlplayground_sfp_los", T_GAUGE, K_SFP_LOS, {0}},
	{"rtlplayground_sfp_voltage_volts", T_GAUGE, K_SFP_DOM, {2, 1, 4}},
	{"rtlplayground_sfp_tx_bias_amperes", T_GAUGE, K_SFP_DOM, {4, 2, 6}},
	{"rtlplayground_sfp_tx_power_watts", T_GAUGE, K_SFP_DOM, {6, 1, 7}},
	{"rtlplayground_sfp_rx_power_watts", T_GAUGE, K_SFP_DOM, {8, 1, 7}},
	{"rtlplayground_sfp_external_calibration", T_GAUGE, K_SFP_EXT, {0}},
#define MIB_FIRST 28	/* index of the first row below */
	/* GET /metrics?mib=1 only: the counterpart of the OpenWrt ethtool
	 * collector. Names are the OpenWrt RTL930x MIB strings where one exists
	 * (otto_stats_930x_mib_list), else the html/app.js MIB label, passed
	 * through the Lua collector's build_ethtool_fqname(): runs of characters
	 * other than [0-9A-Za-z_] become "_", lowercase, leading "_" stripped,
	 * and the words "rx"/"tx" become "received"/"transmitted". */
	{"node_ethtool_received_bytes_total", T_UNTYPED, K_STAT, {S(0, W)}},
	{"node_ethtool_transmitted_bytes_total", T_UNTYPED, K_STAT, {S(1, W)}},
	{"node_ethtool_received_packets_total", T_UNTYPED, K_STAT, {S(2, W), S(3, W), S(4, W)}},
	{"node_ethtool_transmitted_packets_total", T_UNTYPED, K_STAT, {S(5, W), S(6, W), S(7, W)}},
	{"node_ethtool_interface_in_unicast_pkts", T_UNTYPED, K_STAT, {S(2, W)}},
	{"node_ethtool_interface_in_multicast_pkts", T_UNTYPED, K_STAT, {S(3, W)}},
	{"node_ethtool_interface_in_broadcast_pkts", T_UNTYPED, K_STAT, {S(4, W)}},
	{"node_ethtool_interface_out_unicast_pkts", T_UNTYPED, K_STAT, {S(5, W)}},
	{"node_ethtool_interface_out_multicast_pkts", T_UNTYPED, K_STAT, {S(6, W)}},
	{"node_ethtool_interface_out_broadcast_pkts", T_UNTYPED, K_STAT, {S(7, W)}},
	{"node_ethtool_ifoutdiscards", T_UNTYPED, K_STAT, {S(8, H)}},
	{"node_ethtool_dot1dtpportindiscards", T_UNTYPED, K_STAT, {S(8, L)}},
	{"node_ethtool_802_3_single_collision_frames", T_UNTYPED, K_STAT, {S(9, H)}},
	{"node_ethtool_802_3_multi_collision_frames", T_UNTYPED, K_STAT, {S(9, L)}},
	{"node_ethtool_802_3_deferred_transmissions", T_UNTYPED, K_STAT, {S(10, H)}},
	{"node_ethtool_802_3_late_collisions", T_UNTYPED, K_STAT, {S(10, L)}},
	{"node_ethtool_802_3_excessive_collisions", T_UNTYPED, K_STAT, {S(11, H)}},
	{"node_ethtool_802_3_symbol_errors", T_UNTYPED, K_STAT, {S(11, L)}},
	{"node_ethtool_802_3_control_in_unknown_opcodes", T_UNTYPED, K_STAT, {S(12, H)}},
	{"node_ethtool_802_3_in_pause_frames", T_UNTYPED, K_STAT, {S(12, L)}},
	{"node_ethtool_802_3_out_pause_frames", T_UNTYPED, K_STAT, {S(13, H)}},
	{"node_ethtool_dropevents", T_UNTYPED, K_STAT, {S(13, L)}},
	{"node_ethtool_transmitted_broadcastpkts", T_UNTYPED, K_STAT, {S(14, H)}},
	{"node_ethtool_transmitted_multicastpkts", T_UNTYPED, K_STAT, {S(14, L)}},
	{"node_ethtool_transmitted_crcalignerrors", T_UNTYPED, K_STAT, {S(15, H)}},
	{"node_ethtool_received_ether_crc_align_errors", T_UNTYPED, K_STAT, {S(15, L)}},
	{"node_ethtool_transmitted_undersizepkts", T_UNTYPED, K_STAT, {S(16, H)}},
	{"node_ethtool_received_ether_undersized_pkts", T_UNTYPED, K_STAT, {S(16, L)}},
	{"node_ethtool_transmitted_oversizepkts", T_UNTYPED, K_STAT, {S(17, H)}},
	{"node_ethtool_received_ether_oversized_pkts", T_UNTYPED, K_STAT, {S(17, L)}},
	{"node_ethtool_transmitted_fragments", T_UNTYPED, K_STAT, {S(18, H)}},
	{"node_ethtool_received_ether_fragments", T_UNTYPED, K_STAT, {S(18, L)}},
	{"node_ethtool_transmitted_jabbers", T_UNTYPED, K_STAT, {S(19, H)}},
	{"node_ethtool_received_ether_jabbers", T_UNTYPED, K_STAT, {S(19, L)}},
	{"node_ethtool_transmitted_collisions", T_UNTYPED, K_STAT, {S(20, H)}},
	{"node_ethtool_transmitted_ether_pkts_64_octets", T_UNTYPED, K_STAT, {S(20, L)}},
	{"node_ethtool_received_ether_pkts_64_octets", T_UNTYPED, K_STAT, {S(21, H)}},
	{"node_ethtool_transmitted_ether_65_127_octets", T_UNTYPED, K_STAT, {S(21, L)}},
	{"node_ethtool_received_ether_65_127_octets", T_UNTYPED, K_STAT, {S(22, H)}},
	{"node_ethtool_transmitted_ether_pkts_128_255_octets", T_UNTYPED, K_STAT, {S(22, L)}},
	{"node_ethtool_received_ether_pkts_128_255_octets", T_UNTYPED, K_STAT, {S(23, H)}},
	{"node_ethtool_transmitted_ether_pkts_256_511_octets", T_UNTYPED, K_STAT, {S(23, L)}},
	{"node_ethtool_received_ether_pkts_256_511_octets", T_UNTYPED, K_STAT, {S(24, H)}},
	{"node_ethtool_transmitted_ether_pkts_512_1023_octets", T_UNTYPED, K_STAT, {S(24, L)}},
	{"node_ethtool_received_ether_pkts_512_1023_octets", T_UNTYPED, K_STAT, {S(25, H)}},
	{"node_ethtool_transmitted_ether_pkts_1024_1518_octets", T_UNTYPED, K_STAT, {S(25, L)}},
	{"node_ethtool_received_ether_pkts_1024_1518_octets", T_UNTYPED, K_STAT, {S(26, H)}},
	{"node_ethtool_received_undersizedroppkts", T_UNTYPED, K_STAT, {S(27, H)}},
	{"node_ethtool_transmitted_ether_pkts_1518_octets", T_UNTYPED, K_STAT, {S(27, L)}},
	{"node_ethtool_received_ether_pkts_1518_octets", T_UNTYPED, K_STAT, {S(28, H)}},
	{"node_ethtool_transmitted_ether_pkts_too_large", T_UNTYPED, K_STAT, {S(28, L)}},
	{"node_ethtool_received_ether_pkts_too_large", T_UNTYPED, K_STAT, {S(29, H)}},
	{"node_ethtool_transmitted_pktsflexibleoctetsset1", T_UNTYPED, K_STAT, {S(29, L)}},
	{"node_ethtool_received_pktsflexibleoctetsset1", T_UNTYPED, K_STAT, {S(30, H)}},
	{"node_ethtool_transmitted_pktsflexibleoctetscrcset1", T_UNTYPED, K_STAT, {S(30, L)}},
	{"node_ethtool_received_pktsflexibleoctetscrcset1", T_UNTYPED, K_STAT, {S(31, H)}},
	{"node_ethtool_transmitted_pktsflexibleoctetsset0", T_UNTYPED, K_STAT, {S(31, L)}},
	{"node_ethtool_received_pktsflexibleoctetsset0", T_UNTYPED, K_STAT, {S(32, H)}},
	{"node_ethtool_transmitted_pktsflexibleoctetscrcset0", T_UNTYPED, K_STAT, {S(32, L)}},
	{"node_ethtool_received_pktsflexibleoctetscrcset0", T_UNTYPED, K_STAT, {S(33, H)}},
	{"node_ethtool_lengthfielderror", T_UNTYPED, K_STAT, {S(33, L)}},
	{"node_ethtool_falsecarriertimes", T_UNTYPED, K_STAT, {S(34, H)}},
	{"node_ethtool_undersizeoctets", T_UNTYPED, K_STAT, {S(34, L)}},
	{"node_ethtool_framingerrors", T_UNTYPED, K_STAT, {S(35, H)}},
	{"node_ethtool_received_macdiscards", T_UNTYPED, K_STAT, {S(36, H)}},
	{"node_ethtool_received_macipgshortdrop", T_UNTYPED, K_STAT, {S(36, L)}},
	{"node_ethtool_802_1d_tp_learned_entry_discards", T_UNTYPED, K_STAT, {S(37, L)}},
	{"node_ethtool_egress_queue_7_dropped_pkts", T_UNTYPED, K_STAT, {S(38, H)}},
	{"node_ethtool_egress_queue_6_dropped_pkts", T_UNTYPED, K_STAT, {S(38, L)}},
	{"node_ethtool_egress_queue_5_dropped_pkts", T_UNTYPED, K_STAT, {S(39, H)}},
	{"node_ethtool_egress_queue_4_dropped_pkts", T_UNTYPED, K_STAT, {S(39, L)}},
	{"node_ethtool_egress_queue_3_dropped_pkts", T_UNTYPED, K_STAT, {S(40, H)}},
	{"node_ethtool_egress_queue_2_dropped_pkts", T_UNTYPED, K_STAT, {S(40, L)}},
	{"node_ethtool_egress_queue_1_dropped_pkts", T_UNTYPED, K_STAT, {S(41, H)}},
	{"node_ethtool_egress_queue_0_dropped_pkts", T_UNTYPED, K_STAT, {S(41, L)}},
	{"node_ethtool_egress_queue_7_out_pkts", T_UNTYPED, K_STAT, {S(42, H)}},
	{"node_ethtool_egress_queue_6_out_pkts", T_UNTYPED, K_STAT, {S(42, L)}},
	{"node_ethtool_egress_queue_5_out_pkts", T_UNTYPED, K_STAT, {S(43, H)}},
	{"node_ethtool_egress_queue_4_out_pkts", T_UNTYPED, K_STAT, {S(43, L)}},
	{"node_ethtool_egress_queue_3_out_pkts", T_UNTYPED, K_STAT, {S(44, H)}},
	{"node_ethtool_egress_queue_2_out_pkts", T_UNTYPED, K_STAT, {S(44, L)}},
	{"node_ethtool_egress_queue_1_out_pkts", T_UNTYPED, K_STAT, {S(45, H)}},
	{"node_ethtool_egress_queue_0_out_pkts", T_UNTYPED, K_STAT, {S(45, L)}},
	{"node_ethtool_transmitted_good_counter", T_UNTYPED, K_STAT, {S(46, W)}},
	{"node_ethtool_received_good_counter", T_UNTYPED, K_STAT, {S(47, W)}},
	{"node_ethtool_received_error_counter", T_UNTYPED, K_STAT, {S(48, H)}},
	{"node_ethtool_transmitted_error_counter", T_UNTYPED, K_STAT, {S(48, L)}},
	{"node_ethtool_transmitted_good_counter_phy", T_UNTYPED, K_STAT, {S(49, W)}},
	{"node_ethtool_received_good_counter_phy", T_UNTYPED, K_STAT, {S(50, W)}},
	{"node_ethtool_received_error_counter_phy", T_UNTYPED, K_STAT, {S(51, H)}},
	{"node_ethtool_transmitted_error_counter_phy", T_UNTYPED, K_STAT, {S(51, L)}},
};
#define FAMILIES (sizeof(families) / sizeof(families[0]))

static __xdata uint8_t fam, row, typed, mib;

static void begin_sample(void)
{
	if (!typed) {
		put_str("# TYPE ");
		put_str(families[fam].name);
		put_char(' ');
		put_str(type_names[families[fam].type]);
		put_char('\n');
		typed = 1;
	}
	put_str(families[fam].name);
}

/* Logical port of this row; row 0 is the switch itself. */
#define PORT (MACHINE_MIN_PORT + row - 1)

static void put_device(void)
{
	put_str("device=\"lan");
	put_fixed(machine.log_to_phys_port[PORT], 0);
	put_char('"');
}

static void end_sample_int(__xdata int32_t v, __xdata uint8_t decimals)
{
	put_char(' ');
	put_fixed(v, decimals);
	put_char('\n');
}

static uint32_t uptime(void)
{
	reg_read_m(RTL837X_REG_SEC_COUNTER);
	return ((uint32_t)sfr_data[0] << 24) | ((uint32_t)sfr_data[1] << 16) |
	       ((uint16_t)sfr_data[2] << 8) | sfr_data[3];
}

/* SFP slot of this row's port when a module is inserted, else 0xff. */
static uint8_t sfp_slot(void)
{
	__xdata uint8_t s;

	if (!row || !machine.is_sfp[PORT])
		return 0xff;
	s = machine.is_sfp[PORT] - 1;
	return (sfp_pins_last & (1 << (s << 2))) ? 0xff : s;
}

/* Slot with DOM values this exporter can convert: internally calibrated.
 * shortcut: externally calibrated modules (SFF-8472 byte 92 bit 4) need the
 * float calibration polynomials and report no values; add them when a
 * deployed module needs it. */
static uint8_t dom_slot(void)
{
	__xdata uint8_t s = sfp_slot();

	return (s != 0xff && (sfp_options[s] & 0x50) == 0x40) ? s : 0xff;
}

static bool dom_word(uint8_t slot, __xdata uint8_t offset, __xdata uint16_t *__xdata v)
{
	if (!sfp_read_block(slot, 224 + offset, 2))
		return false;
	*v = ((uint16_t)sfp_buf[0] << 8) | sfp_buf[1];
	return true;
}

/* SoC sensor, signed 1/128 degrees as temp_to_html() reads it. */
static void soc_temp(void)
{
	reg_read_m(RTL837X_TM_RESULT);
	end_sample_int((int32_t)(int16_t)(((uint16_t)sfr_data[2] << 8) | sfr_data[3]) * 1000 / 128, 3);
}

static void add64(__xdata uint8_t *__xdata acc, __xdata const uint8_t *__xdata b)
{
	__xdata uint8_t i = 8;
	__xdata uint16_t carry = 0;

	while (i--) {
		carry += acc[i] + b[i];
		acc[i] = carry;
		carry >>= 8;
	}
}

/* Emits this family's line for the current row, if it has one. */
static void emit_row(void)
{
	__code const struct family *f = &families[fam];
	__xdata uint8_t k, s;
	__xdata uint16_t w = 0;	/* SDCC cannot see through the short-circuit reads */
	__xdata uint32_t v;
	__xdata struct snmp_counter c, sum;

	switch (f->kind) {
	case K_UNAME:
		if (row)
			return;
		begin_sample();
		put_str("{sysname=\"RTLPlayground\",release=\"" VERSION_SW "\",version=\"" BUILD_DATE
			"\",machine=\"");
		put_str(machine.machine_name);
		put_str("\",nodename=\"");
		put_esc(hostname, sizeof(hostname));
		put_str("\",domainname=\"(none)\"} 1\n");
		return;
	case K_BOOT:
	case K_TIME:
		if (row || !ntp_state.synced)
			return;
		v = ntp_state.utc - ntp_state.at;
		if (f->kind == K_TIME)
			v += uptime();
		begin_sample();
		put_char(' ');
		put_u32(v);
		put_char('\n');
		return;
	case K_CHIP:
	case K_HWTEMP:
		if (row) {
			s = dom_slot();
			if (s == 0xff || (f->kind == K_HWTEMP && !dom_word(s, 0, &w)))
				return;
			begin_sample();
			put_str("{chip=\"sfp_lan");
			put_fixed(machine.log_to_phys_port[PORT], 0);
			if (f->kind == K_CHIP) {
				put_str("\",chip_name=\"sfp_lan");
				put_fixed(machine.log_to_phys_port[PORT], 0);
				put_str("\"} 1\n");
				return;
			}
			put_str("\",sensor=\"temp1\"}");
			end_sample_int((int32_t)(int16_t)w * 1000 / 256, 3);
			return;
		}
		begin_sample();
		if (f->kind == K_CHIP) {
			put_str("{chip=\"thermal_thermal_zone0\",chip_name=\"cpu_thermal\"} 1\n");
			return;
		}
		put_str("{chip=\"thermal_thermal_zone0\",sensor=\"temp1\"}");
		soc_temp();
		return;
	case K_ZONE:
		if (row)
			return;
		begin_sample();
		put_str("{zone=\"0\",type=\"cpu-thermal\",policy=\"step_wise\",mode=\"enabled\"}");
		soc_temp();
		return;
	}

	if (!row)
		return;

	switch (f->kind) {
	case K_INFO:
	case K_CARRIER:
	case K_SPEED:
		if (!snmp_hw_link(PORT, &v))
			return;
		v = (v >> PORT) & 1;
		if (f->kind == K_SPEED) {
			if (!v || !snmp_hw_speed(PORT, &w) || !w)
				return;
			v = (uint32_t)w * 125000;
		} else if (f->kind == K_INFO) {
			k = 1;
			if (!v && !snmp_hw_admin(PORT, &k))
				return;
			begin_sample();
			put_char('{');
			put_device();
			put_str(",address=\"");
			for (s = 0; s < 6; s++) {
				if (s)
					put_char(':');
				put_hex(uip_ethaddr.addr[s]);
			}
			put_str("\",broadcast=\"ff:ff:ff:ff:ff:ff\",operstate=\"");
			put_str(v ? "up" : k == 2 ? "down" : "lowerlayerdown");
			put_str("\",ifalias=\"");
			put_esc(port_names[PORT], PORT_NAME_SIZE);
			put_str("\"} 1\n");
			return;
		}
		break;
	case K_STAT:
		for (s = 0; s < 8; s++)
			sum.bytes[s] = 0;
		for (k = 0; k < 3 && f->arg[k]; k++) {
			s = f->arg[k];
			if (!snmp_hw_stat(PORT, s >> 2, s & H, s & L, &c))
				return;
			add64(sum.bytes, c.bytes);
		}
		begin_sample();
		put_char('{');
		put_device();
		put_str("} ");
		put_u64(&sum);
		put_char('\n');
		return;
	case K_SFP_INFO:
		s = sfp_slot();
		if (s == 0xff)
			return;
		begin_sample();
		put_char('{');
		put_device();
		put_str(",vendor=\"");
		put_esc(sfp_module_vendor[s], 16);
		put_str("\",model=\"");
		put_esc(sfp_module_model[s], 16);
		put_str("\",serial=\"");
		put_esc(sfp_module_serial[s], 16);
		put_str("\"} 1\n");
		return;
	case K_SFP_LOS:
		s = sfp_slot();
		if (s == 0xff || machine.sfp_port[s].pin_los == GPIO_NA)
			return;
		v = !!(sfp_pins_last & (2 << (s << 2)));
		break;
	case K_SFP_EXT:
		s = sfp_slot();
		if (s == 0xff || !(sfp_options[s] & 0x40))
			return;
		v = !!(sfp_options[s] & 0x10);
		break;
	case K_SFP_DOM:
		s = dom_slot();
		if (s == 0xff || !dom_word(s, f->arg[0], &w))
			return;
		begin_sample();
		put_char('{');
		put_device();
		put_char('}');
		end_sample_int((int32_t)w * f->arg[1], f->arg[2]);
		return;
	default:
		return;
	}
	begin_sample();
	put_char('{');
	put_device();
	put_char('}');
	end_sample_int(v, 0);
}

void metrics_fill(void) __banked
{
	while (fam < (mib ? FAMILIES : MIB_FIRST)) {
		if (slen + METRICS_ROW_MAX > TCP_OUTBUF_SIZE)
			return;
		emit_row();
		if (++row > MACHINE_MAX_PORT - MACHINE_MIN_PORT + 1) {
			row = 0;
			typed = 0;
			fam++;
		}
	}
	metrics_more = false;
}

void metrics_start(__xdata bool with_mib) __banked
{
	slen = 0;
	put_str("HTTP/1.1 200 OK\r\nConnection: close\r\n"
		"Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n\r\n");
	fam = row = typed = 0;
	mib = with_mib;
	metrics_more = true;
	metrics_fill();
}
