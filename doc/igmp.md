# IGMP (Internet Group Management Protocol) and MLD (Multicast Listener Discovery)
IGMP (for IPv4) and MLD (for IPv6) are protocols that control the distribution
of Layer-3 Multicast packets on the LAN, which otherwise would be flooded across the
entire network. For this to work, IGMP/MLD messages are sent, in particular
from MC consumers (e.g. the video-player that plays an IP-Multicast stream), but
also Multicast-aware routers to control switching of the IP-MC or underlying
L2-MC packets. The main usage in home networks is IPTV. 

The RTL8372/3 SoC supports managing IPv4-MC using either Destination-IP (the IPv4
multicast group address)/Source-IP (typically 0.0.0.0) matching or via controlling
the switching of the underlying L2-MC packets (i.e. packets in 01:00:5e:xx:yy:zz, where
xx:yy:zz are the LSBs of the IPv4-MC address). The DIP/SIP-based switching is
not VLAN-aware, meaning a stream will be available in all VLANs if subscribed to.
This is not a problem in a typical home network, however. The L2-based method
is VLAN aware, but currently not supported in the software.

Although there is hardware support for IPv6/MLD-based Multicast management (i.e. intelligent
management by the switch), the current software does not implement managing IPv6 Multicast.
Instead, all IPv6 Multicast pakets will be flooded to all ports, just as an unmanaged
switch would do.

