# Storm Control
Storm control limits the rate at which a port admits broadcast, multicast,
unknown unicast and unknown multicast frames. Frames of a type above its limit
are dropped at ingress, so a loop or a misbehaving host cannot flood the rest
of the network through that port. Known unicast traffic is not affected.

## Commands
```
storm <port> bcast|mcast|ucast|umcast <rate> pps
storm <port> bcast|mcast|ucast|umcast <rate> kbps
storm <port> bcast|mcast|ucast|umcast off
storm
```
`ucast` is unicast to a MAC address the switch has not learned yet, and
`umcast` is multicast without an entry in the address table. Multicast groups
get their entries from IGMP snooping, so without `igmp on` all multicast is
unknown and only the `umcast` limit applies to it; `mcast` covers the groups
IGMP has learned. A limit in
packets per second goes from 1 to 1048575, a limit in kbit/s from 1 to
10000000. `storm` without arguments prints the limits of every port. The
limits are kept in the configuration like any other setting, and the Bandwidth
page of the web interface has a Storm control card to set them.

## RTL836x (TL-SG108PE V5)

The native backend supports the four classes on ports 1–8 in **kbit/s only**:
8–1000000 kbit/s, in steps of eight. PPS commands are refused and the UI offers
only kbit/s. The same `off` commands disable individual classes.

Meters 24–55 are assigned as `24 + port * 4 + type`. The two rate banks start
at `0x1400` and `0x1740`, with two words per meter and three high rate bits.
Counts are kbit/s divided by eight. Mode bits at `0x1440`/`0x1780` are cleared
before enabling a limit, including after a warm boot with retained PPS state.
The packed selectors start at `0x0a44`; the four enable words at `0x0a40`.
Preamble/IFG accounting is disabled for the selected meter without changing
adjacent meters. Bucket and clock calibration are left untouched.

Host tests cover both banks, retained modes, selectors, calibration preservation
and rejected inputs. Build `f1fe7cf4` on a TL-SG108PE V5 exercised all four
classes on port 3, confirmed JSON readback, refused PPS/misaligned requests,
and preserved the bucket register. The browser applied and removed an
8-kbit/s broadcast limit. All classes were restored off without saving.
Hardware storm-rate accuracy needs a controlled ingress traffic source.
The registers and traffic measurements below describe the RTL837x backend.

## Registers
Each port and type has an enable bit and a meter index, and each limit uses
one of the 64 shared meters of the switch. RTLPlayground gives every port and
type its own meter, index `24 + port * 4 + type`, so one port's storm does not
eat into the limit of another. Storm control uses meters 24 to 63 and leaves
0 to 23 to the ACL. The limit of a meter is in `RTL837X_METER_RATE + 4 *
meter`, and bit `meter % 32` of `RTL837X_METER_MODE + 4 * (meter / 32)`
selects pps instead of kbit/s.
```
#define RTL837X_STORM_CTRL		0x54e4	/* + 4 * type, bit = port */
#define RTL837X_STORM_MIDX		0x54f4	/* + 8 * type + 4 * (port / 5), 6 bits per port */
#define RTL837X_METER_RATE		0x5cf0	/* + 4 * meter, 24 bits, kbit/s or pps */
#define RTL837X_METER_MODE		0x5ef0	/* + 4 * (meter / 32), bit set = pps */
```
In kbit/s mode one step of the rate is 1 kbit/s. Measured with 100 and 1000 byte
frames, the meter counts a frame with its FCS, but without preamble and
interframe gap. Every meter has a bit that adds them, in `0x5f08 + 4 * (meter
/ 32)`, which storm control leaves cleared. `IGBW_INC_IFG` and `EGBW_INC_IFG`
are the matching bits of the bandwidth control.

An ingress bandwidth limit on the same port meters the same arriving frames
as the storm limit, side by side rather than one after the other. With the
ingress limit set to drop, a frame has to get past both, so their pass rates
multiply: against a 1.7 Gbit/s stream of unknown multicast, a 1 Mbit/s
`umcast` limit and a 10 Mbit/s ingress limit together let 11 frames through
in 10 seconds, while each alone passed 1.0 and 10.0 Mbit/s. With the ingress
limit in flow control mode the switch instead sends PAUSE frames, the sender
slows down to the ingress limit, and the storm limit then passes its 1 Mbit/s
as usual.

The type is 0 for broadcast, 1 for multicast, 2 for unknown unicast and 3 for
unknown multicast. The burst size of a meter (0x5df0 + 4 * meter) counts packets
in pps mode; its default of 0x2000 lets 8192 frames through before a pps limit
bites, so a pps limit sets the burst to one second of traffic. A kbit/s limit
puts back the default.
