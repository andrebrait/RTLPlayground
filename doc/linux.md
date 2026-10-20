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
(`libcrypt`). The 8051 toolchain is not needed. Runtime dependencies are libc, **libcrypt
(libxcrypt, for `crypt(3)`; the daemon links it)**, libmnl and, for DHCP only, BusyBox with the
`udhcpc` applet and an `ip` command (iproute2 or BusyBox).

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

The default build (and `install`) is the production build: no `-Werror`, and none of the test
hooks (a failing `rename`, a crash in the middle of a save, an injected netlink failure, `--dump-state`
are compiled only into the **test build**). `make PLATFORM=linux test` builds the test build too
(`output/test/`, `-DRTLP_TEST_HOOKS -Werror`), runs the unit tests, a console test against it
(in a network namespace of its own when run as root), a test of the install target and a test of the
bench scripts. The namespace bench (see [Bench](#bench)) runs the test build, needs root and is run
separately; `make -C platform/linux testbin` builds just that.

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
rtlplayground [--console] [--http <addr:port> [--http-any]] [--cli-socket <path>] [--www <dir>]
              [--cfg-dir <dir>] [--dhcp-script <script>]
```

| Option | Default | Meaning |
|---|---|---|
| `--console` | off | Read commands from standard input and write the prompt and output to standard output, like the serial console of the 8051. Without it the daemon does not touch its standard streams. |
| `--http <addr:port>` | `[::]:80` | Where the web interface listens. The address is numeric, IPv6 in brackets, e.g. `0.0.0.0:8080`. On a kernel without IPv6 the default falls back to `0.0.0.0:80`. **The listener is bound to `swbr`** (`SO_BINDTODEVICE`), so the web interface answers on the management VLAN only, not on any other interface that has an address; a loopback listener (`127.0.0.1` and `::1`, same port) serves local tools. The default `[::]:80` therefore serves IPv4 and IPv6 on `swbr` and loopback; an explicit address keeps that address and port and is bound to `swbr` all the same. If `swbr` or the address does not exist, the daemon says so and runs without the web interface (console and CLI socket work). |
| `--http-any` | off | With `--http`: do not bind to `swbr`; listen on exactly that address on every interface that has it (and no extra loopback listener). For setups where the management interface is not `swbr`. |
| `--cli-socket <path>` | `/run/rtlplayground.sock` | The Unix socket `swcli` connects to (mode 0600). `swcli` looks for the same path unless it is given `-s`. A socket nobody listens on (a killed daemon's) is replaced; a live one is not: the daemon refuses to start. |
| `--www <dir>` | `/usr/share/rtlplayground/www` | The directory of the web interface (`make` leaves it in `output/html_min` of the repository root, not under `platform/linux`). |
| `--cfg-dir <dir>` | `/cfg` | Where `startup-config` lives. The directory itself is created if it is missing, but not its parents, and is locked (`flock`) for the life of the process: **a second daemon on the same directory refuses to start.** **Trust:** the daemon runs as root and runs what the file says, so it reads the directory and the file only if they belong to the user the daemon runs as and are not writable by group or others (a directory of ours with other modes is tightened to 0700); otherwise it logs why, runs the built-in default, sets no password and stores nothing. |
| `--dhcp-script <script>` | `/usr/share/rtlplayground/udhcpc.script` | The BusyBox `udhcpc` event script. |

The daemon is meant to run as root: it configures network interfaces, binds port 80 and starts
the DHCP client. It runs one `poll()` loop, which waits only in `poll()` (at most a second at a time).
The exceptions are named: every netlink request is synchronous (the kernel answers at once), starting
the DHCP client waits for it to exec BusyBox for at most 200 ms, and a daemon that exits or restarts
waits (at most about a second) for the DHCP client to end. Stopping the client for `ip <address>` or
`ip dhcp` does not wait: it gets `SIGTERM` at once and is reaped, and killed if it must be, by the loop;
the address that `ip <address>` sets then reaches the kernel as soon as the client has exited (milliseconds, a
second at most), not before, because a client that already has its lease would still run the lease script
that flushes it.
On `SIGTERM` or `SIGINT` it stops; `GET /reset` (the *Reboot switch* button) makes it execute itself again with
the same arguments, so the configuration is read anew. The daemon does not fork or write a PID file;
let the init system supervise it. The process is named `rtlplayground` also after `/reset`.

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

Without `-s` the socket is `/run/rtlplayground.sock`, the daemon's default. The output is exactly what the console would print. The exit status is 0 when the daemon was
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
  after the command ran and wipes the plaintext. Nothing but the `$6$...` hash reaches the disk. One
  copy of plaintext stays in RAM: the **last uploaded configuration text** (with a `passwd <password>`
  line, if the upload had one) is held and served by `GET /config` until the next upload or restart,
  because the web interface's *Save* reads the configuration back and insists that it is byte for byte
  what it sent (see [Configuration](#configuration)). A `passwdhash` line is accepted only in the form
  the daemon writes: `$6$<salt>$<86 characters>`; a `rounds=` field is refused, since a large one
  would keep `crypt` busy in the poll loop for minutes. A password is at most 20 characters (what the shared command keeps); longer input is
  cut at 20.
- **The first password is stored at once.** Setting the first password (when none exists) is what
  makes the switch reachable, so the daemon writes it to `startup-config` itself, as a
  `passwdhash` line added to the file as it is (the built-in default still runs while the file
  holds nothing else), and removes the command from the history, so *Save to flash* has nothing
  new to offer and the web interface shows no unsaved changes. If it cannot be written, the password
  is set anyway, the console prints why and that it will be lost at the next restart, and the command
  stays in the history as `passwd ****`. **Every later password change follows the 8051 model**: it
  takes effect at once and is kept only by *Save to flash*.
- **The startup configuration holds `passwdhash <hash>`**, not `passwd`. `passwdhash` is a command
  the platform adds; it sets the hash and prints nothing, and it refuses anything that is not
  a well-formed `$6$` hash. A saved configuration that contains a plaintext `passwd` line is rewritten into
  a `passwdhash` line before it reaches the disk, and only the last password line of a file is
  kept.
- **The command history is scrubbed.** The shared parser records every command, including
  `passwd <password>`. As soon as the command has run, the platform rewrites that history entry to
  exactly `passwd ****`, whatever the password was: neither its characters nor its length remain. So
  neither the console history, `GET /cmd_log` nor the merge of the history into the startup
  configuration by *Save to flash* contains the password. When *Save* merges a line
  `passwd ****` back, the platform keeps the current hash instead of hashing the asterisks. A password
  that consists only of asterisks therefore cannot be set through an upload; set it on the console.
- One session is valid at a time, as on the 8051: a new login ends the previous session, and so
  does a password change. A session ends after `session <seconds>` of inactivity (default 200).
  After a failed login every login is refused for one second.

## Configuration

The startup configuration is the file `<cfg-dir>/startup-config`, a list of CLI commands that is
replayed line by line on every start, as in the flash of the 8051. It is at most **4095 bytes**
(`CONFIG_LEN` is 4096, the area the 8051 keeps it in, and the text needs a terminating NUL). The 2048
bytes that the 8051's web editor allows are the limit of that editor only; the daemon takes
what the web interface uploads up to 4095.

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
  anything not saved is lost, as on the 8051. At every start the daemon puts the system in the state
  of a booting 8051 **before** it replays the file: every board port administratively up (also after
  `port <n> off` and on a cold start with the ports down), the default MTU (1500, `mtu show` 1526), every
  mode the port supports advertised; ports the file gives no `pvid` get PVID register 1; VLAN 1 is back
  on every port, untagged, unless the file creates or deletes it; VLANs the file does not create are
  removed (see [VLANs](#vlans-and-the-bridge) for how an existing bridge is adopted without a gap in
  switching). The exception is the kernel state the daemon does not own.
- **The built-in default and Save.** The effective startup configuration is the built-in default
  (every port an untagged member of VLAN 1 with PVID 1, VLAN 1 as management VLAN, DHCP on)
  followed by the file's lines when the file has no command but the password (it is missing,
  empty, or only holds the first password); a file with any other command is used alone. `GET
  /config` returns the effective text, so the web interface's *Save to flash* merges its changes into
  the default lines and the saved file keeps them: the next start still has DHCP and VLAN 1. A file that
  is uploaded or edited by hand with commands but without those lines gets no default.

## VLANs and the bridge

The switch fabric is the Linux bridge `swbr` with `vlan_filtering 1`, STP off, multicast snooping off (IGMP
is 2b) and `vlan_default_pvid 0`; every board port is a member. The VLAN table of the console and the web
interface is the VLAN table of the bridge: a VLAN is a set of bridge VLAN entries, the CPU port
(port 9 of the 8051) is the bridge itself, and a port's PVID is the PVID flag of its entry. The
management VLAN is untagged on the bridge, so the management IP address lives on `swbr`.
**The bridge itself is a member of the management VLAN only.** On a DSA switch the bridge's own VLAN
entry is the CPU port's membership in hardware: in every VLAN it would let the floods of every
VLAN reach the switch's CPU. `vlan.json`, `vlan show`'s CPU bit and the VLAN list still show the CPU
in every VLAN, as the 8051 pages expect. The management address is reachable from the management VLAN only.
A VLAN with no board port that is not the management VLAN has no bridge entry and does not exist (the
8051 can hold one with only the CPU). Deleting the management VLAN prints a warning: management
is then possible only through the console and the CLI socket. A VLAN change that fails on some
port is undone (members, tagging, PVIDs and name are as before; if even that fails it says so).

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

`ip dhcp` starts `busybox udhcpc -i swbr -f -S -s <script>` as a child of the daemon, and only when it
runs takes a static address away; if it cannot be started (no BusyBox) the address, the routes and `ip`
stay as they were and the error is printed. The script
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
changed through ethtool; a device that refuses the setting prints an error and keeps its state. The
modes are selected by speed on copper (`1000baseT`, `2500baseT`, ...) and on SFP+, optical and
backplane ports alike (`1000baseX`, `2500baseX`, `10000baseSR/LR/LRM/ER/CR/KR/KX4`): `port <n> 10g`
advertises the 10G modes the port supports and clears the others, `1g` only the 1000 ones.
Every kernel call is checked: a failed change is never reported as applied.

## L2 table

The table is the forwarding database of `swbr`. Learned and static entries of board ports are
listed with VLAN and port, together with the switch's own address (port `CPU`, in the management
VLAN, as on the 8051). The kernel's other local entries, entries in parking VLANs and entries
of other interfaces are not listed. The web interface addresses an entry by a 12-bit slot
number; the platform keeps a stable slot per entry, so a page that is being walked sees no
entry twice while entries come and go. One page of `/l2.json` (up to 30 entries) reads the kernel's
table once: the dump is reused for 250 ms, so the page may be up to 250 ms behind the kernel
(a delete or `l2 forget` drops it). The `l2` command prints the table, `l2 forget` removes the learned entries, the
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
make -C platform/linux testbin
sudo platform/linux/bench/run.sh
docker run --rm --privileged -v "$PWD":/w -w /w rtlplayground-dev platform/linux/bench/run.sh
```

`run.sh` runs every `t_*.sh` on a fresh bench and prints `ok <name>` or `FAIL <name>`; the exit
status is 1 if any failed. It runs the test build (`make -C platform/linux testbin`), not the production
binary. `setup.sh` refuses a run directory that is not a bench's (no owner marker), and `teardown.sh` deletes
a directory only if it holds the marker and lies under `/run/rtlp-bench*` or `/tmp/rtlp-bench*`. The tests are `t_smoke`, `t_cli`, `t_http`, `t_http_hostile`, `t_config`,
`t_ports`, `t_vlan`, `t_restart`, `t_l2` and `t_ui`, which fetches every route that `html/app.js`
requests (except `/upload` and `/reset`, which `t_http` covers) and fails when `app.js` gains a route the
test does not know. See
`platform/linux/bench/README.md` for the helpers. To look at the web interface of a bench by hand,
run the daemon in `rtlp-sw` and forward a port into the namespace, for example with `socat`
and `ip netns exec rtlp-sw`, inside a privileged container with its port published.