The current software support works by trapping IGMP packets (only v3 supported, which
is used in the vast majority of today's networks) to the CPU of the switch which will
update the L3 and L2 switching tables to include switch ports in a stream or remove
them. This trapping to the CPU is also called IGMP snooping. While there is support
in the HW to handle IGMP/MLD packets (v3 has only limited support) entirely in hardware
and even send out reports, it is currently not understood
how this works, and instead IGMP is handled entirely in software, which also allows
to fully support IGMPv3 packet which are the standard in present-day networks.

## IP-MC control
The relevant registers for controlling IP-MC switching are:
```
#define RTL837X_IPV4_PORT_MC_LM_ACT	0x4f78
#define RTL837X_IPV6_PORT_MC_LM_ACT	0x4f7c
#define RTL837X_IGMP_PORT_CFG		0x52a0
#define IGMP_MAX_GROUP			0x00ff0000
#define IGMP_PROTOCOL_ENABLE		0x00007c00
#define IGMP_TRAP			0x0000002a
#define IGMP_FLOOD			0x00000015
#define IGMP_ASIC			0x00000000
#define RTL837X_IGMP_ROUTER_PORT	0x529c
#define RTL837X_IPV4_UNKN_MC_FLD_PMSK	0x5368
#define RTL837X_IPV6_UNKN_MC_FLD_PMSK	0x536c
#define RTL837X_IGMP_TRAP_CFG		0x50bc
#define IGMP_TRAP_PRIORITY		0x7
#define IGMP_CPU_PORT			0x00010000
```
`RTL837X_IPV4_PORT_MC_LM_ACT/RTL837X_IPV6_PORT_MC_LM_ACT` control the action when an
IP-MC packet is encountered at a switch port and there is no rule for forwarding in
the forwarding tables. The default action is to flood such Lookup-Miss packets to all
ports. This is the configuration without IGMP/MLD enabled.

When IGMP/MLD is turned on, the Lookup-Miss action will be changed to drop such packets
unless a rule is found in the forwarding tables, which will need to be configured by
IGMP packets.

Switching on IGMP also configures all ports via `RTL837X_IGMP_PORT_CFG` to trap all
incoming IGMP packets to the CPU. `RTL837X_IGMP_TRAP_CFG` then is used to configure
priority and CPU-Port of trapped IGMP/MLD packets. 

Configuration of the IP-MC-forwarding to the listening ports is done by managing the
forwarding tables of the switch, see [L2 learning](l2.md).


## IGMP API
The code currently provides the following functions:
```
void igmp_setup(void) __banked;
void igmp_enable(void) __banked;
void igmp_router_port_set(uint16_t pmask) __banked;
uint8_t igmp_packet_handler(void) __banked;
void igmp_show(void) __banked;
```
`igmp_setup()` is called at boot-time and configures flooding of all IP-MC packets by
default, as otherwise no IP-MC would be possible in the network. `igmp off` calls it as
well; groups learned while IGMP was on stay in the table, but are no longer looked up.

`igmp_enable()`starts IGMP which cause IGMP packets to be handled by the CPU and forwarding
of IP-MC packets to be limited to only subscribed ports.

`igmp_router_port_set()`configures forwarding ports for IGMP messages.

`igmp_packet_handler()` implements handling of trapped IGMP packets by the CPU. It is called
for frames to 01:00:5e:xx:xx:xx and returns 0 when the frame is not a trapped IGMP packet,
so that it is passed on to the IP stack. IGMPv1/v2 reports (0x12, 0x16) join the group given
in the IGMP header, IGMPv2 leaves (0x17) leave it, IGMPv3 reports use the first group record
(type 4 joins, type 3 leaves). The RTL837x receive envelope has no VLAN tag.
Reports for groups in 224.0.0.0/24 are ignored: the switch floods that block
even with IGMP on, and an entry for a group such as 224.0.0.251 (mDNS) would
otherwise limit it to the ports that reported it, as RFC 4541 warns against.

The receive handler checks the captured frame size, IPv4 version/header/total
length and fragmentation before reading an IGMP report. For IGMPv3 it checks
every declared record, source list and auxiliary-data length before touching
the ASIC, so an incomplete later record cannot partially update a group.
Truncated input and Ethernet padding never substitute stale receive-buffer
bytes for a missing payload. This validation does not add multi-record
processing or source-specific multicast support.

`igmp_show()` prints out the IGMP configuration on the CLI.

## RTL836x (TL-SG108PE V5)

The native backend uses the same `igmp on|off|show` commands and validated
software parser. Its NIC inserts a synthetic VLAN header, so IPv4 begins at
byte 26 rather than byte 22. IGMPv1/v2 reports and the existing first-record
IGMPv3 behavior update native multicast entries; no source-specific or
multi-record processing is added. Groups remain IP-based, not VLAN-specific.

`igmp off` disables IPv4 multicast lookup and hardware snooping, restores
flooding, and removes retained protocol traps. `igmp on` traps IPv4 IGMP to
software, keeps MLD flooding, enables DIP-only lookup with SIP zero, and drops
unknown IPv4 multicast on front-port ingress. CPU10 remains unfiltered.
Retained IPMC classification exceptions are cleared before enabling lookup.

The six-word LUT key contains the full DIP and SIP zero. Joins and leaves
change only the reporting front port's membership; a final leave removes that
key. Existing extended membership bits and unrelated unicast entries are
preserved. A post-write lookup verifies membership, including table-full
refusal. Busy table operations have a bounded poll limit and are not retried.
The router-port API translates logical CPU9 to chip CPU10.

Register facts follow the RTL8367C SDK IGMP/LUT APIs. Host coverage includes
retained state, packed and extended memberships, high address octets, native
RX headers, key isolation, unavailable/full tables and failed writes.

Build `3b69aa05` was exercised on a TL-SG108PE V5. Real OS IGMPv3 joins and
leaves installed and removed two test groups on their measured ingress port
(uplink 8, membership `0x80`). Extra-argument CLI requests returned HTTP 400
without changing state. For 64 test frames, unknown-group egress on control
ports 2 and 5 fell from 67/67 packets with IGMP off to 2/2 with it on;
224.0.0.252 traffic still produced 65/65 packets. These counts include background
traffic. IGMP was restored off, both test memberships removed, and the saved
configuration and Ethernet/PoE administrative settings preserved.

Cross-port forwarding was then verified with the Proxmox master on front port 1
and the listener behind uplink 8. Of 64 test datagrams, 63 reached the receiving
application, the listener-port counter rose by 64, and nonmember ports 5 and 6
each rose by one background multicast packet. Before the join and after the
leave, the same 64-packet source stream produced no uplink egress. Counter
windows were aligned at about two seconds, excluding any extra receive wait.
IGMP was restored off, the test group removed, and saved/port settings preserved.
The sender used its existing `vmbr0`; no container or host-network changes were
needed, and the Proxmox cluster remained quorate.

## IGMP configuration on the Serial Console
For testing the following commands are provided on the serial console:
```
> igmp [on/off]
  Enables or disables IGMP

> igmp show
  Shows information on IGMP
```
Each command takes exactly two words. Extra arguments are rejected before
changing IGMP state or accessing the ASIC.


## LAG configuration via the Web Interface
Not implemented, yet!

## A Test with IP-MC streaming using vlc
The following is a simple test verifying the IGMP and IP-MC switching capabilities.

You will need 2 Linux/Windows devices with a GUI plus a switch.

Connect the switch to an MC-aware router (e.g. to your home network). Connect the 2 Linux/Windows
devices to the switch. The connection to the router makes sure that Linux/Windows will send
out IGMP messages on the ports connected to the switch, which they will only do if they are aware
that there is a MC-aware router in the network. Make sure the 2 GUI devices are in the home network
(e.g. via DHCP).

Start streaming on one of the Linux/Windows machines:
```
$ vlc your_video.mp4 --sout="#std{access=udp, mux=ts, dst=239.255.0.1:8090}"
```
At this point you should see all switch ports flickering heavily as the MC stream is switched to all
switch ports, including flooding your home network. If you do not see any packets arriving at the switch,
you can force the output interface of vlc by using `--miface=<ifname>`

Enable IGMP on the switch-CLI:
```
> igmp on
```
The flickering should now stop on all ports except the port where the streaming device is connected:
the switch drops all IP-MC packets as there are no listeners.

Now, on the second Linux/Windows device start listening to the stream:
```
$ vlc udp://@239.255.0.1:8090
```
You should see the port-led of the port the displaying machine is connected to, to start flickering
and after some synchronization, the video should start playing.

Stopping vlc should also switching of the IP-MC frames to the listening device, i.e. the port-leds
should stop flickering.
