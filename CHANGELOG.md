# Change Log

## [0.x] - 2026-xx-XX

## Added

- Devices
  - Support the hongyavision HYWS-SGT0204S (PCB marking `2G5F4_10G2_V1.01`), an
    RTL8372 based 4x2.5G + 2xSFP+ unmanaged switch with a 512 KB Bright Moon
    `T25S40` flash. It is a sibling of the KP-9000-6XH-X2 (2M-PCB43-V1.1) with
    the two SFP+ cages wired the other way round (physical 5 is on SDS1,
    physical 6 on SDS0), no reset button fitted, wrong silkscreen labels on the
    serial header, `LED_GLB_IO_EN` bit 6 has to be set for the port LEDs to
    work, and the fitted T25S40 cannot be read in DIO mode on this board, so the
    target selects single-IO flash reads (`flash_sio`). New machine target
    `MACHINE_2G5F4_10G2_V1_01`, see `doc/devices/2G5F4_10G2_V1.01.md`.
  - Experimental support for the TP-Link TL-SG108PE V5, an RTL8370N
    based 8-port gigabit switch with PoE on ports 1-4 (`MACHINE=TPLINK_TL_SG108PE_V5`,
    `RTL8370_DUMP=<flash dump>` for the PHY patch). The image sets up the switch,
    PoE, the NIC and the web UI. Native hardware MIB counters feed the dashboard,
    Statistics details and CLI: packet totals sum unicast/multicast/broadcast
    counters with 64-bit carry; error columns specifically count TX excessive
    collisions and RX FCS errors, not all errors. Details expose all 58 native
    per-port counters (64-bit octets, otherwise 32-bit) without fabricated queue
    counters. Busy/reset/selector failures and unsupported MAC occupancy are
    unavailable (`null` in JSON, `-` in the UI, `n/a` in the CLI).
    Native port controls support PHY enable/disable, auto-negotiation, advertised
    10/100 Mbps modes and duplex, gigabit full duplex, and read-only `port show`.
    Speed selection retains auto-negotiation and preserves pause/master settings.
    The UI hides unsupported 2.5G controls; custom advertisements are
    displayed without silently replacing them. Saved duplex changes retain the
    speed command they modify.
    EEE and flow control use the shared Ports page and `/status.json` fields:
    EEE sets native PHY advertisement and MAC capability for 100M/1G, and
    `port <port> fc on|off` sets symmetric pause, both reporting negotiated state;
    the link partner's EEE abilities remain unavailable. EEE is on by default for
    100M and 1G; a saved `eee off <port>` turns it off. The PoE switch sits in
    the same row, shows n/a on ports without PoE and is sent only when it changes.
    GPIO-backed PoE administration is available for physical ports 1-4 through
    `poe port <port> on|off`, the Ports page, dashboard indicators and port details.
    The UI distinguishes permission to supply PoE from actual power delivery,
    which the switch cannot measure. PoE-only changes do not restart the
    Ethernet PHY or touch the controller reset.
    LLDP sends and receives on the RTL836x: the NIC is the switch's port 10, so
    it is now the CPU port and per-port LLDP frames leave only their own port, and
    the RX descriptor's ingress port tells which port a neighbour is on. A PD that
    announces 802.3 Power via MDI or LLDP-MED power shows af or at, its class and
    requested power on the LLDP page and in the port details. The optional
    802.1 Port VLAN ID and 802.3 MAC/PHY Status, Link Aggregation and Maximum
    Frame Size TLVs are sent; Link Aggregation reports whether the port is in a
    static LAG. The maximum frame size warning on the LLDP page
    works as on the RTL837x. LLDP is trapped to the NIC on chip port 10 while
    enabled; `lldp off` restores forwarding between the front ports.
    802.1Q VLANs work as on the RTL837x: the `vlan`, `pvid` and `ingress`
    commands, `/vlan.json`, `/vlanlist` and the VLANs page. At boot every port is
    an untagged member of VLAN 1 with PVID 1. The CPU port is an untagged member
    of every VLAN, so management stays untagged in VLAN 1 and there is no
    management VLAN setting. On the switch, moving front port 3 alone into
    VLAN 10 stopped all VLAN 1 traffic to it while management stayed reachable
    on port 8, and restoring VLAN 1 brought the traffic back.
    Port isolation works as on the RTL837x: the `isolate` command,
    `/isolation.json` and the Port isolation page.
    The maximum frame size works with the `mtu` command, `/mtu.json` and the
    Ports page, but the chip holds only two lengths (MAX_LEN_CFG0 and CFG1) for
    all ports, so a third different length is refused.
    Port mirroring works with the `mirror` command, `/mirror.json` and the
    Mirroring page, but the chip has one source set for both directions, so the
    mirrored ports must all be mirrored RX and TX, or all RX only, or all TX only.
    Static link aggregation works with the `lag` and `laghash` commands,
    `/lag.json` and the Link aggregation page, but the chip has two fixed groups,
    ports 1-4 and ports 5-8, that share one hash, so `lag 1` and `lag 2` take
    ports from their own range only and `laghash` sets the hash for both. A
    member's PVID goes to the whole group.
    LACP runs on the same two groups with `lag <n> lacp <port>...`, `lacp show`,
    `lacp off`, `/lacp.json` and the LACP mode of the Link aggregation page.
    The candidates must be ports of the group's own range, otherwise the
    command is refused without changing the group's LACP or static settings. Only
    ports that negotiated with one partner system become members in the chip, so
    until a partner answers, they stay ordinary links; `/lag.json` shows the
    members the chip holds, not the candidates. The LACPDUs reach the CPU by the
    reserved multicast trap for `01:80:c2:00:00:02`.
    STP/RSTP uses the native MSTI0 state fields, port status and dynamic-only FDB
    flush. Per-port STP off keeps ordinary forwarding, including during startup.
    A hardware test verified blocking, learning and timed forwarding on port 3
    while excluded ports stayed forwarding; blocked-port BPDU delivery and
    multi-bridge interoperability remain unverified.
    The refreshed timer integration was also exercised on hardware: automatic
    edge recovery, clock progression, LLDP reception and the native port/LAG UI
    passed, with the saved configuration unchanged.
    The MAC table uses the native LUT with the existing table page and paginated
    `/l2.json` rows. Exact-index deletion preserves the full lookup key and
    protects the switch's own MAC. Bounded busy failures report unavailable
    data rather than a fabricated empty table; deletion failures leave the UI row.
    Native host tests cover SRAM/CAM indices, CPU mapping, LAG association,
    pagination, key-preserving deletion and busy refusal.
    Build `58a99c49` listed 52 live entries across three pages; the browser later
    rendered 63 with correct front-panel and CPU labels. Deleting the switch's
    own MAC was refused. Deleting one learned neighbour removed only that entry,
    a repeat deletion was refused, and normal traffic relearned it after 7.2 s.
    The test made no link or PoE changes; the saved configuration was unchanged.
    Full-capacity coverage is a host/browser-fixture test, not a silicon table-fill
    test.
    Native bandwidth and storm control use 8-kbit/s steps from 8 to 1000000
    kbit/s on the eight front ports. Storm control supports kbit/s only; PPS is
    refused. Host tests cover limits, retained meter modes, packed fields and
    calibration preservation. Hardware checks cover port-3 readback, invalid
    requests, browser Apply and rate-dependent egress delay; calibrated ingress
    and storm throughput remain unverified.
    Native IGMP uses the validated shared parser with native traps and IPv4
    multicast LUT entries through `igmp on|off|show`. Host tests cover native
    RX headers, retained state, membership updates and bounded failures.
    Hardware checks verified real IGMPv3 join/leave, unknown-group filtering,
    representative link-local multicast flooding and unchanged saved settings.
    Cross-port listener forwarding and post-leave filtering also passed.
    Existing first-record IGMPv3 behavior and IP-based (not VLAN-specific)
    groups are retained.
    Cable test is not ported. Temperature remains unavailable without a
    verified sensor mapping.
    The SNMP agent and Prometheus exporter build with `SNMP=1` and `PROMETHEUS=1`:
    counters come from the native MIB, link, speed and admin state from the port
    status and PHY registers. Interface errors are summed from native counters as
    the Linux rtl8365mb driver does; there is no SoC temperature metric. A full
    SNMP walk and repeated scrapes ran on the switch without a reset.
    Hardware checks passed for standalone boot, DHCP, ping, web login, settings
    persistence, web firmware updates and increasing native packet/byte counters
    on the live network. Port 1 negotiated 10M full, 100M half/full and returned
    to Auto after disable/enable; the uplink stayed at 1G. Pause advertisement
    and negotiated off/symmetric/TX-only states were checked on hardware.
    Build `32a33d74` also passed live PoE UI checks: toggling physical port 2
    changed only GPIO 5, its link dropped, and 100M link plus incoming camera
    traffic resumed after re-enabling it. Other PoE ports and the uplink were
    unchanged; saved configuration and GPIO state were preserved.
    EEE configuration readback passed on ports 1 and 2; neither negotiated LPI
    with its peer. A later manual check showed EEE active on two ports, so
    negotiation works with capable peers. One HTTP status request
    timed out during pause renegotiation; the instrumented follow-up completed
    normally. Browser DOM and action checks passed; screenshot capture timed out.

