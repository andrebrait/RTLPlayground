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
- Commands
  - `port <port> fc on|off` advertises 802.3x pause on a copper port, or not.
    The choice survives later speed and duplex changes.
- SNMP
  - Bounded GETBULK walks, interface administrative state, aliases, error counters
    and 32-bit multicast/broadcast counters for the optional read-only agent.
  - Configure the manager IP/CIDR, community and agent state directly in the Web UI,
    with masked credentials, Apply/Refresh controls and the existing Save to flash flow.

## Changed

- HTTP
  - `/status.json` carries the flow control (`fc`) and EEE (`eee`) state of every port; `/eee.json` is gone.
  - The chip port of each front-panel port moved from `logPort` in every `/status.json` entry to one
    `logPort` array in `/information.json`.

## Fixed

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
- Web UI
  - Accept firmware uploads regardless of the file part's MIME type, including
    `application/macbinary` sent by Chrome on macOS for `.bin` files.
- Health
  - `HEALTH=1` builds no longer write past the phase timing arrays at the LLDP checkpoint, which
    corrupted the `link` slow counter and the DHCP client state. #658
- Config
  - A startup configuration line longer than the command buffer no longer stops the replay:
    the offending line is skipped and the following lines are still applied.
  - The web UI and the configuration upload now refuse a line the replay cannot take,
    instead of writing it to flash.

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