# Mirroring

The RTL837x supports mirroring received packets, transmitted packets, or both
from selected ports to a monitor port.

The RTL836x uses one source mask for both directions: RX and TX masks must be
equal, or one must be empty. The monitor port cannot be a source. Its
`MIRROR_CTRL` register (0x121c) selects the first source, the monitor port and
RX/TX enables; `MIRROR_SRC_PMSK` (0x12fb) holds the shared source mask.

## Mirroring control
On the RTL837x, mirroring is enabled by setting RTL837x_MIRROR_CTRL(0x6048):

```
RTL837x_MIRROR_CTRL = port << 1 | 0x1
```
Ports are numbered 0-9, 9 being the CPU-port.
Mirroring is stopped by writing 0 to this register.

The mirrored ports are configured in RTL837x_MIRROR_CONF(0x604c):

```
RTL837x_MIRROR_CONF = RRRR TTTT
RRRR: 16 bit mask for ports where received packets are mirrored
TTTT: 16 bit mask for ports where transmitted packets are mirrored
```

## Mirroring API
The code currently provides the following functions:
```
bool port_mirror_set(__xdata uint8_t port, __xdata uint16_t rx_pmask, __xdata uint16_t tx_pmask) __banked;
void port_mirror_get(__xdata struct port_mirror * __xdata m) __banked;
void port_mirror_del(void) __banked;
```

# Mirroring on the Serial Console
The serial console uses front-panel port numbers:
```
mirror <monitor port> [<source port>[r|t]]...
  Mirror received (r), transmitted (t), or both (no suffix) packet types.

mirror status
  Show the mirroring configuration.

mirror off
  Disable mirroring.
```
Port tokens must contain only a valid front-panel port number and, for source
ports, at most one `r` or `t` suffix. `status` and `off` take no extra arguments.
