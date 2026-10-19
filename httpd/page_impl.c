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
#include "page_impl.h"
#include "syslog.h"

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
extern __code const uint8_t * __code const hex;
extern __xdata uip_ipaddr_t uip_hostaddr, uip_draddr, uip_netmask;

extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];

extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;

extern __xdata char sfp_module_vendor[2][17];
extern __xdata char sfp_module_model[2][17];
extern __xdata char sfp_module_serial[2][17];
extern __xdata uint8_t sfp_options[2];

__code const uint8_t * __code const HTTP_RESPONCE_JSON = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n";
__code const uint8_t * __code const HTTP_RESPONCE_TXT = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n";

// Convert uint8_t to ascii HEX char push on html-buffer.
void charhex_to_html(char c)
{
	outbuf[slen++] = itohex(c);
}


// Convert (uint8_t) bool to ascii '0' or '1' char push on html-buffer.
void bool_to_html(char c)
{
	outbuf[slen++] = c ? '1' : '0';
}


void char_to_html(char c)
{
	outbuf[slen++] = c;
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


//  Convert uint8_t to ascii HEX char.
void byte_to_html(uint8_t val)
{
	uint8_t cnt = 2;
	do {
		val = (val >> 4) | (val << 4);
		charhex_to_html(val);
		cnt -= 1;
	} while(cnt);
}

/* Converts a uint8_t to raw string.
   Suppress leading zeros.
*/
void itoa_html(uint8_t v)
{
	uint8_t t = (v / 100);
	// when print_zeros is not zero, we know that a non-zero number has printed.
	// That have to print all the next numbers.
	uint8_t print_zeros = t;
	if (print_zeros)
		char_to_html('0' + t);
	t = (v / 10) % 10;
	print_zeros |= t;
	if (print_zeros)
		char_to_html('0' + t);
	char_to_html('0' + (v % 10));
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

void string_to_html(__code const char *s)
{
	while (*s) char_to_html(*s++);
}

static void temp_to_html(void)
{
	int16_t v = chip_temp_get(0);

	if (v < 0) {
		char_to_html('-');
		v = -v;
	}
	itoa_html(v >> 7);
	char_to_html('.');
	itoa_html(((v & 0x7f) * 10) >> 7);
	char_to_html(' ');
	char_to_html('C');
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


/* Converts big-endian bytes into raw hex string, n nibbles (2 per byte) of them.
   Suppress leading zeros, so a 64-bit counter (n = 16) keeps the low word's zeros
   exactly when the high word is set.
   The counter and MAC-count getters leave their result in port_buf.
*/
void hex_to_html(__xdata uint8_t *p, __xdata uint8_t n)
{
 	uint8_t print_zeros = 0;
	uint8_t val = 0;

	for (uint8_t nibble = 0; nibble < n; nibble++) {
	  	if (!(nibble & 1))
	        val = p[nibble>>1];
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


// One '0'/'1' per bit of v below its highest set bit (a marker the caller ORs in).
// Single parameter: a second one on a static function costs a byte of internal RAM.
static void bits_to_html(uint8_t v)
{
	uint8_t m = 0x80;

	while (!(v & m))
		m >>= 1;
	while (m >>= 1)
		bool_to_html(v & m);
}


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
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("send_basic_info called\n");
	slen += strtox(outbuf + slen, "{\"ip_address\":\"");
	itoa_html(uip_hostaddr[0]); char_to_html('.');
	itoa_html(uip_hostaddr[0] >> 8); char_to_html('.');
	itoa_html(uip_hostaddr[1]); char_to_html('.');
	itoa_html(uip_hostaddr[1] >> 8);
	slen += strtox(outbuf + slen, "\",\"ip_gateway\":\"");
	itoa_html(uip_draddr[0]); char_to_html('.');
	itoa_html(uip_draddr[0] >> 8); char_to_html('.');
	itoa_html(uip_draddr[1]); char_to_html('.');
	itoa_html(uip_draddr[1] >> 8);
	slen += strtox(outbuf + slen, "\",\"ip_netmask\":\"");
	itoa_html(uip_netmask[0]); char_to_html('.');
	itoa_html(uip_netmask[0] >> 8); char_to_html('.');
	itoa_html(uip_netmask[1]); char_to_html('.');
	itoa_html(uip_netmask[1] >> 8);
	slen += strtox(outbuf + slen, "\",\"syslog_server\":\"");
	itoa_html(syslog_state.server_ip[0]); char_to_html('.');
	itoa_html(syslog_state.server_ip[1]); char_to_html('.');
	itoa_html(syslog_state.server_ip[2]); char_to_html('.');
	itoa_html(syslog_state.server_ip[3]); char_to_html(':');
	itoa16_html(syslog_state.server_port);
	slen += strtox(outbuf + slen, "\",\"session_timeout\":");
	itoa16_html(session_timeout);
	slen += strtox(outbuf + slen, ",\"mac_address\":\"");
	byte_to_html(uip_ethaddr.addr[0]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[1]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[2]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[3]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[4]); char_to_html(':');
	byte_to_html(uip_ethaddr.addr[5]);
	slen += strtox(outbuf + slen, "\",\"hostname\":\"");
	{
		__xdata char *hp = hostname;	/* sanitized on ingest, emit verbatim */
		while (*hp)
			char_to_html(*hp++);
	}
	slen += strtox(outbuf + slen, "\",\"sw_ver\":\"");
	slen += strtox(outbuf + slen, VERSION_SW);
	slen += strtox(outbuf + slen, "\",\"build_date\":\"");
	slen += strtox(outbuf + slen, BUILD_DATE);
	slen += strtox(outbuf + slen, "\",\"hw_ver\":\"");
	slen += strtox(outbuf + slen, machine.machine_name);
	slen += strtox(outbuf + slen, "\",\"chip_temp\":\"");
	temp_to_html();
	slen += strtox(outbuf + slen, "\",\"flash_size\":\"");
	string_to_html(get_flash_size_str());

	if (machine.n_sfp) {
		slen += strtox(outbuf + slen, "\",\"sfp_slot_0\":\"");
		send_sfp_info(0);
		if (machine.n_sfp == 2) {
			slen += strtox(outbuf + slen, "\",\"sfp_slot_1\":\"");
			send_sfp_info(1);
		}
	}
	char_to_html('"');

	char_to_html('}');
}


void send_vlan(uint16_t vlan)
{
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("sending VLAN\n");
	//{"members":"0x00060011"}
	slen += strtox(outbuf + slen, "{\"members\":\"0x");
	vlan_entry_get(vlan);
	hex_to_html(port_buf, 8);
	slen += strtox(outbuf + slen, "\",\"name\":\"");
	__xdata uint16_t n = vlan_name(vlan);
	if (n== 0xffff) {
		dbg_string("VLAN has no name\n");
	} else {
		while(vlan_names[n] && vlan_names[n] != ' ')
			json_char_to_html(vlan_names[n++]);
	}
	slen += strtox(outbuf + slen, "\",\"pvid\":\"0x");
	uint16_t pvid_mask = 0;
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		if (port_pvid_get(i) == vlan)
			pvid_mask |= (1 << i);
}
	byte_to_html(pvid_mask >> 8);
	byte_to_html(pvid_mask);
	slen += strtox(outbuf + slen, "\"}");
}

/* Send counters
 * Only accepts physical port 1..9.
 * Returns an error if the port physical don't exists.
 */
bool send_counters(uint8_t phys_port)
{
	uint8_t phys_port_idx = phys_port - 1;
	if (phys_port_idx > (machine.max_port - machine.min_port))
		goto err;
	uint8_t log_port = machine.phys_to_log_port[phys_port_idx];

	dbg_string("send_counters called: "); dbg_byte(phys_port_idx); dbg_char('\n');
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("sending counters\n"); dbg_byte(phys_port_idx);

	char_to_html('[');
	for (uint8_t counter = 0; counter < 0x37; counter++) {
		port_counter_get(log_port, counter);
		slen += strtox(outbuf + slen, "\"0x");
		hex_to_html(port_buf, 16);
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
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("sending L2\n");
	dbg_short(idx);
	__xdata uint8_t entries_left = L2_MAX_TRANSFER;

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
	__xdata uint16_t first_entry = 0xffff; // An illegal entry index
	__bit first = true;
	l2_buf.idx = idx & 0xfff;
	char_to_html('[');
	while (1) {
		entries_left--;
		uint8_t lag;
		__bit valid = l2_entry_next();
		if (valid) {
			/* separator + 74-byte worst-case entry + closing "]" */
			if (slen + 76 > TCP_OUTBUF_SIZE)
				break;
			if (!first)
				char_to_html(',');
			first = false;

			slen += strtox(outbuf + slen, "{\"vlan\":\"");
			charhex_to_html(l2_buf.vid >> 8);
			byte_to_html(l2_buf.vid);

			slen += strtox(outbuf + slen, "\",\"mac\":\"");
			for (uint8_t i = 0; i < 6; i++) {
				if (i)
					char_to_html(':');
				byte_to_html(l2_buf.mac[i]);
			}

			if (l2_buf.is_static)
				slen += strtox(outbuf + slen, "\",\"type\":\"s\",\"port\":");
			else
				slen += strtox(outbuf + slen, "\",\"type\":\"l\",\"port\":");

			itoa_html(l2_buf.port);
			slen += strtox(outbuf + slen, ",\"lag\":");
			lag = port_lag_of(l2_buf.port);
			itoa_html(lag == PORT_LAG_NONE ? 0 : lag + 1);

			slen += strtox(outbuf + slen, ",\"idx\":\"");
			byte_to_html(l2_buf.idx >> 8);
			byte_to_html(l2_buf.idx);
			char_to_html('"');
			char_to_html('}');
		}
		l2_buf.idx += 1; // We want the next entry following after the current entry

		if (first_entry == 0xffff)
			first_entry = l2_buf.idx;
		else if (first_entry == l2_buf.idx || !entries_left)
			break;
	}
	char_to_html(']');
}


void l2_delete(uint16_t idx)
{
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("L2 DELETE\n");
	dbg_short(idx);
	slen += strtox(outbuf + slen, "{\"result\":");
	char_to_html('0' + l2_entry_delete(idx));
	char_to_html('}');
}


void send_mirror(void)
{
	dbg_string("send_mirror called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	port_mirror_get();
	if (port_buf[0]) {
		slen += strtox(outbuf + slen, "{\"enabled\":1,\"mPort\":");
	} else {
		slen += strtox(outbuf + slen, "{\"enabled\":0,\"mPort\":");
	}
	itoa_html(machine.log_to_phys_port[port_buf[1]]);

	uint16_t m = port_buf[2];
	m = (m << 8) | port_buf[3];
	slen += strtox(outbuf + slen, ",\"mirror_rx\":\"");
	for (uint8_t i = 0; i < 16; i++) {
		bool_to_html(!!(m & 0x8000));
		m <<= 1;
	}
	m = port_buf[4];
	m = (m << 8) | port_buf[5];
	slen += strtox(outbuf + slen, "\",\"mirror_tx\":\"");
	for (uint8_t i = 0; i < 16; i++) {
		bool_to_html(!!(m & 0x8000));
		m <<= 1;
	}
	char_to_html('\"');
	char_to_html('}');
}


void send_lag(void)
{
	dbg_string("send_lag called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	char_to_html('[');
	for (uint8_t l=0; l < 4; l++) {
		slen += strtox(outbuf + slen, "{\"lagNum\":");
		itoa_html(l);
		slen += strtox(outbuf + slen, ",\"members\":\"");
		uint16_t ports = port_lag_members_get(l);
		for (uint8_t i = 0; i < 16; i++) {
			bool_to_html(!!(ports & 0x8000));
			ports <<= 1;
		}
		slen += strtox(outbuf + slen, "\",\"hash\":\"");
		port_lag_hash_get(l);
		hex_to_html(port_buf, 8);
		slen += strtox(outbuf + slen, "\"},");
	}
	slen -=1; // remove comma
	char_to_html(']');
}


static __xdata uint32_t pi_u32;
static __xdata uint8_t pi_prio, pi_ext;
static __xdata uint8_t * __xdata pi_mac;


static void u32hex_html(void)
{
	__xdata uint8_t *b = (__xdata uint8_t *)&pi_u32;
	byte_to_html(b[3]);
	byte_to_html(b[2]);
	byte_to_html(b[1]);
	byte_to_html(b[0]);
}


static void bridge_to_html(void)
{
	byte_to_html(pi_prio);
	byte_to_html(pi_ext);
	for (uint8_t i = 0; i < 6; i++)
		byte_to_html(pi_mac[i]);
}

void send_stp(void)
{
	uint8_t i, j, st, dsg;

	dbg_string("send_stp called\n");
	stp_status_refresh();
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	slen += strtox(outbuf + slen, "{\"on\":");
	bool_to_html(stp_enabled);
	slen += strtox(outbuf + slen, ",\"rstp\":");
	bool_to_html(stp_rstp);
	slen += strtox(outbuf + slen, ",\"prio\":");
	itoa_html(stp_prio >> 4);
	slen += strtox(outbuf + slen, ",\"hello\":");
	itoa_html(stp_hello_s);
	slen += strtox(outbuf + slen, ",\"maxage\":");
	itoa_html(stp_maxage_s);
	slen += strtox(outbuf + slen, ",\"fwd\":");
	itoa_html(stp_fwddelay_s);
	slen += strtox(outbuf + slen, ",\"txhold\":");
	itoa_html(stp_txhold);
	slen += strtox(outbuf + slen, ",\"rootPrio\":\"");
	byte_to_html(root_bridge.prio);
	byte_to_html(root_bridge.ext);
	slen += strtox(outbuf + slen, "\",\"rootMac\":\"");
	for (j = 0; j < 6; j++)
		byte_to_html(root_bridge.mac[j]);
	slen += strtox(outbuf + slen, "\",\"myMac\":\"");
	for (j = 0; j < 6; j++)
		byte_to_html(uip_ethaddr.addr[j]);
	slen += strtox(outbuf + slen, "\",\"cost\":\"");
	byte_to_html(root_bridge_cost >> 24);
	byte_to_html(root_bridge_cost >> 16);
	byte_to_html(root_bridge_cost >> 8);
	byte_to_html(root_bridge_cost);
	slen += strtox(outbuf + slen, "\",\"weRoot\":");
	bool_to_html(stp_root_port == 0xff ? 1 : 0);
	slen += strtox(outbuf + slen, ",\"rootPort\":");
	j = stp_root_port;
	if (j != 0xff && j >= STP_LAG_BASE) {
		j = 0;
		while (j < STP_LAG_BASE && !((stp_lag_mask[stp_root_port - STP_LAG_BASE] >> j) & 1))
			j++;
		if (j >= STP_LAG_BASE)
			j = 0;
	}
	itoa_html(j == 0xff ? 0 : machine.log_to_phys_port[j]);
	slen += strtox(outbuf + slen, ",\"tc\":\"");
	byte_to_html(stp_tc_count >> 8);
	byte_to_html(stp_tc_count);
	slen += strtox(outbuf + slen, "\",\"ports\":[");
	for (i = 0; i < STP_ENTITIES; i++) {
		if (i < STP_LAG_BASE && (i < machine.min_port || i > machine.max_port))
			continue;
		if (i < STP_LAG_BASE && stp_ent_of[i] != i)
			continue;
		if (i >= STP_LAG_BASE && !stp_lag_mask[i - STP_LAG_BASE])
			continue;
		slen += strtox(outbuf + slen, "{\"p\":");
		itoa_html(i < STP_LAG_BASE ? machine.log_to_phys_port[i] : 0);
		slen += strtox(outbuf + slen, ",\"lag\":");
		itoa_html(i < STP_LAG_BASE ? 0 : i - STP_LAG_BASE + 1);
		slen += strtox(outbuf + slen, ",\"mbr\":");
		itoa16_html(i < STP_LAG_BASE ? 0 : stp_lag_mask[i - STP_LAG_BASE]);
		slen += strtox(outbuf + slen, ",\"st\":");
		j = i;
		if (i >= STP_LAG_BASE) {
			j = 0;
			while (j < STP_LAG_BASE && !((stp_lag_mask[i - STP_LAG_BASE] >> j) & 1))
				j++;
			if (j >= STP_LAG_BASE)
				j = 0;
		}
		st = stp_port_state_get(j);
		itoa_html(st);
		slen += strtox(outbuf + slen, ",\"role\":");
		if (!(stp_pflags[i] & STP_PF_ENABLED) || (stp_pflags[i] & STP_PF_TRIPPED))
			itoa_html(0);
		else if (i == stp_root_port)
			itoa_html(1);
		else if (st == 3)
			itoa_html(2);
		else
			itoa_html(3);
		slen += strtox(outbuf + slen, ",\"f\":");
		itoa_html(stp_pflags[i]);
		slen += strtox(outbuf + slen, ",\"pc\":\"");
		pi_u32 = stp_pcost[i]; u32hex_html();
		slen += strtox(outbuf + slen, "\",\"prio\":");
		itoa_html(stp_pprio[i]);
		slen += strtox(outbuf + slen, ",\"p2\":");
		itoa_html(stp_pp2p[i]);
		dsg = stp_dpid[i] && stp_bpdu_age[i] < (uint16_t)stp_maxage_s * STP_HZ;
		slen += strtox(outbuf + slen, ",\"db\":\"");
		if (dsg) {
			pi_prio = stp_dbridge[i].prio; pi_ext = stp_dbridge[i].ext;
			pi_mac = stp_dbridge[i].mac;
		} else {
			pi_prio = stp_prio; pi_ext = 0;
			pi_mac = uip_ethaddr.addr;
		}
		bridge_to_html();
		slen += strtox(outbuf + slen, "\",\"dp\":\"");
		byte_to_html(dsg ? (stp_dpid[i] >> 8) : stp_pprio[i]);
		byte_to_html(dsg ? stp_dpid[i] : (i + 1));
		slen += strtox(outbuf + slen, "\",\"dc\":\"");
		pi_u32 = dsg ? stp_dcost[i] : root_bridge_cost; u32hex_html();
		slen += strtox(outbuf + slen, "\"},");
	}
	slen -= 1; // remove comma
	slen += strtox(outbuf + slen, "]}");
}

void send_lacp(void)
{
	dbg_string("send_lacp called\n");
	lacp_status_refresh();
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	slen += strtox(outbuf + slen, "{\"on\":");
	bool_to_html(lacpEnabled);
	slen += strtox(outbuf + slen, ",\"lags\":[");
	for (uint8_t l = 0; l < LACP_NUM_LAGS; l++) {
		slen += strtox(outbuf + slen, "{\"cfg\":\"");
		byte_to_html(lacp_lag_ports[l] >> 8);
		byte_to_html(lacp_lag_ports[l]);
		slen += strtox(outbuf + slen, "\",\"aggValid\":");
		bool_to_html(lacp_agg_valid[l]);
		slen += strtox(outbuf + slen, ",\"agg\":\"");
		for (uint8_t j = 0; j < MAC_ADDR_LEN; j++)
			byte_to_html(lacp_agg_sys[l][j]);
		slen += strtox(outbuf + slen, "\",\"members\":\"");
		byte_to_html(lacp_members_last[l] >> 8);
		byte_to_html(lacp_members_last[l]);
		slen += strtox(outbuf + slen, "\"},");
	}
	slen -= 1; // remove comma
	slen += strtox(outbuf + slen, "],\"ports\":[");
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"p\":");
		itoa_html(machine.log_to_phys_port[i]);
		slen += strtox(outbuf + slen, ",\"lag\":");
		itoa_html(lacp_port_lag[i]);
		slen += strtox(outbuf + slen, ",\"a\":\"");
		byte_to_html(lacp_actor_state[i]);
		slen += strtox(outbuf + slen, "\",\"pt\":\"");
		byte_to_html(lacp_partner_state[i]);
		slen += strtox(outbuf + slen, "\",\"rs\":");
		itoa_html(lacp_rx_state[i]);
		slen += strtox(outbuf + slen, ",\"rx\":\"");
		byte_to_html(lacp_rx_count[i] >> 8);
		byte_to_html(lacp_rx_count[i]);
		slen += strtox(outbuf + slen, "\",\"psys\":\"");
		for (uint8_t j = 0; j < MAC_ADDR_LEN; j++)
			byte_to_html(lacp_partner_sys[i][j]);
		slen += strtox(outbuf + slen, "\"},");
	}
	slen -= 1; // remove comma
	slen += strtox(outbuf + slen, "]}");
}

void send_lldp(void)
{
	dbg_string("send_lldp called\n");
	lldp_status_refresh();
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	slen += strtox(outbuf + slen, "{\"on\":");
	bool_to_html(lldp_enabled);
	slen += strtox(outbuf + slen, ",\"portStatus\":[");

	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);

		slen += strtox(outbuf + slen, ",\"logPort\":");
		itoa_html(i);
		slen += strtox(outbuf + slen, ",\"name\":\"");
		for (uint8_t j = 0; j < PORT_NAME_SIZE && port_names[i][j]; j++) {
			json_char_to_html(port_names[i][j]);
		}

		slen += strtox(outbuf + slen, "\",\"permitted\":");
		bool_to_html(1 & (lldp_logical_port_status >> i));
		slen += strtox(outbuf + slen, "}");

		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
	char_to_html('}');
}

void send_eee(void)
{
	dbg_string("send_eee called\nsending EEE status\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);

	char_to_html('[');
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);

		if (machine.is_sfp[i]) {
			slen += strtox(outbuf + slen, ",\"isSFP\":1");
		} else {
			slen += strtox(outbuf + slen, ",\"isSFP\":0,\"eee\":\"");
			bits_to_html(port_eee_adv_get(i) | 8);
			slen += strtox(outbuf + slen, "\",\"eee_lp\":\"");
			bits_to_html(port_eee_lp_get(i) | 8);
			slen += strtox(outbuf + slen, "\",\"active\":");
			bool_to_html(port_eee_active(i));
		}
		char_to_html('}');
		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
}


void send_bandwidth(void)
{
	dbg_string("send_bandwidth called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	char_to_html('[');
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);
		slen += strtox(outbuf + slen, ",\"iLimited\":");
		bandwidth_get(i, 0);
		bool_to_html(port_buf[0]);
		slen += strtox(outbuf + slen, ",\"iBW\":\"");
		byte_to_html(port_buf[1]);
		byte_to_html(port_buf[2]);
		byte_to_html(port_buf[3]);
		slen += strtox(outbuf + slen, "\",\"iFC\":");
		bool_to_html(port_buf[4]);
		bandwidth_get(i, 1);
		slen += strtox(outbuf + slen, ",\"eLimited\":");
		bool_to_html(port_buf[0]);
		slen += strtox(outbuf + slen, ",\"eBW\":\"");
		byte_to_html(port_buf[1]);
		byte_to_html(port_buf[2]);
		byte_to_html(port_buf[3]);
		char_to_html('"');
		char_to_html('}');
		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
}


void send_storm(void)
{
	__xdata uint8_t i, t, j;

	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	char_to_html('[');
	for (i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);
		storm_get(i);
		slen += strtox(outbuf + slen, ",\"en\":\"");
		for (t = 0; t < STORM_TYPES; t++)
			bool_to_html(port_buf[t]);
		slen += strtox(outbuf + slen, "\",\"pps\":\"");
		for (t = 0; t < STORM_TYPES; t++)
			bool_to_html(port_buf[4 + t]);
		slen += strtox(outbuf + slen, "\",\"rate\":\"");
		for (t = 0; t < STORM_TYPES; t++)
			for (j = 1; j < 4; j++)
				byte_to_html(port_buf[8 + (t << 2) + j]);
		char_to_html('"');
		char_to_html('}');
		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
}


void send_isolation(void)
{
	__xdata uint16_t allow;

	dbg_string("send_isolation called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	char_to_html('[');
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);
		slen += strtox(outbuf + slen, ",\"allow\":\"");
		allow = port_isolation_get(i);
		byte_to_html(allow >> 8);
		byte_to_html(allow & 0xff);
		char_to_html('"');
		char_to_html('}');
		if (i < machine.max_port)
			char_to_html(',');
	}
	char_to_html(']');
}


void send_mtu(void)
{
	dbg_string("send_mtu called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	char_to_html('[');
	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);
		slen += strtox(outbuf + slen, ",\"mtu\":\"0x");
		uint16_t mtu = port_mtu_get(i);
		byte_to_html(mtu >> 8);
		byte_to_html(mtu & 0xff);
		char_to_html('"');
		char_to_html('}');
		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
}


void send_status(void)
{
	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	dbg_string("sending status\n");
	char_to_html('[');

	for (uint8_t i = machine.min_port; i <= machine.max_port; i++) {
		slen += strtox(outbuf + slen, "{\"portNum\":");
		itoa_html(machine.log_to_phys_port[i]);
		slen += strtox(outbuf + slen, ",\"logPort\":");
		itoa_html(i);
		slen += strtox(outbuf + slen, ",\"name\":\"");
		for (uint8_t j = 0; j < PORT_NAME_SIZE && port_names[i][j]; j++) {
			json_char_to_html(port_names[i][j]);
		}
		slen += strtox(outbuf + slen, "\"");

		if (machine.is_sfp[i]) {
			uint8_t sfp = machine.is_sfp[i] - 1;
			slen += strtox(outbuf + slen, ",\"isSFP\":1,\"enabled\":");
			if (sfp_present(sfp)) {
				bool_to_html(1);
				slen += strtox(outbuf + slen,",\"sfp_options\":\"0x");
				byte_to_html(sfp_options[sfp]);
				if (sfp_options[sfp] & 0x40) {
					slen += strtox(outbuf + slen,"\",\"sfp_temp\":\"0x");
					sfp_send_data(sfp, 224, 2);
					slen += strtox(outbuf + slen,"\",\"sfp_vcc\":\"0x");
					sfp_send_data(sfp, 226, 2);
					slen += strtox(outbuf + slen,"\",\"sfp_txbias\":\"0x");
					sfp_send_data(sfp, 228, 2);
					slen += strtox(outbuf + slen,"\",\"sfp_txpower\":\"0x");
					sfp_send_data(sfp, 230, 2);
					slen += strtox(outbuf + slen,"\",\"sfp_rxpower\":\"0x");
					sfp_send_data(sfp, 232, 2);
					if (sfp_options[sfp] & 0x10) {
						slen += strtox(outbuf + slen,"\",\"sfp_temp_cal\":\"0x");
						sfp_send_data(sfp, 212, 4);
						slen += strtox(outbuf + slen,"\",\"sfp_vcc_cal\":\"0x");
						sfp_send_data(sfp, 216, 4);
						slen += strtox(outbuf + slen,"\",\"sfp_txbias_cal\":\"0x");
						sfp_send_data(sfp, 204, 4);
						slen += strtox(outbuf + slen,"\",\"sfp_txpower_cal\":\"0x");
						sfp_send_data(sfp, 208, 4);
						slen += strtox(outbuf + slen,"\",\"sfp_rxpower_cal\":\"0x");
						sfp_send_data(sfp, 184, 16);
						sfp_send_data(sfp, 200, 4);
					}
					slen += strtox(outbuf + slen,"\",\"sfp_state\":\"0x");
					sfp_send_data(sfp, 238, 1);
				}
				slen += strtox(outbuf + slen,"\",\"sfp_vendor\":\"");
				str_x_to_html(sfp_module_vendor[sfp]);
				slen += strtox(outbuf + slen,"\",\"sfp_model\":\"");
				str_x_to_html(sfp_module_model[sfp]);
				slen += strtox(outbuf + slen,"\",\"sfp_serial\":\"");
				str_x_to_html(sfp_module_serial[sfp]);
				slen += strtox(outbuf + slen,"\",\"sfp_los\":");
				if (machine.sfp_port[sfp].pin_los == GPIO_NA) {
					slen += strtox(outbuf + slen,"null");
				} else {
					bool_to_html(sfp_los(sfp));
				}
			} else {
				bool_to_html(0);
			}
		} else {
			slen += strtox(outbuf + slen, ",\"isSFP\":0,\"enabled\":");
			bool_to_html(port_phy_enabled(i));
			slen += strtox(outbuf + slen, ",\"adv\":\"");
			bits_to_html(port_phy_adv_get(i) | 0x40);
			char_to_html('"');
		}

		slen += strtox(outbuf + slen, ",\"link\":");
		
		char_to_html('0' + port_link_get(i));

		slen += strtox(outbuf + slen, ",\"txG\":\"0x");
		port_counter_get(i, STAT_COUNTER_TX_PKTS);
		hex_to_html(port_buf, 16);

		slen += strtox(outbuf + slen, "\",\"txB\":\"0x");
		port_counter_get(i, STAT_COUNTER_ERR_PKTS);
		hex_to_html(port_buf + 4, 8);		// 32 bit Tx Packet errors

		slen += strtox(outbuf + slen, "\",\"rxG\":\"0x");
		port_counter_get(i, STAT_COUNTER_RX_PKTS);
		hex_to_html(port_buf, 16);

		slen += strtox(outbuf + slen, "\",\"rxB\":\"0x");
		port_counter_get(i, STAT_COUNTER_ERR_PKTS);
		hex_to_html(port_buf, 8);		// 32bit RX packet errors

		slen += strtox(outbuf + slen, "\",\"macs\":\"0x");
		port_macs_learned_get(i);
		hex_to_html(port_buf, 8);
		slen += strtox(outbuf + slen, "\"}");
		if (i < machine.max_port)
			char_to_html(',');
		else
			char_to_html(']');
	}
}


void send_config(void)
{
	dbg_string("send_config called\n");
	__xdata uint16_t valid_len = 0;
	__xdata uint16_t n;
	__xdata uint8_t i;
	__xdata uint8_t flash_buf[CONFIG_PAGE_SIZE];

	slen = strtox(outbuf, HTTP_RESPONCE_TXT);

	// Scan through config to find the end of valid data (null terminator).
	// config_read() returns whole pages (CONFIG_LEN is a multiple), which the uint8_t index walks.
	while (config_read(valid_len, flash_buf, CONFIG_PAGE_SIZE)) {
		i = 0;
		do {
			if (flash_buf[i] == 0) {
				// Found end of valid data
				valid_len += i;
				goto found_end;
			}
		} while (++i);
		valid_len += CONFIG_PAGE_SIZE;
	}

found_end:
	// Now send the valid data
	if (valid_len > (TCP_OUTBUF_SIZE - slen)) {
		n = TCP_OUTBUF_SIZE - slen;
		config_stream_rest(n, valid_len - n);
		valid_len = n;
	}

	config_read(0, outbuf + slen, valid_len);
	slen += valid_len;
}

void send_cmd_log(void)
{
	dbg_string("send_cmd_log called\n");
	slen = strtox(outbuf, HTTP_RESPONCE_TXT);
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

	slen = strtox(outbuf, HTTP_RESPONCE_JSON);
	slen += strtox(outbuf + slen, "{\"mgmt\":");
	itoa16_html(management_vlan);
	slen += strtox(outbuf + slen, ",\"vlan\":[");

	for (i = 1; i < 4095; i++) {
		if (!vlan_entry_get(i))
			continue;

		if (slen + 141 > TCP_OUTBUF_SIZE) /* comma + 138-byte worst-case entry + closing "]}" */
			break;

		if (!first)
			char_to_html(',');
		first = 0;

		slen += strtox(outbuf + slen, "{\"id\":");
		itoa16_html(i);
		slen += strtox(outbuf + slen, ",\"name\":\"");

		n = vlan_name(i);
		if (n != 0xffff) {
			while (vlan_names[n] && vlan_names[n] != ' ')
				json_char_to_html(vlan_names[n++]);
		}

		slen += strtox(outbuf + slen, "\"}");

	}

	char_to_html(']');
	char_to_html('}');
}
