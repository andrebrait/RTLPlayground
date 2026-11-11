// #define REGDBG 1

#include "rtl837x_sfr.h"
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_storm.h"
#include "rtl837x_port.h"
#include "rtl837x_flash.h"
#include "rtl837x_pins.h"
#include "uip.h"
#include "html_data.h"
#include <stdint.h>
#include "phy.h"
#include "version.h"
#include "machine.h"
#include "rtl837x_stp.h"
#include "rtl837x_lacp.h"
#include "rtl837x_pause.h"
#include "page_impl.h"
#include "syslog.h"
#include "dhcp.h"
#ifdef SNMP
#include "snmp.h"
#endif
#ifdef PROMETHEUS
#include "metrics.h"
#endif
#if SOC_FAMILY == RTL836X
#include "rtl836x_regs.h"
#include "rtl836x_mib.h"
#include "rtl836x_rate.h"
#endif
#ifdef MACHINE_POE_PORTS
#include "poe.h"
#endif

// #define DEBUG
#include "debug.h"

#define L2_MAX_TRANSFER 30

#pragma codeseg BANK1
#pragma constseg BANK1

extern __code const struct machine machine;
extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;
extern __xdata uint16_t management_vlan;
extern __xdata uint16_t session_timeout;
extern __xdata uint32_t uptime_secs;
extern __xdata struct dhcp_state dhcp_state;
extern __xdata uint16_t cont_len;
extern __xdata uint32_t cont_addr;
extern __code const uint8_t * __code const hex;
extern __xdata uip_ipaddr_t uip_hostaddr, uip_draddr, uip_netmask;

extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t sfp_pins_last;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];

extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;

extern __xdata struct flash_region_t flash_region;

extern __xdata char sfp_module_vendor[2][17];
extern __xdata char sfp_module_model[2][17];
extern __xdata char sfp_module_serial[2][17];
extern __xdata uint8_t sfp_options[2];

#include "rtl837x_lldp.h"
extern __xdata bool lldp_enabled;

// Convert uint8_t to ascii HEX char push on html-buffer.
void charhex_to_html(char c)
{
	outbuf[slen++] = itohex(c);
}

static void str_x_to_html(__xdata const char *s)
{
	while (*s)
		outbuf[slen++] = *s++;
}


void json_char_to_html(uint8_t c)
{
	if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
		c = '.';
	outbuf[slen++] = c;
}

/* A mask of logical ports as a JSON array of physical port numbers; the CPU port is physical port 0 */
static void pmask_to_json(__xdata uint16_t m)
{
	char_to_html('[');
	for (__xdata uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		if ((m >> i) & 1)
			json_add_array_uint_item(machine.log_to_phys_port[i]);
	}
	if ((m >> CPU_LOGICAL_PORT) & 1)
		json_add_array_uint_item(CPU_PHYSICAL_PORT);
	char_to_html(']');
}



void itoa16_html(uint16_t v) /* up to five digits (65535) */
{
	uint8_t print_zeros = 0;
	uint8_t d;
	d = v / 10000;
	if (d) { char_to_html('0' + d); print_zeros = 1; }
	d = (v / 1000) % 10;
	if (d || print_zeros) { char_to_html('0' + d); print_zeros = 1; }
	d = (v / 100) % 10;
	if (d || print_zeros) { char_to_html('0' + d); print_zeros = 1; }
	d = (v / 10) % 10;
	if (d || print_zeros) char_to_html('0' + d);
	char_to_html('0' + (v % 10));
}

static void temp_to_html(void)
{
#if SOC_FAMILY == RTL836X
	str_code_to_html("n/a");	/* shortcut: no temperature reading on the RTL836x yet */
#else
	int16_t v;

	reg_read(RTL837X_TM_RESULT);
	v = SFR_DATA_U16;
	if (v < 0) {
		char_to_html('-');
		v = -v;
	}
	itoa_html(v >> 7);
	char_to_html('.');
	itoa_html(((v & 0x7f) * 10) >> 7);
	char_to_html(' ');
	char_to_html('C');
#endif
}

uint16_t stat_content(void)
{
	dbg_string("stat_content called\n");
	return 0;
}


uint16_t port_status(void)
{
	dbg_string("port_status called\n");
	return 0;
}


/* Converts sfr_data[] into raw hex string.
   Suppress leading zeros.
*/
void sfr_data_to_html(void)
{
 	uint8_t print_zeros = 0;
	uint8_t val = 0;

	for (uint8_t nibble = 0; nibble < 8; nibble++) {
	  	if (!(nibble & 1))
	        val = sfr_data[nibble>>1];
		// force the swap instruction, itohex() ignores the upper nibble.
		val = (val << 4) | (val >> 4);
		// when print_zeros is not zero, we know that a non-zero number has printed.
		// That have to print all the next numbers.
		print_zeros |= val;
		// only care about lower nibble, that is what is printed.
		print_zeros &= 0x0f;
		if (print_zeros)
			charhex_to_html(val);
	}
	if (print_zeros == 0) {
	    char_to_html('0');
	}
}


void reg_to_html(uint16_t reg)
{
	reg_read_m(reg);
	sfr_data_to_html();
}


void reg_to_html_long(uint16_t reg)
{
	reg_read_m(reg);
	byte_to_html(sfr_data[0]);
	byte_to_html(sfr_data[1]);
	byte_to_html(sfr_data[2]);
	byte_to_html(sfr_data[3]);
}


#if SOC_FAMILY == RTL837X
/* The 64-bit counter of the last STAT_GET as one number: the low word keeps
 * its leading zeros whenever the high word is set. */
void counter_to_html(void)
{
	reg_read_m(RTL837X_STAT_V_HIGH);
	if (SFR_DATA_24 | SFR_DATA_16 | SFR_DATA_8 | SFR_DATA_0) {
		sfr_data_to_html();
		reg_to_html_long(RTL837X_STAT_V_LOW);
	} else {
		reg_to_html(RTL837X_STAT_V_LOW);
	}
}
#else
static void mib_to_html(__xdata uint8_t *value, __xdata bool available)
{
	if (!available) {
		str_code_to_html("null");
		return;
	}
	str_code_to_html("\"0x");
	uint8_t b = 0;
	while (b < 7 && !value[b]) b++;
	if (value[b] < 16)
		charhex_to_html(value[b++]);
	for (; b < 8; b++)
		byte_to_html(value[b]);
	char_to_html('"');
}
#endif


