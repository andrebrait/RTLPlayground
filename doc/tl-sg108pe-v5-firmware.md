# TP-Link TL-SG108PE V5 stock firmware

This note records what static analysis of TP-Link's public TL-SG108PE V5 firmware images shows. It is meant as groundwork for a possible port of RTLPlayground to the RTL8367C/RTL8370 family, discussed in [#287](https://github.com/logicog/RTLPlayground/issues/287).

The evidence comes from the update images, TP-Link's release notes, photographs of a V5.0 board and a read-only dump of that board's SPI flash. The switch was not flashed or reconfigured, and no modified image was uploaded. Inferences and untested behavior are distinguished from observations.

The comparison uses release notes as landmarks for reverse engineering: to separate stable hardware interfaces from changing software and connect changes to their likely purpose. It is not an audit of every advertised fix or a hardware-compatibility certification.

## Images examined

All three images are 1,110,868 bytes (0x10f354).

| Release | File | SHA-256 |
|---|---|---|
| 1.0.0, Sep 30 2022 | `SG108PE(UN) 5.0_1.0.0 Build 20220930 Rel.36161_up.bin` | `3eb6c84b31765abfd978d6ec11bd1ea7051c225cd82b50caedf561bae493cdad` |
| 1.0.0, Feb 18 2023 | `TL-SG108PE(UN) 5.0_20230218_up.bin` | `b21c1081d8d9aae55eef03da7ebe5d166285d5eeb9d1452d8c72ff08e478cc7c` |
| 1.0.1, Mar 30 2026 | `SG108PE(UN) 5.0_1.0.1 Build 20260330 Rel.42395_up.bin` | `8be5c999e4bbc213bf4c93d2986b6ca531ea50875f55a5b338c9420f26bc4634` |

TP-Link lists the 2023 and 2026 releases for hardware V5.6 as well; the 2022 release does not mention V5.6. The release dates come from the build strings inside each image.

All three images were imported into separate Ghidra 12.2 projects (8051, big endian), using release-specific entry points and bank-call tables. Mechanical decompilation was followed by manual inspection of selected routines. The DHCP builders at bank 5 CODE 0x40cc and 0x4384 required explicit seeding after the initial passes.

| Build | Code banks processed | Candidate function listings | Failed decompilations |
|---|---|---|---|
| 20220930 | 14 | 1,146 | 2 |
| 20230218 | 15 | 1,185 | 1 |
| 20260330 | 15 | 1,145 | 2 |

These counts include inferred boundaries, shared tails and runtime helpers; they are not counts of manually validated source functions or evidence of feature growth. Assembly remains available where decompilation failed. Keil calling conventions and banked pointers also make some generated prototypes and expressions unreliable.

## Chip and SDK

The images contain `RTL8367C`, `RTL8367N` and the Realtek SDK function name `rtl8367c_setAsicVlan4kEntry`. The startup data names the model `RTL8367N`. This places the firmware in the RTL8367C SDK family (RTL8367N/RTL8370N class), with an embedded 8051. These strings do not prove the package marking of the chip on the board.

The SDK used for comparison is the public mirror of Realtek's Unmanaged Switch Library 1.3.12 in [shiroichiheisen/Realtek-Unmanaged-Switch-Arduino-Library](https://github.com/shiroichiheisen/Realtek-Unmanaged-Switch-Arduino-Library). Its source files carry Realtek's restrictive license header; nothing from it is copied here beyond register addresses and names.

No usable open-source firmware for this switch was found during the research. OpenWrt cannot run on an 8051-only platform.

