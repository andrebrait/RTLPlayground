# RTLPlayground on Linux

`PLATFORM=linux` builds the command line and the web interface of RTLPlayground as an ordinary
Linux daemon instead of an 8051 firmware image. The shared files (`cmd_parser.c`, `cmd_editor.c`,
`httpd/page_impl.c` and everything under `html/`) are compiled and served unchanged; what is
different is the implementation of `platform.h` in `platform/linux/`. It drives the kernel through
rtnetlink and ethtool instead of the switch registers, so the switch is whatever the kernel
exposes as network devices: on a Realtek RTL93xx board running a DSA driver, one device per front
panel port.

This document covers the first stage of that port (called 2a below): the console, the CLI socket,
the web server and login, the configuration store, ports, VLANs, the management address and the
L2 table. Everything else is listed in [What is supported](#what-is-supported).

## Build

Dependencies: `make`, `gcc`, `python3`, `gzip`, `libmnl-dev`, and a C library with `crypt(3)`
(`libcrypt`). The 8051 toolchain is not needed. Runtime dependencies are libc, libmnl and, for
DHCP only, BusyBox with the `udhcpc` applet and an `ip` command (iproute2 or BusyBox).

```
make PLATFORM=linux [BOARD=BENCH6]
make PLATFORM=linux test
```

This produces, in `platform/linux/output/`:

- `rtlplayground`, the daemon;
- `swcli`, its command line client (see [CLI socket](#cli-socket-and-swcli));

and, in `output/html_min/`, the minified web interface together with a gzip copy (`<name>.gz`)
of every file except `login.html`. The daemon serves the compressed copy with
`Content-Encoding: gzip`, as the 8051 does.

`make PLATFORM=linux test` runs the unit tests, a console test against the daemon and a test of
the install target. The namespace bench (see [Bench](#bench)) needs root and is run separately.

The Docker image of the repository (`docker build -t rtlplayground-dev .`) contains every
dependency, including those of the bench.

### Install

```
make PLATFORM=linux install DESTDIR=<root> [PREFIX=/usr]
```

installs `$PREFIX/sbin/rtlplayground`, `$PREFIX/bin/swcli`, the web interface in
`$PREFIX/share/rtlplayground/www` and the DHCP script in `$PREFIX/share/rtlplayground/udhcpc.script`.
These are the paths the daemon uses by default for `--www` and `--dhcp-script` when `PREFIX` is
`/usr`; with another prefix pass both options explicitly. The target is meant for the root
filesystem builds that package the daemon; it does not install an init script.

### Board files

A board is one C file in `platform/linux/boards/`, selected with `BOARD=<name>` (default `BENCH6`).
It is the only place that names a board. It defines the `machine` structure of the shared code
(name, first and last logical port, logical-to-physical port maps) and `board_ifnames`, the
netdev name of each logical port:

```c
const struct machine machine = {
	.machine_name = "BENCH6",
	.min_port = 0,
	.max_port = 5,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};

const char *const board_ifnames[9] = { "lan1", "lan2", "lan3", "lan4", "lan5", "lan6" };
```

A board has at most nine ports. The name appears in the firmware version string and in
`hw_ver` of the web interface. Boards shipped: `BENCH6` (six veth ports of the namespace bench)
and `VM2E1000` (two e1000e NICs of a test virtual machine).

## Running

```
rtlplayground [--console] [--http <addr:port>] [--cli-socket <path>] [--www <dir>]
              [--cfg-dir <dir>] [--dhcp-script <script>]
```

| Option | Default | Meaning |
|---|---|---|
| `--console` | off | Read commands from standard input and write the prompt and output to standard output, like the serial console of the 8051. Without it the daemon does not touch its standard streams. |
| `--http <addr:port>` | `[::]:80` | Where the web interface listens. The address is numeric, IPv6 in brackets, e.g. `0.0.0.0:8080`. |
| `--cli-socket <path>` | `/run/rtlplayground.sock` | The Unix socket `swcli` connects to (mode 0600). |
| `--www <dir>` | `/usr/share/rtlplayground/www` | The directory of the web interface (`make` leaves it in `output/html_min`). |
| `--cfg-dir <dir>` | `/cfg` | Where `startup-config` lives. It is created with mode 0700. |
| `--dhcp-script <script>` | `/usr/share/rtlplayground/udhcpc.script` | The BusyBox `udhcpc` event script. |

The daemon is meant to run as root: it configures network interfaces, binds port 80 and starts
the DHCP client. It runs one `poll()` loop and never blocks. On `SIGTERM` or
`SIGINT` it stops; `GET /reset` (the *Reboot switch* button) makes it execute itself again with
the same arguments, so the configuration is read anew. The daemon does not fork or write a PID file;
let the init system supervise it.

The daemon creates or adopts the bridge `swbr` (see [VLANs](#vlans-and-the-bridge)) and starts
BusyBox `udhcpc` as a child when DHCP is on. Standard error carries diagnostics.

## CLI socket and swcli

`swcli` runs console commands on the daemon through the Unix socket, one connection per command.
The socket is for the local administrator: there is no login, access is the file mode (0600,
owned by the daemon's user).

```
swcli [-s <socket>] <command words...>     # one command
swcli [-s <socket>]                        # commands from standard input, one per line
```

The output is exactly what the console would print. The exit status is 0 when the daemon was
reached (also for a command that printed an error) and 1 when it was not. Use the standard-input
form for `passwd`, because the password would otherwise be visible in the process list and in
the shell history:

```
echo 'passwd <password>' | swcli
```

The same commands are available on the web interface (*System*, *Console*) and as
`POST /cmd`.

## Password and sessions

- **There is no default password.** Until one is set, the web interface serves only the login
  page, with a notice that a password has to be set on the console (`passwd <password>`), every
  login fails, and `POST /cmd` answers 401.
- **Only hashes are stored.** The shared `passwd <password>` command writes the plaintext
  into a global as on the 8051; the platform hashes it (`crypt(3)`, SHA-512, random salt) right
  after the command ran and wipes the plaintext. Nothing but the `$6$...` hash is kept in memory
  or on disk.
- **The startup configuration holds `passwdhash <hash>`**, not `passwd`. `passwdhash` is a command
  the platform adds; it sets the hash and prints nothing, and it refuses anything that is not
  a well-formed `$6$` hash. A saved configuration that contains a plaintext `passwd` line is rewritten into
  a `passwdhash` line before it reaches the disk, and only the last password line of a file is
  kept.
- **The command history is scrubbed.** The shared parser records every command, including
  `passwd <password>`. The platform overwrites the argument with asterisks as soon as the command
  has run, so neither the console history, `GET /cmd_log` nor the merge of the history into the
  startup configuration by *Save to flash* contains the password. When *Save* merges a line
  `passwd ****` back, the platform keeps the current hash instead of hashing the asterisks. A password
  that consists only of asterisks therefore cannot be set through an upload; set it on the console.
- One session is valid at a time, as on the 8051: a new login ends the previous session, and so
  does a password change. A session ends after `session <seconds>` of inactivity (default 200).
  After a failed login every login is refused for one second.

## Configuration

The startup configuration is the file `<cfg-dir>/startup-config`, a list of CLI commands that is
replayed line by line on every start, as in the flash of the 8051. It is at most 2048 bytes.

- **Saving is atomic.** The new text is written to `startup-config.new` and synced, the old file
  is renamed to `startup-config.bak`, the new one is renamed into place and the directory is synced.
  A crash leaves either the old file or the old file as `.bak`; at the next start a missing
  `startup-config` is restored from `.bak` and a leftover `.new` is deleted. The save that
  was interrupted is lost, the old configuration survives.
- **Saving happens from the web interface** (*Save to flash* merges the command history into the file and
  verifies what it wrote) or by uploading a file (`POST /config`, the *Write to flash* button of
  *System*). The console has no save command.
- **Runtime changes need a save to survive.** Commands change the running system at once (the
  kernel state) and are recorded in the command history; only *Save to flash* writes them to
  `startup-config`. `GET /reset`, a restart of the daemon, and a reboot replay the file, so
  anything not saved is lost, as on the 8051. The exceptions are the kernel state the daemon does
  not own (see [VLANs](#vlans-and-the-bridge)).
- With no file, or an empty one, the built-in default runs: every port an untagged member of VLAN
  1 with PVID 1, VLAN 1 as management VLAN, and DHCP on.

## VLANs and the bridge

The switch fabric is the Linux bridge `swbr` with `vlan_filtering 1`, STP off and
`vlan_default_pvid 0`; every board port is a member. The VLAN table of the console and the web
interface is the VLAN table of the bridge: a VLAN is a set of bridge VLAN entries, the CPU port
(port 9 of the 8051) is the bridge itself, and a port's PVID is the PVID flag of its entry. The
management VLAN is untagged on the bridge, so the management IP address lives on `swbr`.

The kernel is the single source of truth: reads are netlink dumps, not copies. If the bridge
already exists at start (daemon restart or `/reset`) it is adopted without interrupting traffic and the
configuration is applied as a difference; VLANs the file does not create are removed afterwards.

**Reserved parking VLANs.** The 8051 lets a port have no usable PVID (a PVID register naming a
VLAN the port is not in, ingress mode `allow tagged`, or a deleted PVID VLAN). A bridge port without
PVID can leak untagged frames as VID 0 on the Realtek DSA driver, so the platform never leaves a port
without one: such a port is *parked* in its own VLAN, in which it is the only member. The parking
VLAN of logical port N is **4094 − N**, so a board with N ports reserves N VIDs (4089 to
4094 on the six-port bench board). The parking VLANs are never reported by `vlan show`, `vlanlist`
or `vlan.json`, and `vlan`, `pvid` and `l2` refuse them (`vlan 4090 mgmt` is
refused as well, since it would cut the management connection). The daemon owns them: at start any
foreign member is removed.

Differences from the 8051:

- `ingress u` (accept untagged only) is refused; a bridge cannot drop tagged frames per port.
- Ingress filtering is always on.
- While DHCP is on, `netmask` and `gw` are refused; `ip <address>` stops DHCP and sets a static
  address.

## Management address and DHCP

`ip`, `netmask` and `gw` set the address on `swbr` through netlink and read the result back,
so `ip` and `information.json` report what is configured. A netmask of `0.0.0.0` gives a `/32`;
a gateway that is not reachable from the address is refused, or kept without a route (with a
warning) when it became unreachable because the address changed in the same command run.

`ip dhcp` starts `busybox udhcpc -i swbr -f -S -s <script>` as a child of the daemon. The script
`udhcpc.script` puts address and default route on `swbr` with the lease as lifetime, and the
daemon reads them back once a second. If the client exits it is started again after five seconds; if
it cannot be started DHCP is switched off and the reason logged. When the daemon exits the child is
stopped; the lease stays on the interface until it expires.

## Counters

`counters.json` and the statistics of the web interface use the counter list (`MIB`) of
`html/app.js`; a word reads the first source that has it:

1. a named ethtool statistic of the driver (`ETHTOOL_GSTATS`, names as the Realtek Otto DSA
   driver reports them, e.g. `ifOutDiscards`, `tx_BroadcastPkts`, `Collisions`; for other NICs
   the `e1000e` names);
2. the standard netdev statistic (`IFLA_STATS64`);
3. otherwise 0.

These words always read 0, because the kernel has no such counter: the unicast packet counts (no
unicast counter exists; packets minus multicast would count broadcasts as unicast), the pause and
control frame counts, the RMON frame size histograms, the per-queue counters, and the PHY side
counters. The broadcast counts read 0 on a driver without a broadcast counter (veth). The Otto
driver reports frame sizes and pause frames only through the structured ethtool statistics
groups, which the platform does not read yet, so on that hardware these words stay 0 for now.
The counts are those of the netdev, since it was created.

## MTU

`mtu <port> <value>` and the MTU field of the web interface use the 8051 unit: the longest frame the
MAC accepts, counted on the wire, 64 to 16383. A Linux MTU is the payload only, so

    value = netdev MTU + 26

where 26 = 14 (Ethernet header) + 2 × 4 (two VLAN tags) + 4 (FCS). It is the conversion of the
Realtek DSA driver itself (`RTLDSA_FRAME_OVERHEAD`), so `mtu 1 9026` programs the same register
value as on the 8051 and gives a netdev MTU of 9000. The default 1500 is shown as 1526. A value
whose MTU the device does not accept (`min_mtu`, `max_mtu`) is an error and changes nothing. The
platform never sets the MTU of `swbr`; the kernel follows the smallest port MTU by itself.

## Ports

Link state and speed come from sysfs and ethtool and map to the link codes of the web interface
(10M, 100M, 1000M, 2.5G, 5G, 10G; 10G for veth). Speed, duplex and enable/disable are
changed through ethtool; a device that refuses the setting prints an error and keeps its state.
Every kernel call is checked: a failed change is never reported as applied.

## L2 table

The table is the forwarding database of `swbr`. Learned and static entries of board ports are
listed with VLAN and port, together with the switch's own address (port `CPU`, in the management
VLAN, as on the 8051). The kernel's other local entries, entries in parking VLANs and entries
of other interfaces are not listed. The web interface addresses an entry by a 12-bit slot
number; the platform keeps a stable slot per entry, so a page that is being walked sees no
entry twice while entries come and go. The `l2` command prints the table, `l2 forget` removes the learned entries, the
delete button of the web interface removes one entry, and `mac <address>` sets the switch's own
address. The learned-addresses column of the port page counts the learned entries of the port.

## What is supported

Implemented (2a): console, CLI socket, web server with login, `/cmd`, `/config`, the configuration
store, port status, speed, MTU and counters, bridge VLANs, PVIDs and ingress modes, the
management address and DHCP, the L2 table, hostname, session timeout and the password.

Not yet (2b): link aggregation and LACP, spanning tree, IGMP snooping, LLDP, mirroring, port
isolation, bandwidth and storm limits, EEE and flow control, cable test, DNS, NTP and syslog, and
SNMP. A command for one of these prints `Not supported on this platform yet`, and its web page shows
the feature as off. The *Firmware* page does not upload on Linux: `POST /upload` answers 501
and the image is installed with the root filesystem's own update tool. SFP cages and the
chip temperature (read from `/sys/class/thermal`, 0 when there is none) have no counterpart on
the bench boards.

## Bench

`platform/linux/bench/` runs the daemon in a network namespace, `rtlp-sw`, with six veth ports `lan1` to
`lan6`; the other ends are hosts `rtlp-h1` to `rtlp-h6` with the addresses `192.0.2.11` to `192.0.2.16`.
It needs root, `iproute2`, BusyBox (with `udhcpc`, `udhcpd` and `ping`), `ethtool`, `curl`,
`python3` and `openssl`.

```
make PLATFORM=linux
sudo platform/linux/bench/run.sh
docker run --rm --privileged -v "$PWD":/w -w /w rtlplayground-dev platform/linux/bench/run.sh
```

`run.sh` runs every `t_*.sh` on a fresh bench and prints `ok <name>` or `FAIL <name>`; the exit
status is 1 if any failed. The tests are `t_smoke`, `t_cli`, `t_http`, `t_http_hostile`, `t_config`,
`t_ports`, `t_vlan`, `t_restart`, `t_l2` and `t_ui`, which fetches every route that `html/app.js`
requests and fails when the script gains a route the test does not know. See
`platform/linux/bench/README.md` for the helpers. To look at the web interface of a bench by hand,
run the daemon in `rtlp-sw` and forward a port into the namespace, for example with `socat`
and `ip netns exec rtlp-sw`, inside a privileged container with its port published.