void send_sfp_info(uint8_t sfp)
{
	// This loops over the Vendor-name, Vendor OUI, Vendor PN and Vendor rev ASCII fields
	for (uint8_t i = 16; i < 64; i++) {
		if (!(i & 0xf) && !sfp_read_block(sfp, i, 16))
			return;
		if (i < 20 || i >= 60 || (i >= 36 && i < 40)) // Skip Non-ASCII codes
			continue;
		uint8_t c = sfp_buf[i & 0xf];
		if (c)
			json_char_to_html(c);
	}
}


void sfp_send_data(uint8_t slot, uint8_t reg, uint8_t len)
{
	// maximum supported transfer size is 16 bytes
	if (len > 16)
		return;

	if (!sfp_read_block(slot, reg, len))
		return;

	for (uint8_t i = 0; i < len; i++)
		byte_to_html(sfp_buf[i]);
}


void send_basic_info(void)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("send_basic_info called\n");
	str_code_to_html("{\"ip_address\":\"");
	itoa_html(uip_hostaddr[0]); char_to_html('.');
	itoa_html(uip_hostaddr[0] >> 8); char_to_html('.');
	itoa_html(uip_hostaddr[1]); char_to_html('.');
	itoa_html(uip_hostaddr[1] >> 8);
	str_code_to_html("\",\"ip_gateway\":\"");
	itoa_html(uip_draddr[0]); char_to_html('.');
	itoa_html(uip_draddr[0] >> 8); char_to_html('.');
	itoa_html(uip_draddr[1]); char_to_html('.');
	itoa_html(uip_draddr[1] >> 8);
	str_code_to_html("\",\"ip_netmask\":\"");
	itoa_html(uip_netmask[0]); char_to_html('.');
	itoa_html(uip_netmask[0] >> 8); char_to_html('.');
	itoa_html(uip_netmask[1]); char_to_html('.');
	itoa_html(uip_netmask[1] >> 8);
	str_code_to_html("\",\"syslog_server\":\"");
	itoa_html(syslog_state.server_ip[0]); char_to_html('.');
	itoa_html(syslog_state.server_ip[1]); char_to_html('.');
	itoa_html(syslog_state.server_ip[2]); char_to_html('.');
	itoa_html(syslog_state.server_ip[3]); char_to_html(':');
	itoa16_html(syslog_state.server_port);
	str_code_to_html("\",\"session_timeout\":");
	itoa16_html(session_timeout);
	str_code_to_html(dhcp_state.state == DHCP_OFF ? ",\"ip_source\":\"static\"" : ",\"ip_source\":\"dhcp\"");
	str_code_to_html(",\"mac_address\":\"");
	byte_to_html(uip_ethaddr.addr[0]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[1]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[2]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[3]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[4]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[5]);
	str_code_to_html("\",\"hostname\":\"");
	{
		__xdata char *hp = hostname;	/* sanitized on ingest, emit verbatim */
		while (*hp)
			char_to_html(*hp++);
	}
	str_code_to_html("\",\"sw_ver\":\"");
	str_code_to_html(VERSION_SW);
	str_code_to_html("\",\"build_date\":\"");
	str_code_to_html(BUILD_DATE);
	str_code_to_html("\",\"uptime\":\"0x");
	byte_to_html(uptime_secs >> 24);
	byte_to_html(uptime_secs >> 16);
	byte_to_html(uptime_secs >> 8);
	byte_to_html(uptime_secs);
	str_code_to_html("\",\"hw_ver\":\"");
	str_code_to_html(machine.machine_name);
	str_code_to_html("\",\"pcb_ver\":\"");
	str_code_to_html(MACHINE_PCB_INFO);
	str_code_to_html("\",\"chip_temp\":\"");
	temp_to_html();
	str_code_to_html("\",\"flash_size\":\"");
	str_code_to_html(get_flash_size_str());

	if (machine.n_sfp) {
		str_code_to_html("\",\"sfp_slot_0\":\"");
		send_sfp_info(0);
		if (machine.n_sfp == 2) {
			str_code_to_html("\",\"sfp_slot_1\":\"");
			send_sfp_info(1);
		}
	}
	char_to_html('"');

#ifdef SNMP
    {
        static __xdata uint8_t status[SNMP_STATUS_SIZE];
        __xdata volatile uint8_t i;
        snmp_get_status(status);
        str_code_to_html(",\"snmp\":[");
        for (i = 0; i < SNMP_STATUS_SIZE; ++i) {
            if (i) char_to_html(',');
            itoa_html(status[i]);
        }
        char_to_html(']');
    }
#endif
#ifdef PROMETHEUS
	str_code_to_html(",\"prometheus\":");
	bool_to_json(prometheus_enabled);
#endif
#if SOC_FAMILY == RTL836X
	str_code_to_html(",\"mib_family\":\"rtl836x\"");
#endif
#ifdef MACHINE_POE_PORTS
	str_code_to_html(",\"poe_ports\":");
	itoa_html(MACHINE_POE_PORTS);
#endif
	char_to_html('}');
}


void send_vlan(uint16_t vlan)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("sending VLAN\n");
	vlan_get(vlan);
	/* xdata: internal RAM is full in RTL836x SNMP builds */
	__xdata uint16_t members = (((uint16_t)sfr_data[2] << 8) | sfr_data[3]) & 0x3ff;
	__xdata uint16_t untagged = ((((uint16_t)sfr_data[1] << 8) | sfr_data[2]) >> 2) & members;
	members &= ~PMASK_CPU;	/* the arrays list the front-panel ports only */
	str_code_to_html("{\"untagged_members\":");
	pmask_to_json(untagged & members);
	str_code_to_html(",\"tagged_members\":");
	pmask_to_json(~untagged & members);
	str_code_to_html(",\"name\":\"");
	__xdata uint16_t n = vlan_name(vlan);
	if (n== 0xffff) {
		dbg_string("VLAN has no name\n");
	} else {
		while(vlan_names[n] && vlan_names[n] != ' ')
			json_char_to_html(vlan_names[n++]);
	}
	str_code_to_html("\",\"pvid\":");
	members = 0;
	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		if (port_pvid_get(i) == vlan)
			members |= (1 << i);
	}
	pmask_to_json(members);
	char_to_html('}');
}

