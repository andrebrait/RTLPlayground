#!/usr/bin/env python3
"""Exercise management VLAN admission and TX tags through the real native NIC."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "rtlplayground.c").read_text()
framing = source[source.index("struct eth_in {"):source.index("void isr_timer0(")]


def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


harness = r'''
#define main nic_fixture_main
#include "test_rtl836x_nic.c"
#undef main
#include "uip.h"
uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len, management_vlan, rx_packet_vlan;
uint8_t tx_seq, lacpEnabled, igmpEnabled;
bool frame_tagged, stp_enabled, lldp_enabled;
static bool pending;
static unsigned arp_calls, ip_calls, lldp_calls;
static bool receive_one(void) { bool p = pending; pending = false; return p; }
#define nic_rx_pending receive_one
#define health_rx_frame() ((void)0)
#define RX_BUDGET 4
void stp_in(void) { assert(0); }
void lacp_in(void) { assert(0); }
bool lldp_in(void) { lldp_calls++; uip_len = 0; return true; }
uint8_t igmp_packet_handler(void) { assert(0); return 0; }
void uip_arp_arpin(void) { arp_calls++; uip_len = 0; }
void uip_rx_ip_input(void) { ip_calls++; uip_len = 0; }
void uip_arp_out(void) { assert(0); }
void print_string(const char *s) { (void)s; }
'''
# handle_rx() once more as the RTL837x builds it: DHCP snooping and the ACL trap frames for other stations
rtl837x = r'''
#undef SOC_FAMILY
#define SOC_FAMILY RTL837X
static bool dhcp_snoop_on;
static unsigned snoop_calls;
void dhcp_snoop_in(void) { snoop_calls++; }
struct uip_eth_addr uip_ethaddr = {{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
#define handle_rx handle_rx_837x
'''
checks = r'''
static void receive(uint16_t management, uint16_t incoming, uint16_t type, bool allowed)
{
    uint8_t header[6] = {0x81, 0, incoming >> 8, incoming, type >> 8, type};
    put_frame(0x100, 60, 0);
    ring_put(0x108 + 12, header + (incoming ? 0 : 4), incoming ? 6 : 2);
    registers[0x1a50] = 0x20;
    registers[0x1a51] = 0;
    management_vlan = management;
    pending = true;
    arp_calls = ip_calls = lldp_calls = 0;
    handle_rx();
    assert(rx_packet_vlan == (incoming ? incoming : 1));
    assert(arp_calls == (unsigned)(allowed && type == 0x0806 && !lldp_enabled));
    assert(ip_calls == (unsigned)(allowed && type == 0x0800 && !lldp_enabled));
    assert(lldp_calls == (unsigned)lldp_enabled);
}
static void receive_837x(uint16_t incoming, const uint8_t *dst, bool snoop, unsigned snoops, unsigned ips)
{
    uint8_t header[6] = {0x81, 0, incoming >> 8, incoming, 0x08, 0x00};
    put_frame(0x100, 60, 0);
    ring_put(0x108, dst, 6);
    ring_put(0x108 + 12, header, 6);
    registers[0x1a50] = 0x20;
    registers[0x1a51] = 0;
    management_vlan = 100;
    dhcp_snoop_on = snoop;
    pending = true;
    ip_calls = snoop_calls = 0;
    handle_rx_837x();
    assert(snoop_calls == snoops && ip_calls == ips);
}
int main(void)
{
    const struct { uint16_t management, incoming; bool allowed; } rx[] = {
        {1, 0, true}, {1, 100, false}, {1, 200, false},
        {100, 0, false}, {100, 100, true}, {100, 200, false},
        {0, 0, true}, {0, 100, true}, {0, 200, true},
        {4094, 4094, true}, {4094, 4093, false}
    };
    for (unsigned i = 0; i < sizeof(rx) / sizeof(rx[0]); i++) {
        receive(rx[i].management, rx[i].incoming, 0x0800, rx[i].allowed);
        receive(rx[i].management, rx[i].incoming, 0x0806, rx[i].allowed);
    }
    lldp_enabled = true;
    receive(100, 200, 0x88cc, false);
    lldp_enabled = false;
    const struct { uint16_t vid; uint8_t hi, lo; } tx[] = {
        {1, 0, 1}, {100, 0, 100}, {4094, 15, 254}, {0, 0, 0}
    };
    for (unsigned m = 0; m < sizeof(tx) / sizeof(tx[0]); m++) {
        management_vlan = tx[m].vid;
        memset(uip_buf, 0, sizeof(uip_buf));
        uip_len = 60;
        uip_buf[24] = 0x08;
        tcpip_output();
        if (management_vlan) {
            assert(sent[20] == 0x81 && sent[21] == 0);
            assert(sent[22] == tx[m].hi && sent[23] == tx[m].lo);
            assert(sent[24] == 0x08 && sent[25] == 0);
            assert(sent[6] == 0 && sent[7] == 64);
        } else {
            assert(sent[20] == 0x08 && sent[21] == 0);
            assert(sent[6] == 0 && sent[7] == 60);
        }
    }
    memset(uip_buf, 0, sizeof(uip_buf));
    management_vlan = 100;
    uip_len = 60;
    uip_buf[24] = 0x88;
    uip_buf[25] = 0x99;
    tcpip_output();
    assert(sent[20] == 0x88 && sent[21] == 0x99 && sent[7] == 60);
    const uint8_t own[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, other[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    receive_837x(200, broadcast, true, 1, 0);   /* another VLAN: snooped, kept out of the IP stack */
    receive_837x(100, broadcast, true, 1, 1);   /* management VLAN: snooped once, then the IP stack */
    receive_837x(100, own, true, 1, 1);
    receive_837x(100, other, true, 1, 0);       /* trapped for another station */
    receive_837x(100, other, false, 0, 0);
    receive_837x(100, own, false, 0, 1);
    receive_837x(200, own, false, 0, 0);
    puts("PASS management VLAN: IPv4/ARP isolation, untagged VLAN 1, disabled filtering, LLDP bypass, native TX tags, RTL837x snooping and trap filter");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="rtl-management-vlan-") as temp:
    c = Path(temp) / "management_vlan.c"
    binary = Path(temp) / "management_vlan"
    c.write_text(harness + framing + function("void tcpip_output(void)") +
                 function("void handle_rx(void)") + rtl837x + function("void handle_rx(void)") +
                 "\n#undef handle_rx\n" + checks)
    subprocess.run(shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-Wno-unknown-pragmas", "-Wno-pointer-sign", "-fpack-struct=1",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-DRTLP_HOST_TEST", "-DMACHINE_NAME=TPLINK_TL_SG108PE_V5",
        "-include", str(root / "test/sdcc_shim.h"),
        "-I" + str(root / "test"), "-I" + str(root),
        "-I" + str(root / "uip"), "-I" + str(root / "httpd"),
        str(c), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