The [RTL8370N-VB datasheet](https://datasheet.lcsc.com/datasheet/pdf/858258fb4f2d165541ae61d6abdbbbb4.pdf?productCode=C2803243) (section 9.17) lists the embedded 8051's memory: 256 bytes of internal RAM, 48 KB of data memory, 16 KB of code memory with code banking, and an 8 KB NIC buffer. For comparison, an RTLPlayground build for an RTL837x board with SNMP and Prometheus enabled allocates 19,786 bytes of external RAM, so data memory is not the limiting factor for a port. The 139 bytes left for the stack in internal RAM are the tighter constraint.

## Update image header

Each image starts with a 20-byte header. An identical copy of the header appears at offset 0x4012 in all three images.

| Offset | Size | 2022 | 2023 | 2026 | Meaning |
|---|---|---|---|---|---|
| 0x00 | 4 | `50451085` | `50451085` | `50451085` | Identity; `PE` followed by `0x1085`, purpose of the second half unknown |
| 0x04 | 4 | `0010f340` | `0010f340` | `0010f340` | Payload length: file length minus 20 |
| 0x08 | 4 | `00000571` | `00000524` | `000004d1` | Header checksum |
| 0x0c | 4 | `042edd4c` | `043a4f81` | `044a1b51` | Payload checksum |
| 0x10 | 4 | `332255ff` | `332255ff` | `332256ff` | Unknown |

The header checksum is the sum of the other 16 header bytes:

```python
header_checksum = sum(header[0x00:0x08]) + sum(header[0x0c:0x14])
```

The payload checksum is a 32-bit sum of all bytes after the header, with the embedded header copy at 0x4012 counted as erased flash (0xff):

```python
payload_checksum = (
    sum(image[0x14:0x4012])
    + 20 * 0xff
    + sum(image[0x4026:])
) & 0xffffffff
```

Both formulas reproduce the stored values exactly in both header copies of all three images. This suggests the payload checksum is computed before the embedded header is filled in. That build order is an inference. Whether the updater checks these values the same way has not been tested.

The last four bytes are not understood. `3322` is the same in all three images. The byte at offset 0x12 changes from `0x55` to `0x56` between software 1.0.0 and 1.0.1. It does not track V5.6 support: the 2022 and 2023 images both carry `55` although only the 2023 one is listed for V5.6.

## Code layout and banking

The payload is plain 8051 code and data. It is neither encrypted nor compressed.

In the 2026 image, the instruction at file offset 0x14 is `LJMP 0x3388`, the reset vector. The common code is mapped at CODE address 0x1000, not 0:

```text
file offset = CODE address - 0x1000 + 0x14
```

Banked code uses a 48 KiB window at CODE 0x4000-0xffff, starting with bank 3:

```text
file offset = 0x3012 + (bank - 3) * 0xc000 + (CODE address - 0x4000)
```

In the 2026 image:

- SFR 0x96 selects the code bank. The common trampolines save it in SFR 0xbb, switch banks, call the target and restore it.
- 719 six-byte call stubs in the common area reach 15 distinct code banks.
- Generic data pointers select their bank as `(R3 - 1) & 0x3f`.

The two-byte offset between the common area and the bank windows comes from the flash layout, described below: the flash starts with a two-byte prefix, so CODE address `a` in the common area lives at flash offset `a + 2`.

## Switch register access from the 8051

The common code contains the routines the 8051 uses to read and write switch registers:

| Build | Read routine (CODE) | Write routine (CODE) |
|---|---|---|
| 20220930 | 0x3796 | 0x381c |
| 20230218 | 0x37a2 | 0x3828 |
| 20260330 | 0x3862 | 0x38e8 |

Their behavior, reconstructed from the assembly, is:

```text
read16(address):
    disable interrupts
    SFR 0xa2 = address high byte
    SFR 0xa3 = address low byte
    SFR 0xa0 = 1
    repeat:
        11 x NOP
    until SFR 0xa1 == 0
    value = SFR 0xa6 << 8 | SFR 0xa7
    enable interrupts

write16(address, value):
    disable interrupts
    SFR 0xa2 = address high byte
    SFR 0xa3 = address low byte
    SFR 0xa4 = value high byte
    SFR 0xa5 = value low byte
    SFR 0xa0 = 3
    repeat:
        3 x NOP
    until SFR 0xa1 == 0
    enable interrupts
```

The polling loops have no timeout, and both routines enable interrupts on exit instead of restoring the previous state. All three images use the same delays: 11 NOPs per poll for reads and 3 for writes. This matches the NOP padding around register access mentioned in #287.

Ghidra names these SFR addresses after standard 8051 parts (`P2`, `FIE`, `WDTRST` and so on). Those names do not apply to this chip.

The MIB counter reader in bank 10 (CODE 0x78a0) uses the RTL8367C register layout from the SDK: the counter address register 0x1004, the busy flag in 0x1005, the counter data in 0x1000-0x1003 and a per-port stride of 0x7c.

## UART

Bank 3 CODE 0x4000 sets bit 2 of register 0x1d32, which the SDK names `UART_EN`, and configures Timer 2 and the serial port. The divider is derived from 0x12c000 = 38400 × 32, so the console is expected to run at 38,400 baud. The rate has not been measured.

Menu strings in the image offer clearing the screen, printing the IP configuration, help, entering loader mode, setting the MAC address and rebooting. Whether the board exposes the UART pins is not known.

## Startup data and PoE control

The startup routine copies initialised data into XDATA from a table of records in bank 3. Each record is a tag byte (`0x40`, length in the low six bits; with `0x20` set, a 13-bit length spread over the tag and the next byte), a 16-bit XDATA address and the data.

| Release | Records | Table start (CODE) | `01 05 08 09` loaded at XDATA |
|---|---|---|---|
| 2022 | 137 | 0x6573 | 0x95f0 |
| 2023 | 148 | 0x6573 | 0x9bde |
| 2026 | 157 | 0x6599 | 0x9be2 |

In the 2026 image, the PoE recovery code looks up `XDATA[0x9be1 + port]` for PoE ports 1 to 4 and passes the value to the GPIO output routines (bank 13 CODE 0x4298/0x429f, through bank 12 CODE 0x6360 and bank 10 CODE 0xa399). This gives:

| PoE port | GPIO |
|---|---|
| 1 | 1 |
| 2 | 5 |
| 3 | 8 |
| 4 | 9 |

The older decompilations now confirm the same lookup through bank 13 CODE 0x4298/0x429f: `XDATA[0x95ef + port]` in 2022 and `XDATA[0x9bdd + port]` in 2023. Both pass the selected byte to the GPIO backend through bank 12 CODE 0x6360. The software assignment of PoE ports 1-4 to GPIOs 1, 5, 8 and 9 therefore holds across all three builds, not merely their initializer data.

The PoE controller part, GPIO polarity and board wiring have not been checked. Stable software mappings and register timing do not establish complete board equivalence or V5.6 cross-flash safety.

The PoE recovery task (bank 13 CODE 0x4ff0) keeps per-port timers, watches received packets, turns PoE off and back on, and counts retries. A separate "extend" mode changes the PHY abilities of the PoE ports.

## Web interface

The firmware runs TP-Link's own IP stack, "lwps". Source paths in the image name `common/src/lwps/` modules for TCP, UDP, ARP, ICMP and IGMP snooping, and `common/src/app/` modules for DHCP and the web server. No public source for lwps was found.

88 web routes were matched to candidate handlers. The port statistics page `/PortStatisticsRpm.htm` (bank 17 CODE 0x7f8d) reads the hardware MIB counters and returns them as JavaScript, four values per port in this order: TX good packets, TX bad packets, RX good packets, RX bad packets. Port and link state are included; byte counters are not. `/port_statistics_set.cgi` clears the counters.

That page could be scraped by an exporter running elsewhere, without changing the switch firmware. Login handling for such a scraper has not been worked out.

All three images contain the same base64 RSA public-key blob and signature-checking code. In the 2026 image, `SignCheck` at bank 23 CODE 0x814c reads the flash UID and a stored signature. When and whether that check is enforced remains unknown. The additional RSA-decryption path introduced in 2023 is discussed below; key presence alone does not identify that change.

## Cross-release findings and release-note context

The firmware cards were extracted from the saved page `Download for TL-SG108PE _ TP-Link.mht`, captured on 2026-11-02 from the [V5 download page](https://www.tp-link.com/us/support/download/tl-sg108pe/v5/#Firmware). Firmware notes were kept separate from the Easy Smart Configuration Utility notes on the same page.

### September 2022: accessibility baseline

The release was published on 2022-10-21. TP-Link describes expanding all menu bars for VoiceOver and similar software, lists V5 only, and says configuration is retained after upgrading.

The image contains the dynamic menu implementation. Without a pre-September-2022 image, the historical change from collapsed to expanded menus cannot be independently established from this comparison. No browser or screen-reader test was run.

### February 2023: a new decryption path

The release was published on 2023-02-20 and still identifies itself as software 1.0.0. TP-Link describes an Easy Smart management-protocol security upgrade, adds V5.6 to the supported-hardware wording, and says configuration is retained while recommending a backup.

The comparison found more than new strings:

- `RSA_private_decrypt error` and `RSA_private_decrypt success` are referenced by bank 22 CODE 0xa264 in 2023 and CODE 0xa447 in 2026. These diagnostics are absent from the 2022 image.
- Those helpers are called from larger packet-processing routines at bank 22 CODE 0xd948 in 2023 and CODE 0xdbbf in 2026. Common stub 0x128f reaches the respective packet-processing entry.
- The web-login handlers also change: bank 17 CODE 0xdfa0 in 2022, CODE 0xe2db in 2023 and CODE 0xe4af in 2026. This does not establish that the RSA call path serves web login.

The called decryption routines support the management-protocol explanation in the release notes. The public key and generic RSA/signature code were already present in 2022, so finding a key alone would have missed the distinction. An exact protocol specification, security-strength assessment and live compatibility test were not produced.

This does not prove that the protocol update was the only change needed for V5.6.

### March 2026: validation in both browser and server

The release was published on 2026-05-27 as software 1.0.1. TP-Link lists stricter Device Description validation, an RFC 2131 DHCP fix, a single-router-port IGMP forwarding fix, and a stored-XSS fix reported on V5.6.

The Device Description browser validators in 2022 and 2023 are identical. They allow letters, digits, underscores and hyphens, including an empty string and leading/trailing hyphens. The 2026 validator rejects empty strings, underscores and leading/trailing hyphens.

The extracted JavaScript functions were executed offline, with 18 assertions across the three builds:

| Input | 2022 | 2023 | 2026 |
|---|---|---|---|
| `switch-office` | accepted | accepted | accepted |
| `switch_office` | accepted | accepted | rejected |
| `-switch` | accepted | accepted | rejected |
| `switch-` | accepted | accepted | rejected |
| empty string | accepted | accepted | rejected |
| `switch office` | rejected | rejected | rejected |

These results concern the validator functions, not every constraint imposed by the complete form or server. The input field separately limits the description to 32 characters.

The server-side change is also visible. `/system_name_set.cgi` reaches bank 17 CODE 0xa3ed in all three builds. The older handlers pass the extracted name to the configuration setter without the new explicit format check. In 2026, the handler first calls bank 6 CODE 0xa169, compares its return value with 0xffffffff, and skips the setter on validation failure. The helper checks endpoint hyphens and character ranges for letters, digits and hyphens.

The configuration setters are at bank 5 CODE 0xbd1b, 0xbd22 and 0xbf02 in the 2022, 2023 and 2026 builds, respectively. Other management paths may share storage without sharing the same validation. This corroborates the announced hardening, not a complete audit or reproduction of the stored-XSS fix.

### Device Description also supplies the DHCP hostname

The packet builders at bank 5 CODE 0x40cc and 0x4384 obtain the description through the configuration getter and include it in DHCP hostname option 12. The getter is at bank 5 CODE 0xbdd7 in 2023 and CODE 0xbfb7 in 2026; getter and setter use configuration attribute 0x17 and a 33-byte buffer. The builders also contain the DHCP magic cookie `63 82 53 63` and use UDP destination port 67.

Consequently, description validation affects both the management UI and the hostname sent by DHCP. This is a recovered data-flow connection, not proof that validation alone explains the advertised RFC 2131 fix. The exact packet-level correction was not isolated or checked against a capture.

### IGMP: a useful landmark, not a reproduced fix

The 2026 image adds `vlanPmask is 0x%lx` alongside existing IGMP-router-port diagnostics. That is consistent with work in multicast/VLAN forwarding, but a string alone does not demonstrate the correction.

The complete single-router-port decision path was not manually reconstructed across releases, and no packet-forwarding experiment was performed. The IGMP fix remains a vendor release-note claim rather than independently verified behavior.

### Conflicting hardware-version wording

The saved download page labels the 2026 firmware for **V5.0/V5.6**, but its [linked release-note PDF](https://static.tp-link.com/upload/firmware/2026/202605/20260528/SG108PE(UN)%205.0_1.0.1%20Build%2020260330%20Rel.42395_up.pdf) says:

> Version Info: Firmware for TL-SG108PE(UN) 5.40.

The fix lists otherwise agree, including the stored-XSS issue reported on V5.6. The conflicting wording is observed; a publication mistake is only a possible explanation. Neither this discrepancy nor the shared image identity establishes cross-flash compatibility.

### What the additional releases established

The comparison separates stable hardware-facing behavior from changing management software: the GPIO assignment and register timing persist while decryption and validation paths change. Multiple releases also turn the header-checksum hypotheses into repeatable observations, while the description-to-DHCP connection explains an additional consumer of a management setting.

Release notes provided landmarks for those discoveries. Unresolved advertised fixes are limits of this investigation, not a requirement to audit every vendor claim before the findings are useful.

## Board and flash

These observations come from one TL-SG108PE(UN) Ver 5.0 board running software 1.0.1 Build 20260330.

### Board

- The switch chip sits under a heatsink. Its pin 1 is at the bottom-left corner, which puts the Ethernet pairs (pins 1-64 and 103-128) on the edges facing the magnetics and places the SPI flash pins (67-70) next to U3 and the crystal pins (97/98) next to the 25 MHz crystal.
- U3 is the SPI flash: a cFeon/ESMT `EN25QH16B`, 16 Mbit (2 MiB), rated for 2.3-3.6 V ([datasheet](https://www.endrich.com/Datenbl%C3%A4tter/Aktive%20Komponenten/EN25QH16B%20%282A%29.pdf)). flashrom identifies it as `EN25QH16` with JEDEC ID `1c 7015`.
- U4 is an MPS MP3924, next to the PoE MOSFETs and 0.25 Ω sense resistors. The two EEC `DG48201P1-S` magnetics serve the four ports next to that section.
- U5 is a Cmsemicon `CMS8S5880`, a separate 8051 microcontroller with 16 KB of flash. Its position next to the PoE section suggests it manages PoE; that is an inference.
- J2, a five-pin header next to U5, is not the switch chip's console. From the left: pin 1 is wired to pin 5 through an unpopulated resistor (R59); pin 2 is idle at 0 V; pin 3 shows a changing voltage of roughly 1.3-1.8 V; pin 4 is ground; pin 5 (square pad) is 3.3 V. A USB serial adapter on pin 3 received nothing at 38,400 baud over about 70 seconds, with the switch running and without a power cycle. J2 is most likely U5's programming header.
- No labelled UART pads for the switch chip were found on either side of the board. The datasheet does not list UART pins. On the RTL8367N, UART_RX shares a pin with GPIO1; whether the RTL8370N does the same has not been checked.
- Circuit ground is isolated from the chassis.

### Flash layout

The flash was read twice with a CH341A programmer at 3.3 V and a SOIC-8 clip, with the switch unpowered; both reads were identical. The layout matches the RTL837x layout described in [ghidra.md](ghidra.md):

| Flash offset | Contents |
|---|---|
| 0x000000 | Two-byte prefix `00 40` (the 0x4000-byte prefetch size), then TP-Link boot code for CODE 0x0000-0x0fff |
| 0x001002 | Firmware common code for CODE 0x1000-0x3ffd |
| 0x004000-0x01bfff | Banks 1 and 2, belonging to the boot code |
| 0x01c000 + (bank - 3) × 0xc000 | Firmware banks 3-25 |
| 0x01d000 | Update header, inside the bank 3 region |
| 0x130000-0x1fbfff | Erased (816 KiB) |
| 0x1fc000 | Device identity block |
| 0x1ff000 | Settings |

The update image contains only the firmware part. Its common code (file offset 0x14) is stored at flash 0x1002, and its banks (file offset 0x3012 onward) at flash 0x1c000 onward; all 23 bank-sized chunks of the 2026 image match the flash byte for byte. The header copy at file offset 0x4012 therefore lands at flash 0x1d000. The boot code in CODE 0x0000-0x0fff and banks 1-2 is not part of the update image.

The boot code has its own network stack strings (TCP states), flash-handling routines and a message about loading the MAC address from configuration storage. Whether it offers a recovery mode has not been checked.

The identity block at 0x1fc000 contains the model name, the firmware version string, a 40-character hexadecimal string and binary data. Its format, and whether the boot code or firmware verifies it, are not known. Because it is written per device, a flash dump must not be copied to another unit. The settings area at 0x1ff000 contains the configured device description among other values.

For a port, the shared layout means RTLPlayground's existing image format, linker bank addresses and bank-switching approach should carry over; this has not been tested.

## Open questions

- The meaning of `0x1085` and of the trailing `3322 55ff`/`3322 56ff` header bytes.
- Whether the boot code or updater accepts an image that only satisfies the two checksums, or also checks the identity block.
- Where the switch chip's UART is routed, and what U5 does.
- Whether hardware V5.6 differs in a way the 2022 firmware does not handle.