#if SOC_FAMILY == RTL837X
/* Send counters
 * Only accepts physical port 1..9.
 * Returns an error if the port physical don't exists.
 */
bool send_counters(uint8_t phys_port)
{
	uint8_t phys_port_idx = phys_port - 1;
	if (phys_port_idx > (MACHINE_MAX_PORT - MACHINE_MIN_PORT))
		goto err;
	uint8_t log_port = machine.phys_to_log_port[phys_port_idx];

	dbg_string("send_counters called: "); dbg_byte(phys_port_idx); dbg_char('\n');
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("sending counters\n"); dbg_byte(phys_port_idx);

	char_to_html('[');
	for (uint8_t counter = 0; counter < 0x37; counter++) {
		STAT_GET(counter, log_port);
		str_code_to_html("\"0x");
		counter_to_html();
		char_to_html('\"');
		if (counter != 0x36)
			char_to_html(',');
	}
	char_to_html(']');

	return false;

err:
	dbg_string("Error: counters: phy_port_idx don't exists\n");
	return true;
}


void send_l2(uint16_t idx)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("sending L2\n");
	dbg_short(idx);
	__xdata uint8_t entries_left = L2_MAX_TRANSFER;

	do {
		reg_read_m(RTL837X_TBL_CTRL);
	} while (sfr_data[3] & TBL_EXECUTE);

	/* The L2 table in the ASIC can hold up to 4096 (0x1000) entries, which
	 * are accessed using an index. The index is the hash of the MAC address
	 * and forwarding ID (basically VID). The hash-table is 4-way associative,
	 * i.e. for a given hash value, 4 entries with that same hash can be stored
	 * (i.e. the hash points to a bucket with up to 4 entries).
	 * When the table or a hash bucket is full, further entries will lead to
	 * L2 flooding.
	 * To find all entries, we start with entry-index 0 and iteratively search for
	 * the next entry (with the next higher index), until we arrive again at the first
	 * entry. The indices are sorted, so if an entry has a smaller index than
	 * the previous one, we know that we have wrapped around the entire table.
	 */
	__xdata uint16_t entry = idx & 0xfff;
	__xdata uint16_t first_entry = 0xffff; // An illegal entry index
	__bit first = true;
	char_to_html('[');
	while (1) {
		entries_left--;
		uint8_t port = 0;
		uint8_t lag;
		reg_read_m(RTL837x_TBL_DATA_0);
		REG_WRITE(RTL837x_TBL_DATA_0, sfr_data[0], sfr_data[1] & 0xfc, sfr_data[2] | (TBL_LUTREAD_NEXT_L2UC << 6), sfr_data[3]);

		REG_WRITE(RTL837X_TBL_CTRL, entry >> 8, entry, TBL_L2_UNICAST, TBL_EXECUTE);
		do {
			reg_read_m(RTL837X_TBL_CTRL);
		} while (sfr_data[3] & TBL_EXECUTE);

		reg_read_m(RTL837x_L2_DATA_OUT_B);
		__bit valid = (sfr_data[0] & 0x20) != 0;
		if (valid) {
			/* separator + 81-byte entry + closing "]" */
			if (slen + 83 > TCP_OUTBUF_SIZE)
				break;
			if (!first)
				char_to_html(',');
			first = false;

			// VLAN, taken from the read above instead of reading the register twice
			str_code_to_html("{\"vlan\":\"");
			charhex_to_html(sfr_data[0] & 0x0f);
			byte_to_html(sfr_data[1]);

			// MAC
			str_code_to_html("\",\"mac\":\"");
			byte_to_html(sfr_data[2]); char_to_html(':');
			byte_to_html(sfr_data[3]); char_to_html(':');
			port = (sfr_data[0] >> 6) & 0x3;
			reg_read_m(RTL837x_L2_DATA_OUT_A);
			byte_to_html(sfr_data[0]); char_to_html(':');
			byte_to_html(sfr_data[1]); char_to_html(':');
			byte_to_html(sfr_data[2]); char_to_html(':');
			byte_to_html(sfr_data[3]);

			// type
			reg_read_m(RTL837x_L2_DATA_OUT_C);
			if (sfr_data[1] & 0x1)
				str_code_to_html("\",\"type\":\"s\",\"port\":");
			else
				str_code_to_html("\",\"type\":\"l\",\"port\":");

			port |= (sfr_data[3] & 0x3) << 2;
			itoa_html(port == CPU_LOGICAL_PORT ? CPU_PHYSICAL_PORT : machine.log_to_phys_port[port]);
			str_code_to_html(",\"lag\":");
			lag = port_lag_of(port);
			itoa_html(lag == PORT_LAG_NONE ? 0 : lag + 1);
		}

		// Index
		reg_read_m(RTL837x_TBL_DATA_0);
		entry = (((uint16_t)sfr_data[2] & 0x0f) << 8) | sfr_data[3];
		if (valid) {
			str_code_to_html(",\"idx\":\"");
			byte_to_html(entry >> 8);
			byte_to_html(entry);
			char_to_html('"');
			char_to_html('}');
		}
		entry += 1; // We want the next entry following after the current entry

		if (first_entry == 0xffff)
			first_entry = entry;
		else if (first_entry == entry || !entries_left)
			break;
	}
	char_to_html(']');
}