- Web UI
  - Compress the embedded assets (minify + gzip) and switch to a single-page layout. #315
  - Replace the multi-page UI with a themed single-page app: light, dark and Selenized themes following the browser by default,
    English, Japanese and Chinese, save to flash merges the command log into the startup config and verifies the write,
    firmware images are checked in the browser before upload. #429
  - Ports page: flow control and EEE sit next to the other port settings, each as a switch with its
    state (TX/RX, Active, Inactive or Off); ⓘ buttons show what the port and its link partner
    advertise, and the link speed is shown in the colour of the port strip. One Apply button sends
    the changed ports, listing the commands first, and leaving with unapplied changes asks first.
    Devices learned moved to the Statistics page. The separate EEE page is gone.
    EEE details include 5G and 10G abilities for external copper PHYs; SFP ports remain n/a.
- Commands
  - `port <port> fc on|off` advertises 802.3x pause on a copper port, or not.
    The choice survives later speed and duplex changes.
  - `port <port> fc` and `eee on|off` leave the link up when the port already has that setting.
    `eee on <port> 100m` now also stops advertising 2.5G and 5G EEE.
- SNMP
  - Bounded GETBULK walks, interface administrative state, aliases, error counters
    and 32-bit multicast/broadcast counters for the optional read-only agent.
  - Configure the manager IP/CIDR, community and agent state directly in the Web UI,
    with masked credentials, Apply/Refresh controls and the existing Save to flash flow.