void l2_delete(uint16_t idx)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("L2 DELETE\n");
	dbg_short(idx);
	do {
		reg_read(RTL837X_TBL_CTRL);
	} while (SFR_DATA_0 & TBL_EXECUTE);
	str_code_to_html("{\"result\":");
	// First, search for the entry based on the index
	reg_read_m(RTL837x_TBL_DATA_0);
	sfr_data[1] &= 0xfc;
	sfr_data[2] |= (TBL_LUTREAD_NEXT_L2UC << 6);
	reg_write_m(RTL837x_TBL_DATA_0);

	REG_WRITE(RTL837X_TBL_CTRL, (idx >> 8) & 0xf, idx, TBL_L2_UNICAST, TBL_EXECUTE);
	do {
		reg_read(RTL837X_TBL_CTRL);
	} while (SFR_DATA_0 & 0x1);
	reg_read_m(RTL837x_TBL_DATA_0);
	/* An aged-out entry must not turn deletion into a next-entry write. */
	if (idx != ((((uint16_t)sfr_data[2] & 0x0f) << 8) | sfr_data[3])) {
		slen += strtox(outbuf + slen, "0}");
		return;
	}
	reg_read_m(RTL837x_L2_DATA_OUT_B);
	if (!(SFR_DATA_24 & 0x20)) {
		char_to_html('0');
	} else {
		__bit is_our_mac_addr = uip_ethaddr.addr[0] == SFR_DATA_8 && uip_ethaddr.addr[1] == SFR_DATA_0;
		sfr_data[0] &= 0x3f; // Clear SPA
		reg_write_m(RTL837x_TBL_DATA_IN_B);

		// Second half of MAC is copied
		reg_read_m(RTL837x_L2_DATA_OUT_A);
		if (is_our_mac_addr && uip_ethaddr.addr[2] == SFR_DATA_24 && uip_ethaddr.addr[3] == SFR_DATA_16
		    && uip_ethaddr.addr[4] == SFR_DATA_8 && uip_ethaddr.addr[5] == SFR_DATA_0) {
			// the switch's own entry keeps management reachable
			char_to_html('0');
		} else {
			reg_write_m(RTL837x_TBL_DATA_IN_A);

			reg_read_m(RTL837x_L2_DATA_OUT_C);
			sfr_data[3] &= 0xc0; // Clear age, auth and second part of ports
			sfr_data[1] &= 0xfe; // Clear nosalearn
			reg_write_m(RTL837x_TBL_DATA_IN_C);

			reg_read_m(RTL837x_TBL_DATA_0);
			sfr_data[2] = TBL_L2_UNICAST;
			reg_write_m(RTL837x_TBL_DATA_0);

			REG_WRITE(RTL837X_TBL_CTRL, idx >> 8, idx, TBL_L2_UNICAST, TBL_WRITE | TBL_EXECUTE);
			do {
				reg_read(RTL837X_TBL_CTRL);
			} while (SFR_DATA_0 & TBL_EXECUTE);

			char_to_html('1');
		}
	}
	char_to_html('}');
}

#else
static void send_json(__code const char *s)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	slen += strtox(outbuf + slen, s);
}

bool send_counters(uint8_t phys_port)
{
	if (!phys_port || phys_port > MACHINE_MAX_PORT - MACHINE_MIN_PORT + 1)
		return true;
	uint8_t port = machine.phys_to_log_port[phys_port - 1];
	__xdata uint8_t value[8];
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (uint8_t counter = 0; counter < RTL836X_MIB_COUNT; counter++) {
		bool available = rtl836x_mib_read(port, counter, value);
		mib_to_html(value, available);
		if (counter + 1 < RTL836X_MIB_COUNT)
			char_to_html(',');
	}
	char_to_html(']');
	return false;
}

/* SDK rtl8367c_getAsicL2LookupTb: status splits address bits 11 and 12. */
static __xdata uint16_t lut_words[6], lut_entry;

/* shortcut: 256 polls; use a timed wait if real LUT operations exceed this bound. */
static bool lut_idle(void)
{
	__xdata uint8_t polls = 0;
	do {
		reg_read(RTL836X_REG_TABLE_LUT_ADDR);
		if (!(SFR_DATA_U16 & TABLE_LUT_ADDR_BUSY))
			return true;
	} while (++polls);
	return false;
}

/* -1 unavailable, 0 absent, 1 unicast entry. */
static int8_t lut_read(__xdata uint16_t idx, __xdata uint8_t command)
{
	__xdata uint8_t i;
	if (idx >= RTL836X_LUT_SIZE)
		return 0;
	if (!lut_idle())
		return -1;
	REG_SET(RTL836X_REG_TABLE_ACCESS_ADDR, idx);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, command);
	if (!lut_idle())
		return -1;
	if (!(SFR_DATA_U16 & TABLE_LUT_HIT))
		return 0;
	lut_entry = (SFR_DATA_U16 & 0x07ff) | ((SFR_DATA_U16 & 0x4000) >> 3) |
		    ((SFR_DATA_U16 & 0x0800) << 1);
	for (i = 0; i < 6; i++) {
		reg_read(RTL836X_REG_TABLE_READ_DATA0 + i);
		lut_words[i] = SFR_DATA_U16;
	}
	if ((lut_words[2] & 0x0100) || (lut_words[3] & 0x1000) ||
	    (!(lut_words[4] & 0x3800) && !(lut_words[5] & 0x0020)))
		return 0;
	return 1;
}

void send_l2(uint16_t idx)
{
	__xdata uint8_t n, b, port, lag;
	__xdata int8_t found;
	if (idx >= RTL836X_LUT_SIZE)
		idx = 0;
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (n = 0; n < L2_MAX_TRANSFER && slen + 96 <= TCP_OUTBUF_SIZE; n++) {
		found = lut_read(idx, TABLE_ACCESS_LUT_NEXT_UC);
		if (found < 0) {
			send_json("null");
			return;
		}
		if (!found || (n && lut_entry < idx))
			break;
		if (n)
			char_to_html(',');
		str_code_to_html("{\"vlan\":\"");
		charhex_to_html((lut_words[3] >> 8) & 0x0f);
		byte_to_html(lut_words[3]);
		str_code_to_html("\",\"mac\":\"");
		for (b = 0; b < 6; b++) {
			if (b)
				char_to_html(':');
			byte_to_html(lut_words[2 - b / 2] >> (b & 1 ? 0 : 8));
		}
		str_code_to_html(lut_words[5] & 0x0020 ? "\",\"type\":\"s\",\"port\":" : "\",\"type\":\"l\",\"port\":");
		port = ((lut_words[4] >> 8) & 7) | ((lut_words[3] >> 12) & 8);
		if (port == RTL836X_CPU_PORT)
			port = CPU_LOGICAL_PORT;
		itoa_html(port == CPU_LOGICAL_PORT ? CPU_PHYSICAL_PORT : machine.log_to_phys_port[port]);
		str_code_to_html(",\"lag\":");
		lag = port_lag_of(port);
		itoa_html(lag == PORT_LAG_NONE ? 0 : lag + 1);
		str_code_to_html(",\"idx\":\"");
		byte_to_html(lut_entry >> 8);
		byte_to_html(lut_entry);
		str_code_to_html("\"}");
		idx = lut_entry + 1;
		if (idx >= RTL836X_LUT_SIZE)
			break;
	}
	char_to_html(']');
}

void l2_delete(uint16_t idx)
{
	__xdata uint8_t i, result = 0;
	if (lut_read(idx, TABLE_ACCESS_LUT_ADDR) != 1 || lut_entry != idx)
		goto done;
	for (i = 0; i < 6; i++)
		if (uip_ethaddr.addr[i] != (uint8_t)(lut_words[2 - i / 2] >> (i & 1 ? 0 : 8)))
			break;
	if (i == 6)
		goto done;
	/* Delete by the full key; clear age/static/SPA and policy, keeping VID/FID/EFID. */
	lut_words[3] &= 0x7fff;
	lut_words[4] &= 0x007f;
	lut_words[5] = 0;
	for (i = 0; i < 6; i++)
		REG_SET(RTL836X_REG_TABLE_WRITE_DATA0 + i, lut_words[i]);
	REG_SET(RTL836X_REG_TABLE_ACCESS_CTRL, TABLE_ACCESS_LUT_WRITE);
	if (lut_idle())
		result = (SFR_DATA_U16 >> 12) & 1;
done:
	send_json(result ? "{\"result\":1}" : "{\"result\":0}");
}
#endif


void send_lag(void)
{
	/* xdata: internal RAM is full in RTL836x SNMP builds */
	__xdata uint8_t l, i, j;

	dbg_string("send_lag called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);

	str_code_to_html("{\"lags\":[");
	for (l = 0; l < PORT_LAG_COUNT; l++) {
		str_code_to_html("{\"lagNum\":");
		itoa_html(l);
		if (lacp_lag_ports[l]) {
			str_code_to_html(",\"type\":\"lacp\",\"members\":");
			pmask_to_json(lacp_lag_ports[l]);
			str_code_to_html(",\"active_members\":");
			pmask_to_json(lacp_members_last[l]);
			str_code_to_html(",\"valid\":");
			bool_to_json(lacp_agg_valid[l]);
			str_code_to_html(",\"aggregator\":\"");
			for (j = 0; j < MAC_ADDR_LEN; j++)
				byte_to_html(lacp_agg_sys[l][j]);
			char_to_html('"');
		} else {
			str_code_to_html(",\"type\":\"static\",\"members\":");
			pmask_to_json(port_lag_members_get(l));
		}
#if SOC_FAMILY == RTL836X
		/* A fixed group takes the ports of its own bank only, as port_lag_members_valid() has it */
		str_code_to_html(",\"range\":");
		pmask_to_json(0x000f << (l << 2));
#endif
		str_code_to_html(",\"hash\":\"000000");
		byte_to_html(port_lag_hash_get(l));
		str_code_to_html("\"},");
	}
	slen -= 1; // remove comma
	str_code_to_html("],\"ports\":[");
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"p\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"lag\":");
		itoa_html(lacp_port_lag[i]);
		str_code_to_html(",\"a\":\"");
		byte_to_html(lacp_actor_state[i]);
		str_code_to_html("\",\"pt\":\"");
		byte_to_html(lacp_partner_state[i]);
		str_code_to_html("\",\"rs\":");
		itoa_html(lacp_rx_state[i]);
		str_code_to_html(",\"rx\":\"");
		byte_to_html(lacp_rx_count[i] >> 8);
		byte_to_html(lacp_rx_count[i]);
		str_code_to_html("\",\"psys\":\"");
		for (j = 0; j < MAC_ADDR_LEN; j++)
			byte_to_html(lacp_partner_sys[i][j]);
		str_code_to_html("\"},");
	}
	slen -= 1; // remove comma
	str_code_to_html("]}");
}

void send_lldp(void)
{
	dbg_string("send_lldp called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);

	str_code_to_html("{\"on\":");
	bool_to_json(lldp_enabled);
	str_code_to_html(",\"tlv\":");
	itoa_html(lldp_tlv_mask);
	str_code_to_html(",\"portStatus\":[");

	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"name\":\"");
		for (uint8_t j = 0; j < PORT_NAME_SIZE && port_names[i][j]; j++) {
			json_char_to_html(port_names[i][j]);
		}

		str_code_to_html("\",\"permitted\":");
		bool_to_json(1 & (lldp_logical_port_status >> i));
		str_code_to_html("}");

		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
	lldp_neighbors_to_html();
	char_to_html('}');
}


static __code const char * __code const storm_json_names[STORM_TYPES] = { "\"bcast\":{\"enabled\":", ",\"mcast\":{\"enabled\":", ",\"ucast\":{\"enabled\":", ",\"umcast\":{\"enabled\":" };

#if SOC_FAMILY == RTL837X
/* EEE_* bits of a copper port; eee_ablty is RTL8373_PHY_EEE_ABLTY, read once per reply */
// Parameters and locals in xdata: internal RAM is full on SNMP builds
static uint16_t port_eee_state(__xdata uint8_t port, __xdata uint16_t eee_ablty)
{
	__xdata uint16_t s = 0;
	__xdata uint16_t v;

	phy_read(port, PHY_MMD_AN, PHY_EEE_ADV);
	v = SFR_DATA_U16;
	if (v & PHY_EEE_BIT_100M) s |= EEE_ADV_100M;
	if (v & PHY_EEE_BIT_1G) s |= EEE_ADV_1G;
	if (v & PHY_EEE_BIT_10G) s |= EEE_ADV_10G;
	phy_read(port, PHY_MMD_AN, PHY_EEE_ADV2);
	v = SFR_DATA_U16;
	if (v & PHY_EEE_BIT_2G5) s |= EEE_ADV_2G5;
	if (v & PHY_EEE_BIT_5G) s |= EEE_ADV_5G;
	phy_read(port, PHY_MMD_AN, PHY_EEE_LP_ABILITY);
	v = SFR_DATA_U16;
	if (v & PHY_EEE_BIT_100M) s |= EEE_LP_100M;
	if (v & PHY_EEE_BIT_1G) s |= EEE_LP_1G;
	if (v & PHY_EEE_BIT_10G) s |= EEE_LP_10G;
	phy_read(port, PHY_MMD_AN, PHY_EEE_LP_ABILITY2);
	v = SFR_DATA_U16;
	if (v & PHY_EEE_BIT_2G5) s |= EEE_LP_2G5;
	if (v & PHY_EEE_BIT_5G) s |= EEE_LP_5G;
	if (eee_ablty & ((uint16_t)1 << port)) s |= EEE_ACTIVE;
	return s;
}