## Changed

- Web UI
  - The web session expires after one hour without a request (was 200 seconds). The UI stops
    polling while its tab is hidden, so with 200 seconds switching tabs for a few minutes logged
    you out. `session <seconds>` still sets it.
  - Saving to flash leaves out `port <n> on|off`, `eee on|off <n>`, `port <n> lldp permit|block`
    and `lldp on|off` when their last line sets what the switch starts with, together with every
    earlier line for that setting, so switching a setting back lets the default apply again. The
    defaults are build-time values in `machine_defaults.h`, which a machine header may override;
    the boot code sets them and `/defaults.json` reports them. Flow control, MTU, SFP ports, and
    lines that speed, duplex or qualified `eee` lines depend on are always kept. EEE commands on
    mixed-speed boards are also retained because boot and CLI advertisement defaults differ.
  - Reuse the JSON string appender to reduce BANK1 code usage without changing responses.
    The appender is shared in the common area, so the STP pages in BANK4 and the HTTP
    responses in `httpd.c` use it too.
  - Rename the Prometheus section to Metrics, keeping the toggle labelled Prometheus exporter.
  - Place LLDP warnings beside the affected maximum frame size, aggregation state
    and chassis ID. Open warning details for local and advertised values; read
    existing MTU and LAG endpoints on demand without changing firmware defaults.
    Closing a warning restores keyboard focus to the correct neighbour even when
    multiple neighbours share a physical port and the table refreshes.
  - The LLDP neighbour table shows each neighbour's link from its 802.3 MAC/PHY
    Status TLV: the MAU type, such as 1000BASE-T FD, and whether
    auto-negotiation is on. `/lldp.json` has them as `mau` and `aneg`, and
    `lldp show` prints `mau <type>`, followed by `auto` when it is on.
- HTTP
  - `/status.json` carries the flow control (`fc`) and EEE (`eee`) state of every port; `/eee.json` is gone.
  - The chip port of each front-panel port moved from `logPort` in every `/status.json` entry to one
    `logPort` array in `/information.json`.

- Time
  - The uptime in seconds (sessions, NTP, the reset button) is counted in XDATA instead of
    `RTL837X_REG_SEC_COUNTER`, so it no longer depends on SerDes 0 being on.

- HTTP
  - `/cmd_log_clear` answers with an empty 200 instead of the MTU table.

- Serial
  - Waiting for the UART to take a character gives up after a bounded time, and a
    UART that timed out once is no longer written to, so a UART that does not run
    cannot hang the firmware.

## Fixed

- Experimental TL-SG108PE V5 console
  - Execute the vendor settling delays before polling switch-register commands. Stop further
    register and NIC DMA commands after a controller timeout, rather than using stale data.