void send_bandwidth(void)
{
	dbg_string("send_bandwidth called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"iLimited\":");
		reg_read_m(RTL837X_IGBW_PORT_CTRL + i * 4);
		bool_to_json(sfr_data[1] & 0x10);
		str_code_to_html(",\"iBW\":\"0x");
		byte_to_html(sfr_data[1] & 0x0f);
		byte_to_html(sfr_data[2]);
		byte_to_html(sfr_data[3]);
		str_code_to_html(reg_bit_test(RTL837X_IGBW_PORT_FC_CTRL, i) ? "\",\"iAction\":\"fc\"" : "\",\"iAction\":\"drop\"");
		reg_read_m(RTL837X_EGBW_PORT_CTRL + i * 1024);
		str_code_to_html(",\"eLimited\":");
		bool_to_json(sfr_data[1] & 0x10);
		str_code_to_html(",\"eBW\":\"0x");
		byte_to_html(sfr_data[1] & 0x0f);
		byte_to_html(sfr_data[2]);
		byte_to_html(sfr_data[3]);
		char_to_html('"');
		char_to_html('}');
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
}


void send_storm(void)
{
	__xdata uint8_t i, t, idx;

	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		char_to_html(',');
		for (t = 0; t < STORM_TYPES; t++) {
			str_code_to_html(storm_json_names[t]);
			bool_to_json(reg_bit_test(RTL837X_STORM_CTRL + (t << 2), i));
			idx = storm_meter(i, t);
			str_code_to_html(reg_bit_test(RTL837X_METER_MODE + ((idx >> 5) << 2), idx & 0x1f)
					 ? ",\"unit\":\"pps\",\"rate\":\"0x" : ",\"unit\":\"kbit/s\",\"rate\":\"0x");
			reg_read_m(RTL837X_METER_RATE + (idx << 2));
			byte_to_html(sfr_data[1]);
			byte_to_html(sfr_data[2]);
			byte_to_html(sfr_data[3]);
			str_code_to_html("\"}");
		}
		char_to_html('}');
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
}


#else
/* EEE_* bits: native advertisement and negotiated LPI; the partner's EEE is not read yet */
static uint16_t port_eee_state(uint8_t port)
{
	__xdata uint16_t s = 0;
	phy_read(port, 0, RTL836X_OCP_EEE_ADV);
	if (SFR_DATA_U16 & 2) s |= EEE_ADV_100M;
	if (SFR_DATA_U16 & 4) s |= EEE_ADV_1G;
	reg_read(RTL836X_REG_PORT0_STATUS + port);
	if ((SFR_DATA_U16 & PORT_STATUS_LINK) && (SFR_DATA_U16 & PORT_STATUS_EEE_CAP))
		s |= EEE_ACTIVE;
	return s;
}

void send_bandwidth(void)
{
	__xdata uint8_t i;
	__xdata uint32_t count;
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		count = rtl836x_bw_get(i, 1);
		str_code_to_html(",\"iLimited\":");
		bool_to_json(count != RTL836X_RATE_OFF);
		str_code_to_html(",\"iBW\":\"0x");
		byte_to_html(count >> 16); byte_to_html(count >> 8); byte_to_html(count);
		str_code_to_html(reg_bit_test(RTL836X_REG_PORT0_MISC_CFG + ((uint16_t)i << 5), 11)
				 ? "\",\"iAction\":\"fc\"" : "\",\"iAction\":\"drop\"");
		count = rtl836x_bw_get(i, 0);
		str_code_to_html(",\"eLimited\":");
		bool_to_json(count != RTL836X_RATE_OFF);
		str_code_to_html(",\"eBW\":\"0x");
		byte_to_html(count >> 16); byte_to_html(count >> 8); byte_to_html(count);
		str_code_to_html("\"}");
		if (i < MACHINE_MAX_PORT) char_to_html(',');
	}
	char_to_html(']');
}

void send_storm(void)
{
	__xdata uint8_t i, t;
	__xdata uint32_t rate;
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		char_to_html(',');
		for (t = 0; t < STORM_TYPES; t++) {
			str_code_to_html(storm_json_names[t]);
			bool_to_json(reg_bit_test(RTL836X_REG_STORM_CTRL0 + t, i));
			str_code_to_html(",\"unit\":\"kbit/s\",\"rate\":\"0x");	/* this family has no packet-rate meters */
			rate = rtl836x_storm_get(i, t);
			byte_to_html(rate >> 16); byte_to_html(rate >> 8); byte_to_html(rate);
			str_code_to_html("\"}");
		}
		char_to_html('}');
		if (i < MACHINE_MAX_PORT) char_to_html(',');
	}
	char_to_html(']');
}
#endif

/* Build-time defaults shared with boot initialization. */
void send_defaults(void)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	str_code_to_html("{\"lldp\":");
	bool_to_json(MACHINE_DEFAULT_LLDP);
	str_code_to_html(",\"ports\":[");
	for (__xdata uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"lldp\":");
		bool_to_json((MACHINE_DEFAULT_LLDP_PORTS >> i) & 1);
		if (!machine.is_sfp[i]) {
			str_code_to_html(",\"en\":true");
			// Mixed-speed boards use different EEE speed defaults at boot and in the CLI.
			if (!machine.n_10g) {
				str_code_to_html(",\"eee\":");
				bool_to_json(MACHINE_DEFAULT_EEE);
			}
		}
		char_to_html('}');
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	str_code_to_html("]}");
}

void send_mirror(void)
{
	__xdata struct port_mirror m;

	dbg_string("send_mirror called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	port_mirror_get(&m);
	str_code_to_html(m.enabled ? "{\"enabled\":true,\"mirrorPort\":" : "{\"enabled\":false,\"mirrorPort\":");
	itoa_html(machine.log_to_phys_port[m.port]);
	str_code_to_html(",\"mirror_rx\":");
	pmask_to_json(m.rx_pmask);
	str_code_to_html(",\"mirror_tx\":");
	pmask_to_json(m.tx_pmask);
	char_to_html('}');
}

void send_isolation(void)
{
	dbg_string("send_isolation called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	char_to_html('[');
	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"allow\":");
		pmask_to_json(port_isolation_get(i));
		char_to_html('}');
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
}


void send_ports(void)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("sending ports\n");
#if SOC_FAMILY == RTL837X
	reg_read_m(RTL8373_PHY_EEE_ABLTY);
	__xdata uint16_t eee_ablty = ((uint16_t)sfr_data[2] << 8) | sfr_data[3];
#endif
	char_to_html('[');
#ifdef MACHINE_POE_PORTS
	__xdata uint8_t poe_enabled = poe_admin_mask();
#endif

	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"name\":\"");
		for (uint8_t j = 0; j < PORT_NAME_SIZE && port_names[i][j]; j++) {
			json_char_to_html(port_names[i][j]);
		}
		char_to_html('"');

		if (machine.is_sfp[i]) {
			uint8_t sfp = machine.is_sfp[i] - 1;
			str_code_to_html(",\"isSFP\":true,\"enabled\":");
			if (!(sfp_pins_last & (0x1 << (sfp << 2)))) {
				bool_to_json(1);
				str_code_to_html(",\"sfp_options\":\"0x");
				byte_to_html(sfp_options[sfp]);
				if (sfp_options[sfp] & 0x40) {
					str_code_to_html("\",\"sfp_temp\":\"0x");
					sfp_send_data(sfp, 224, 2);
					str_code_to_html("\",\"sfp_vcc\":\"0x");
					sfp_send_data(sfp, 226, 2);
					str_code_to_html("\",\"sfp_txbias\":\"0x");
					sfp_send_data(sfp, 228, 2);
					str_code_to_html("\",\"sfp_txpower\":\"0x");
					sfp_send_data(sfp, 230, 2);
					str_code_to_html("\",\"sfp_rxpower\":\"0x");
					sfp_send_data(sfp, 232, 2);
					if (sfp_options[sfp] & 0x10) {
						str_code_to_html("\",\"sfp_temp_cal\":\"0x");
						sfp_send_data(sfp, 212, 4);
						str_code_to_html("\",\"sfp_vcc_cal\":\"0x");
						sfp_send_data(sfp, 216, 4);
						str_code_to_html("\",\"sfp_txbias_cal\":\"0x");
						sfp_send_data(sfp, 204, 4);
						str_code_to_html("\",\"sfp_txpower_cal\":\"0x");
						sfp_send_data(sfp, 208, 4);
						str_code_to_html("\",\"sfp_rxpower_cal\":\"0x");
						sfp_send_data(sfp, 184, 16);
						sfp_send_data(sfp, 200, 4);
					}
					str_code_to_html("\",\"sfp_state\":\"0x");
					sfp_send_data(sfp, 238, 1);
				}
				str_code_to_html("\",\"sfp_vendor\":\"");
				str_x_to_html(sfp_module_vendor[sfp]);
				str_code_to_html("\",\"sfp_model\":\"");
				str_x_to_html(sfp_module_model[sfp]);
				str_code_to_html("\",\"sfp_serial\":\"");
				str_x_to_html(sfp_module_serial[sfp]);
				str_code_to_html("\",\"sfp_los\":");
				if (machine.sfp_port[sfp].pin_los == GPIO_NA) {
					str_code_to_html("null");
				} else {
					bool_to_json(sfp_pins_last & (0x2 << (sfp << 2)));
				}
			} else {
				bool_to_json(0);
			}
		} else {
			str_code_to_html(",\"isSFP\":false,\"enabled\":");
			uint16_t w;
#if SOC_FAMILY == RTL836X
			phy_read(i, 0, 0);			// MII BMCR
			bool_to_json(!(SFR_DATA_8 & 0x08));	// not powered down
			str_code_to_html(",\"adv\":[");		// no 2500Base
#else
			phy_read(i, PHY_MMD31, 0xa610);
			bool_to_json(SFR_DATA_8 == 0x20);
			str_code_to_html(",\"adv\":[");
			phy_read(i, PHY_MMD_AN, PHY_ANEG_MGBASE_CTRL);
			w = SFR_DATA_U16;
			if (w & 0x80)				// 2500BaseN-Full
				json_add_array_string_item("\"2.5G\"");
#endif
			phy_read(i, PHY_MMD31, PHY_MMD31_GBCR);
			w = SFR_DATA_U16;
			if (w & 0x0200)				// 1000Base-Full
				json_add_array_string_item("\"1G\"");
#if SOC_FAMILY == RTL836X
			phy_read(i, 0, 4);			// MII ANAR
#else
			phy_read(i, PHY_MMD_AN, PHY_ANEG_ADV);
#endif
			w = SFR_DATA_U16;
			if (w & 0x0100)
				json_add_array_string_item("\"100M-FD\"");
			if (w & 0x80)
				json_add_array_string_item("\"100M-HD\"");
			if (w & 0x40)
				json_add_array_string_item("\"10M-FD\"");
			if (w & 0x20)
				json_add_array_string_item("\"10M-HD\"");
			char_to_html(']');
		}

		str_code_to_html(",\"mtu\":\"0x");
		__xdata uint16_t mtu = port_mtu_get(i);
		byte_to_html(mtu >> 8);
		byte_to_html(mtu & 0xff);
		// Pause and EEE flags: the FC_* and EEE_* bits of rtl837x_pause.h and page_impl.h
		str_code_to_html("\",\"fc\":");
		itoa_html(port_pause_state(i));
#if SOC_FAMILY == RTL836X
		str_code_to_html(",\"eee\":");
		itoa16_html(port_eee_state(i));
#else
		if (!machine.is_sfp[i]) {
			str_code_to_html(",\"eee\":");
			itoa16_html(port_eee_state(i, eee_ablty));
		}
#endif
#ifdef MACHINE_POE_PORTS
		uint8_t phys_port = machine.log_to_phys_port[i];
		if (phys_port && phys_port <= MACHINE_POE_PORTS) {
			str_code_to_html(",\"poe_admin\":");
			bool_to_json(poe_enabled & (1 << (phys_port - 1)));
			str_code_to_html(",\"poe_on\":null");	// no delivery telemetry yet
		}
#endif
		char_to_html('}');
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
}


#if SOC_FAMILY == RTL837X
static __code const char * __code const link_names[7] = { "10M", "100M", "1000M", "500M", "10G", "2.5G", "5G" };
#else
static __code const char * __code const link_names[3] = { "10M", "100M", "1000M" };
#endif