- Commands
  - Reject malformed mirror port tokens, invalid source suffixes, and extra arguments
    after `mirror status` or `mirror off` without changing the mirroring configuration.
  - Reject incomplete LAG group numbers and extra arguments after `lag show`,
    `lag <group> d` or `lag <group> lacp off` without changing configuration.
  - Reject extra arguments to `lacp off` and `lacp show`. A malformed
    `lacp off 1` no longer silently disables every LACP group.
  - Reject extra arguments to global `stp on`, `stp off`, `stp status` and
    `stp clear` before changing state. Rejecting an invalid per-port `edge`
    value now preserves the existing edge configuration.
  - Reject extra arguments to `bw status` and bandwidth changes before altering
    limits or flow-control mode.
  - Report invalid storm-control commands as errors instead of returning success
    after printing usage.
- IGMP
  - Validate received frame and IPv4 lengths, fragments, and every IGMPv3 record
    before accessing the ASIC. Truncated reports no longer use stale buffer
    bytes to change multicast membership.
  - Reject extra arguments to `igmp on`, `igmp off` and `igmp show` before
    changing state or accessing the ASIC.

- Time
  - `rtl8372_init()` sets SDS 0 to SGMII as the stock firmware does. The masks were 0, so on RTL8372
    boards without 10G ports both SerDes stayed off, and with SDS 0 off `RTL837X_REG_SEC_COUNTER`
    does not hold a written value: NTP never retried after a failed first attempt at boot and
    froze the clock after a sync.

- Web UI
  - Keep an L2 entry visible when the switch rejects its deletion.
  - Firefox no longer fills a saved login into the new admin password fields. It also put the
    user name into the NTP poll interval, which marked the Time card as edited, so it kept its
    defaults (NTP off, no server, UTC-12:00) instead of the switch's settings.
  - Accept firmware uploads regardless of the file part's MIME type, including
    `application/macbinary` sent by Chrome on macOS for `.bin` files.
  - Store embedded assets uncompressed when gzip would not make them smaller, with the
    compression flag set separately for each file. Empty files no longer hide later assets.
  - Give the LLDP Name column the remaining table width and wrap long names.
  - Refuse MAC-table deletion when the requested index has aged out or is out of
    range, instead of using the next entry's data for a write.
  - Load the complete MAC table even when responses contain fewer than 30 rows.
    Stop at an empty page or a repeated entry, not an assumed page count.

- Health
  - `HEALTH=1` builds no longer write past the phase timing arrays at the LLDP checkpoint, which
    corrupted the `link` slow counter and the DHCP client state. #658
- Networking
  - Wildcard UDP listeners reply to the current sender without retaining its
    address or transmitting stale replies during periodic polls.
- SNMP
  - Valid requests exceeding the stored varbind limit return tooBig instead of timing out.
  - A transient hardware timeout no longer disables monitoring until reboot;
    reads resume once the SFR engine and statistics selector are idle.
  - Align SNMP status labels and values with the existing settings-row spacing,
    typography and theme colors.
- PHY
  - Setting a port's duplex on an autonegotiated link now changes only the duplex bits of the
    advertisement. It used to clear the rest of the register, selector field and pause included.
  - Report EEE activity on logical port 8 in both the web UI and `eee status`.
- Config
  - A startup configuration line longer than the command buffer no longer stops the replay:
    the offending line is skipped and the following lines are still applied.
  - The web UI and the configuration upload now refuse a line the replay cannot take,
    instead of writing it to flash.

- Startup
  - Initialised and zero-initialised globals above xdata 0x4000 now get their values at boot.
    Hardware reset leaves SFR 0x97 at 1, which maps xdata 0x4000-0xbfff to the flash image,
    and it was cleared only in `main()`, after the C startup code had copied and cleared them.
    This showed as a random web session timeout and LLDP blocked on some ports without config.
- Serial console
  - Keys that send an escape sequence the line editor does not use (Home, End, PgUp, the function keys,
    Ctrl-arrows, ...) no longer hang the switch. The sequence is now ignored. #664

- LLDP
  - An LLDPDU with a malformed Port ID (no identifier octets, a reserved subtype, a MAC address
    that is not six octets) or a truncated TLV is ignored instead of publishing a neighbour with
    an empty Port ID or the default TTL.

## Breaking changes

- Config
  - VLAN don't accept port `u`-suffix anymore.
    So `vlan 1 4u` is not valid anymore.
    Replace it with `vlan 1 4`.
- Commands
  - port zero/`0` is treated as the `CPU_PORT`. #326
  - Many commands don't accept the CPU_PORT anymore. See #334.
    When the CPU_PORT is needed, the command/service will add the CPU_PORT automaticly.
    Only `isolate` accept the CPU_PORT as destination port.
- HTTP
  - Scripts reading `/eee.json` or `logPort` from `/status.json` need `eee` from `/status.json`
    and the `logPort` array of `/information.json` instead.

## [v0.1_aplha] - 2025-11-03

First release.