void send_traffic(void)
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	dbg_string("sending port counters\n");
	char_to_html('[');

	for (uint8_t i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		str_code_to_html("{\"port\":");
		itoa_html(machine.log_to_phys_port[i]);
		str_code_to_html(",\"link\":\"");
#if SOC_FAMILY == RTL836X
		reg_read(RTL836X_REG_PORT0_STATUS + i);
		uint16_t status = SFR_DATA_U16;
		if ((status & PORT_STATUS_LINK) && (status & PORT_STATUS_SPEED_MASK) < 3)
			str_code_to_html(link_names[status & PORT_STATUS_SPEED_MASK]);
		else
			str_code_to_html("DOWN");
		char_to_html('"');
		__xdata uint8_t value[8];
		for (uint8_t metric = 0; metric < 4; metric++) {
			switch (metric) {
			case 0: str_code_to_html(",\"txG\":"); break;
			case 1: str_code_to_html(",\"txB\":"); break;
			case 2: str_code_to_html(",\"rxG\":"); break;
			default: str_code_to_html(",\"rxB\":"); break;
			}
			bool available = rtl836x_mib_summary(i, metric, value);
			mib_to_html(value, available);
		}
		str_code_to_html(",\"macs\":null}");
#else
		uint8_t b = 7;	// link down
		reg_read_m(RTL837X_REG_LINKS_STS);
		if ((sfr_data[(i / 8) + 1] >> (i % 8)) & 1) {
			if (i < 8)
				reg_read_m(RTL837X_REG_LINKS);
			else
				reg_read_m(RTL837X_REG_LINKS_89);
			b = sfr_data[3 - ((i & 7) >> 1)];
			b = (i & 1) ? b >> 4 : b & 0xf;
		}
		str_code_to_html(b < 7 ? link_names[b] : "DOWN");

		STAT_GET(STAT_COUNTER_TX_PKTS, i);
		str_code_to_html("\",\"txG\":\"0x");
		counter_to_html();

		str_code_to_html("\",\"txB\":\"0x");
		STAT_GET(STAT_COUNTER_ERR_PKTS, i);
		reg_to_html(RTL837X_STAT_V_LOW);	// 32 bit Tx Packet errors

		str_code_to_html("\",\"rxG\":\"0x");
		STAT_GET(STAT_COUNTER_RX_PKTS, i);
		counter_to_html();

		str_code_to_html("\",\"rxB\":\"0x");
		STAT_GET(STAT_COUNTER_ERR_PKTS, i);
		reg_to_html(RTL837X_STAT_V_HIGH);	// 32bit RX packet errors

		str_code_to_html("\",\"macs\":\"0x");
		reg_to_html(RTL837X_L2_LRN_PORT_CONSTRT_CNT + (i << 2));
		str_code_to_html("\"}");
#endif
		if (i < MACHINE_MAX_PORT)
			char_to_html(',');
	}
	char_to_html(']');
}


void send_config(void)
{
	dbg_string("send_config called\n");
	__xdata uint32_t pos = CONFIG_START;
	__xdata uint16_t valid_len = 0;
	__xdata uint16_t len_left = CONFIG_LEN;
	__xdata uint8_t flash_buf[256];
	
	slen = strtox(outbuf, HTTP_RESPONSE_TXT);
	
	// Scan through config to find the end of valid data (null terminator)
	do {
		flash_region.addr = pos;
		flash_region.len = (len_left > 256) ? 256 : len_left;
		flash_read_bulk(flash_buf);
		
		// Look for null terminator in this chunk
		for (uint16_t i = 0; i < flash_region.len; i++) {
			if (flash_buf[i] == 0) {
				// Found end of valid data
				valid_len += i;
				goto found_end;
			}
		}
		
		valid_len += flash_region.len;
		len_left -= flash_region.len;
		pos += flash_region.len;
	} while (len_left > 0);
	
found_end:
	// Now send the valid data
	if (valid_len > (TCP_OUTBUF_SIZE - slen)) {
		cont_len = valid_len - (TCP_OUTBUF_SIZE - slen);
		valid_len = TCP_OUTBUF_SIZE - slen;
		cont_addr = CONFIG_START + valid_len;
	}
	
	flash_region.addr = CONFIG_START;
	flash_region.len = valid_len;
	flash_read_bulk(outbuf + slen);
	slen += valid_len;
}

void send_cmd_log(void)
{
	dbg_string("send_cmd_log called\n");
	slen = strtox(outbuf, HTTP_RESPONSE_TXT);
	__xdata uint16_t p = (cmd_history_ptr + 1) & CMD_HISTORY_MASK;
	__xdata uint8_t found_begin = 0;
	dbg_string("History ptr: ");
	dbg_short(cmd_history_ptr); dbg_char('\n');
	while (p != cmd_history_ptr && slen < TCP_OUTBUF_SIZE) {
		if (!cmd_history[p] || cmd_history[p] == '\n')
			found_begin = 1;
		if (found_begin && cmd_history[p])
			outbuf[slen++] = cmd_history[p];
		p = (p + 1) & CMD_HISTORY_MASK;
	}
}


void send_vlanlist(void)
{
	/* Worst case per entry: {"id":4094,"name":"<117-char name>"} = 138 bytes
	 * (name bound: CMD_BUF_SIZE=128 minus command prefix); +1 for closing ']'.
	 * At worst case ~18 VLANs fit; typical configs with short names fit many more. */
	__xdata uint16_t i;
	__xdata uint16_t n;
	uint8_t first = 1;

	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	str_code_to_html("{\"mgmt\":");
	itoa16_html(management_vlan);
	str_code_to_html(",\"vlan\":[");

	for (i = 1; i < 4095; i++) {
		vlan_get(i);
		if (!(sfr_data[0] & 0x02)) /* bit 1: VLAN table entry valid */
			continue;

		if (slen + 141 > TCP_OUTBUF_SIZE) /* comma + 138-byte worst-case entry + closing "]}" */
			break;

		if (!first)
			char_to_html(',');
		first = 0;

		str_code_to_html("{\"id\":");
		itoa16_html(i);
		str_code_to_html(",\"name\":\"");

		n = vlan_name(i);
		if (n != 0xffff) {
			while (vlan_names[n] && vlan_names[n] != ' ')
				json_char_to_html(vlan_names[n++]);
		}

		str_code_to_html("\"}");

	}

	char_to_html(']');
	char_to_html('}');
}
