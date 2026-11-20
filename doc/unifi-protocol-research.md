# UniFi controller integration: protocol research

Research date: 2026-11-20. This note is the basis for deciding whether RTLPlayground
switches can be adopted and controlled by the UniFi Network Application as if
they were UniFi switches (USW). It covers the existing reverse-engineering work,
the inform protocol, the switch-specific configuration format, model
impersonation, and the feasibility of all of this on the RTL837x 8051.

Scope and method:

- Third-party projects were cloned and read at the source level. Line references
  are to the revisions current on the research date.
- The UniFi Network Application 10.6.109 (Linux "unix" zip,
  `md5 98097d4d6722c387885973f880f4b99f`, located through the public
  `fw-update.ui.com` API) was downloaded. Its bundled data files were read
  directly. For the Java code, only the **string constant pools** of selected
  classes were listed with `javap -v`. No decompilation was done. Statements
  that rest on string constants alone are marked *(string evidence)*. They show
  that a check or key exists, but not exactly how it is used.
- Most of this note was written without a running controller. A later lab
  run against a stock UniFi Network 10.6.109 controller with *emulated*
  switches is reported in
  [Lab results](#lab-results-unifi-network-106109-emulated). Items it settled
  are marked **confirmed in the lab** or **refuted in the lab**. Claims taken
  from other projects' lab results are attributed to them. Items marked
  **unverified** have no primary source or only one weak source.

## Summary

1. **Someone has already adopted an emulated switch into a current controller.**
   [MartinDejmal/unifi-simulator](https://github.com/MartinDejmal/unifi-simulator)
   (Python, October 2026) reports adoption and the Connected state as US8 and
   US24PRO2 on UniFi Network 10.6.106. Port states, speeds, byte counters,
   `mac_table` clients and `lldp_table` neighbours were all accepted.
   [JohnKiller/TNBU](https://github.com/JohnKiller/TNBU) (C#, August 2026) has a
   protocol library, a MitM proxy, a device simulator and an alternative
   controller that provisions a real USW-Pro-48-PoE. TNBU is also the only
   source for the `switch.port.N.*` / `switch.vlan.N.*` system_cfg format. None
   of the switch emulators found *applies* controller config to real hardware
   in a validated way. RTLPlayground would be the first.
2. **The protocol is small and well understood.** It is an HTTP POST to
   `:8080/inform` with a 40-byte `TNBU` header, optional zlib, and AES-128
   (CBC with PKCS#7, or GCM with a 16-byte nonce and the header as AAD). The
   payload is JSON. Adoption is three steps. First the device informs with the
   public key `ba86f2bbe107c7c57eb5f2690775c712` (MD5 of "ubnt") and gets HTTP
   404 until an admin clicks Adopt. Then a `setparam` delivers `mgmt_cfg` with
   a per-device `authkey`. Finally a `setparam` with `system_cfg` and
   `cfgversion` follows. Neither SSH nor UDP discovery is needed for layer-3
   adoption.
3. **The controller decides the port layout from the model code**, not from what
   the device reports. The table is the bundled `UniFi/dl/uidb/uidb.json`
   (698 devices, with `numberOfPorts`, `ports.standard/sfp/plus` and a
   `diagram`). The Java code also hardcodes lists of USW model codes, and logs
   `unable to manage dev[{}] with model[{}]` *(string evidence)*. So inventing a
   model code is not practical. Impersonation is. There are good matches for
   the RTLPlayground layouts: `US6XG150` (4 RJ45 plus SFP+ on ports 5 and 6)
   for 4+2 devices, `USWED36` / USW-Flex-2.5G-8 (8 ports plus a 9/10 combo
   10G uplink) for 8+1 devices, `USWED35` / USW-Flex-2.5G-5 (itself built on a
   Realtek RTL8372N) for 5-port devices, and `US8` / `USL8LP` for 8-port
   gigabit devices.
4. **The controller requires encryption, and rejects downgrades.** The 10.6.109
   inform servlet contains `Plain text inform is not supported`,
   `Unencrypted payload is not allowed`, and `Device {} tried to downgrade
   inform encryption from AES-GCM to AES-CBC, rejecting inform`
   *(string evidence)*. AES-GCM only needs the forward AES cipher plus GHASH. On
   the 8051 that is the cheaper choice compared with CBC, which needs both
   directions.
5. **It fits both switches the maintainer runs, but code space and CPU are
   tight, not RAM or TCP.** The inform POST is several KB (4.7–6.2 KB of
   JSON for 6–10 ports in the lab). It needs no compression, and responses
   are never compressed (both **confirmed in the lab**). A provisioning
   response is 2.2–7.6 KB on the wire.
   - Memory: the RTL8373 and RTL8370N builds each have about 28 KB of free
     XDATA, and the UniFi client needs about 13 KB.
   - TCP: a second uIP connection for inform and a third for metrics cost
     30 B each plus their buffers.
   - Code: the RTL8373 build has 17 KB of free code in fragments, so the
     client needs a fifth code bank. The flash slot for it is empty on both
     images.
   - CPU: AES-128-GCM in C costs about 72,000 8051 cycles per 16 bytes
     (measured in a simulator), so the inform must be trimmed and the crypto
     run in small steps from the main loop. See
     [Feasibility](#feasibility-on-rtl837x--8051).
6. **Several of the gaps above are filled by work done for an OpenWrt switch.**
   A parallel investigation for an OpenWrt-based RTL930x switch (LG510S-4T2XS)
   found the projects listed in [Additional projects](#additional-projects):
   two OpenWrt packages that already get adopted by Network 10.6 as access
   points ([antoniocosta/unifi-openwrt](https://github.com/antoniocosta/unifi-openwrt),
   [yesrab/openUF](https://github.com/yesrab/openUF)), a switch emulator with
   per-model profiles and a full capability-bit dictionary
   ([jamesbraid/unifi-emu](https://github.com/jamesbraid/unifi-emu)), and a
   real USW-Pro-24 adoption capture in
   [jda/pixiedust](https://github.com/jda/pixiedust). It also extracted the
   complete device-side `switch.*` key set from Ubiquiti's own Realtek RTL930x
   switch firmware. Those findings are folded into the sections below and
   attributed to their sources. They were not re-checked for this note.

## Projects

| Name | URL | Language | Status / last activity | What it covers | Usefulness for switch emulation |
| --- | --- | --- | --- | --- | --- |
| unifi-simulator (MikroTik UniFi Switch Emulator) | https://github.com/MartinDejmal/unifi-simulator | Python | Active, created 2026-10-15, last push 2026-10-16 | Device-side inform with CBC/GCM and zlib. US8, US24PRO2 and UGW3 profiles. port_table, mac_table and LLDP from RouterOS. Lab validation on 10.6.106. Read-only by design: it never applies controller configuration. | **High.** The best current evidence of what a real controller accepts from a fake switch. See `docs/protocol-research.md` and `docs/validation.md`. |
| TNBU | https://github.com/JohnKiller/TNBU | C# (.NET) | Active, last push 2026-08-13 (`ba2c22f`) | Core library (discovery and inform codec), MitM proxy between real devices and a real controller, multi-model device simulator, and a lightweight controller that adopts APs and a USW-Pro-48-PoE. | **High.** The only public generator of switch `system_cfg`, plus the full `port_table` / `lldp_table` schema and the response types. |
| unifi-gateway (amd989 fork) | https://github.com/amd989/unifi-gateway | Python | Active, pushed 2026-11-20 | Emulates a UGW3 (USG) for OpenWrt, pfSense and others. CBC/GCM, zlib/snappy, discovery TLV, adoption via `set-adopt`. | **Medium.** Clear reference code for framing and adoption. Gateway, not switch. |
| unifi-gateway (qvr) | https://github.com/qvr/unifi-gateway | Python | Archived, last push 2023-10-02 | Python 3 port and AES-GCM support for the original. | Historical. |
| unifi-gateway (stephanlascar) | https://github.com/stephanlascar/unifi-gateway | Python | Archived, last push 2017-10-20 | Original USG emulator, including `poc/` captures and an example payload. | Historical. |
| unifi-stubd | https://github.com/konstruktor1/unifi-stubd | Go | Active, created 2026-05-16, pushed 2026-10-31 | Presents a Linux bridge as a US8. Discovery, inform, optional SSH shim for `set-adopt`. Detailed protocol notes. Says it passed the "visible product path" on 10.5.67. | **High** for protocol detail (`docs/en/protocol-notes.md`). Read-only, never applies config. |
| unifi-tlsg | https://github.com/benklop/unifi-tlsg | Python | Created 2026-07-14, pushed 2026-07-26, 0 stars | Inform proxy that presents a TP-Link TL-SG2424 as a `US24` and applies VLAN and port config over SSH CLI. | **Low to medium.** The only project that tries to *apply* config, but its README says "needs lab validation". Its system_cfg parser expects `port.N.*` keys, which disagrees with TNBU's and the controller's `switch.port.N.*`. Treat it as unvalidated. |
| UniFiction | https://github.com/ArmedGuy/unifiction | Go | Inactive, last push 2020-09-27 | Represents SNMP or Cisco switches as UniFi devices via L3 adoption. README: "We tell UniFi that we have the latest configuration, even if the device driver never even tried to apply it". | Low. Old, but the same idea. |
| unifi-inform-protocol | https://github.com/jeffreykog/unifi-inform-protocol (jk-5 redirects here) | Markdown | Last push 2020-05-18 | AP-era header, flags, discovery TLVs, SSH/L3 adoption narrative. | Reference only. It calls CBC "unpadded", which is contradicted by all code (PKCS#7). |
| unifi-protocol-reverse-engineering | https://github.com/fxkr/unifi-protocol-reverse-engineering | Markdown | Last push 2015-03-23 | Early AP notes. Example AP inform JSON. DHCP option 43 (sub-option 1, IP). mitmproxy setup. | Historical. |
| ubntmfi | https://github.com/mcrute/ubntmfi | Python and Markdown | Archived, last push 2017-08-16 | mFi inform. `inform_protocol.md` documents the response types (`noop`, `setparam`, `reboot`, `upgrade`, `cmd`) and fields. | Medium. A good description of response payloads. |
| Unofficial UniFi guide | https://jrjparks.github.io/unofficial-unifi-guide/ | Docs (mdBook) | Unknown date | Inform header and flags, GCM AAD and tag layout, discovery TLV IDs, response `_type` list. The system_cfg and mgmt_cfg sections are TODO. | Medium. |
| nanofi / pixiedust | https://github.com/jda/nanofi, https://github.com/jda/pixiedust | Go | nanofi pushed 2026-08-12 | Inform codec library and traffic tools. Notes that the controller returns 404 until a device is adopted. | Low to medium. |
| unifi-inform (ZAP-Quebec) | https://github.com/ZAP-Quebec/unifi-inform | Go | Last push 2017 | Inform codec. | Low. |
| inform-inspect | https://github.com/dmke/inform-inspect | Go | Pushed 2026-11-18 | Inform packet decoder and inspector. | Low. Debugging aid. |
| unispi | https://github.com/foxt/unispi | TypeScript | 2024-11-20, one commit | Notes and code on inform. | Low. |
| Wireshark UBDP dissector | https://gitlab.com/wireshark/wireshark/-/raw/master/epan/dissectors/packet-ubdp.c | C | Upstream Wireshark | Dissects UDP 10001 discovery: TLV IDs 1–28 (lines 15–44) and registration as "Ubiquiti Discovery Protocol" (line 395). | Useful for debugging discovery. **No inform (TCP 8080) dissector was found.** |
| nmap `ubiquiti-discovery` | https://nmap.org/nsedoc/scripts/ubiquiti-discovery.html | Lua (NSE) | Upstream nmap | UDP 10001 probe. Tries v1, then v2. | Useful for checking discovery replies. |
| ubnt-discovery tools | e.g. https://github.com/nitefood/python-ubnt-discovery | Various | Various | Desktop discovery clients. | Low. |
| aiounifi, unpoller, etc. | e.g. https://github.com/Kane610/aiounifi | Python, Go | Active | Controller *client* APIs. They show the controller's view of `port_table`, but not the device protocol. | Only for checking what the controller stored. |

No project emulating a switch on a microcontroller was found. Ubiquiti's own
low-end switches do run the protocol on microcontrollers, though. The uidb
marks USW-Flex-Mini, USW-Flex-2.5G-5/8 and USW-Ultra as `MCU_BASED` (see the
model impersonation section). The Flex Mini runs a Nuvoton Cortex-M4 next to an
RTL8367RB ([kloenk.eu teardown, 2026-03-04](https://kloenk.eu/posts/unifree/usw-flex-mini/)).
The USW-Flex-2.5G-5 uses a Realtek RTL8372N plus an RTL8221B
([ServeTheHome review, 2025-11-14](https://www.servethehome.com/ubiquiti-flex-mini-2-5g-review-ubiquiti-does-a-cheap-5-port-2-5gbe-switch/)).
That is the same switch silicon RTLPlayground targets. Reader comments there
report an ESP32 as the management CPU, which is **unverified**.

### Additional projects

Found by the OpenWrt investigation. Last activity is the GitHub push date on
2026-11-20.

| Name | URL | Language, licence | Last activity | What it covers | Usefulness here |
| --- | --- | --- | --- | --- | --- |
| unifi-openwrt | https://github.com/antoniocosta/unifi-openwrt | Go, GPL-2.0 | 2026-11-20 | OpenWrt package adopted as an AP (default `U7PG2`) on Network 10.6. Discovery, SSH and L3 adoption, full provisioning, a `system_cfg` to UCI translator with rollback, an "unhandled keys" ledger, and refusal to apply Ubiquiti firmware. | **High** for the structure of client and translator (`docs/internals.md`, `internal/inform`, `internal/translate`). |
| openUF | https://github.com/yesrab/openUF (fork of https://github.com/jonasevcik/openUF, MIT) | Lua, GPL-3.0 | 2026-11-18 | OpenWrt AP agent validated on a UCG Ultra running Network 10.6. `PROTOCOL-VALIDATION.md` is the most thorough live-controller log found: the AES-GCM provisioning gate, both adoption paths, the per-port VLAN `switch.*` keys and the `port_table` contract. | **High.** Read `PROTOCOL-VALIDATION.md` before writing code. |
| unifi-emu | https://github.com/jamesbraid/unifi-emu | Go, MIT | 2026-11-19 | Software emulation of APs, **switches** and gateways for controller tests. Per-model profiles (including `US6XG150`, `USL8A`, `USWED76`), and `capability_bits.json` with every capability bitmap the controller tests. | **High.** It answers the capability-bit question, and is the quickest way to watch a candidate model in a real controller. |
| open-unifi | https://github.com/lucavb/open-unifi | Go, MIT | 2026-11-19 | Alternative controller that adopts a real `U7PG2`. Protocol docs cite controller bytecode, including UDP 10001 discovery (`docs/PROTOCOL-discovery.md`). | Medium. |
| pixiedust capture | https://github.com/jda/pixiedust/blob/a4356227df/unifi-fresh-usw.pcap | pcap | 2026-08-12 | A real USW-Pro-24 (`US24PRO2`, firmware 4.3.22) adoption, with the switch's full inform payload and the controller's `system_cfg`. pixiedust can recover the authkey and decrypt it. | **High.** It is the only real switch capture found. |
| USW-Aggregation firmware 7.5.15 | `https://fw-download.ubnt.com/data/unifi-firmware/127c-USL8A-7.5.15-dc23f38c-6be7-470e-ac36-fe6ec832275c.bin` | Ubiquiti binary | — | `USL8A`, platform `US.rtl930x`. Plaintext initramfs with `mcad` (the inform agent), `syswrapper.sh`, `mca-dump`, and in `ubntbox` the full `switch.*` key set. LEDE 17.01 based. | **High** for interoperability facts (key names). Do not copy its code. |

## Protocol reference

### Transport and ports

- Devices send `POST /inform` with `Content-Type: application/x-binary` to the
  inform URL. The default is `http://unifi:8080/inform`
  ([unofficial guide, inform page](https://jrjparks.github.io/unofficial-unifi-guide/protocols/inform.html)).
  amd989 and unifi-stubd send `User-Agent: AirControl Agent v1.0`
  (amd989 `unifi_gateway.py:391-408`; unifi-stubd `docs/en/protocol-notes.md:54-60`).
- Controller defaults are in `UniFi/lib/ace.jar!/system.properties` in 10.6.109:
  `unifi.http.port=8080` (commented as "device inform"),
  `unifi.stun.port=3478`, and `system_ip` ("the IP devices should be talking to
  for inform").
- The controller's discovery listener joins multicast `233.89.188.1`, and logs
  `unable to bind udp:{} socket and L2 discovery will not work`
  (`com/a/a/a/QdjEeOBh.class` in `internal-dependencies.jar`,
  *string evidence*). The UDP port is 10001 per the
  [Wireshark dissector](https://gitlab.com/wireshark/wireshark/-/raw/master/epan/dissectors/packet-ubdp.c),
  [nmap](https://nmap.org/nsedoc/scripts/ubiquiti-discovery.html) and the
  emulators.
- The device repeats inform every ~10 s by default. After any command
  response it informs again immediately (mcrute `inform_protocol.md:14-19`).
  `noop` carries the next `interval`.

### Packet header

All integers are big-endian. Sources: TNBU `TNBU.Core/Models/InformPacket.cs:15-169`,
amd989 `unifi_protocol.py:30-124`, unifi-stubd `docs/en/protocol-notes.md:74-90`.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | magic `TNBU` (0x544E4255 = 1414414933) | The controller logs `Bad packet magic` *(string evidence)*. |
| 4 | 4 | packet version | TNBU simulator and stubd send 0. amd989 sends 1. unifi-simulator found both work on 10.6.106 (`docs/protocol-research.md:32,144`). |
| 8 | 6 | device MAC | Unencrypted. The controller routes by it and keys the lookup on it. |
| 14 | 2 | flags | `0x01` encrypted, `0x02` zlib, `0x04` snappy, `0x08` AES-GCM. The controller rejects `Both zlib and snappy compression flags set` *(string evidence)*. |
| 16 | 16 | IV / nonce | Random per packet. |
| 32 | 4 | payload (data) version | 1 = JSON. Version 0 was key=value (mcrute `inform_protocol.md:47-49`). The controller has `Data version \u0001 is not supported` *(string evidence)*. |
| 36 | 4 | payload length | For GCM this *includes* the 16-byte tag (TNBU `InformPacket.cs:138-139`; amd989 `unifi_protocol.py:62-63`). |
| 40 | n | payload | Compressed first, then encrypted. Reverse the order to read. |

### Encryption and keys

- Key: AES-128, given as 32 hex characters. Before adoption it is the public
  default key `ba86f2bbe107c7c57eb5f2690775c712` = MD5("ubnt"). That literal
  appears in the 10.6.109 `com/ubnt/net/InformServlet.class`, in TNBU
  `InformPacket.cs:16` and in amd989 `unifi_protocol.py:27`. After adoption it
  is the `authkey` from `mgmt_cfg`. The controller then refuses the default key
  (`dev[{}] used default key in {} state, reject it!`, `devmgr/l/mMObEXELV.class`,
  *string evidence*).
- **CBC**: AES-128-CBC with PKCS#7 padding (TNBU `InformPacket.cs:73-81,146-150`;
  amd989 `unifi_protocol.py:68-72,108-112`). Some older notes say "unpadded",
  which is wrong.
- **GCM**: flags `0x01|0x08`. The nonce is the full **16-byte** IV, which is
  non-standard length, so J0 is derived via GHASH. The 40-byte header, with the
  final length field, is the AAD. A 16-byte tag is appended (TNBU
  `InformPacket.cs:64-70,137-144`; amd989 `unifi_protocol.py:62-67,101-106`;
  unifi-stubd `protocol-notes.md:114-119`).
- **Negotiation**: the controller puts `use_aes_gcm=true` in `mgmt_cfg`
  (TNBU `ManagementConfig.cs:17`). Devices switch to GCM from the next request
  (amd989 `unifi_gateway.py:381-384`). Once a device has used GCM, CBC is
  refused (`InformServlet` *string evidence*, quoted above). The adoption reply
  is encrypted in the mode the request used (TNBU controller:
  `inform_resp.IsGCM = req.IsGCM`, `TNBU.GUI/Services/DeviceManagerService.cs:214`).
- **GCM is required for provisioning.** openUF found that Network 10.4.57 does
  not provision a device until it has received a genuinely GCM-encrypted
  inform. The `x_aes_gcm` flag is set only from the wire flag. A device stuck
  on CBC shows a new `cfgversion` on every cycle and stays "Adopting"
  ([openUF `PROTOCOL-VALIDATION.md`](https://github.com/yesrab/openUF/blob/3da571983b/PROTOCOL-VALIDATION.md)
  lines 529–565). CBC is therefore only needed, if at all, for the first
  inform with the default key. GCM-only should be tested first.
  **Confirmed in the lab:** a device that sends AES-GCM from its very first
  inform (default key, flags `0x09` or `0x0b`) is adopted and provisioned
  normally. The controller answers each inform in the mode it was sent in (a
  CBC adoption inform gets a `0x01` reply, a GCM one gets `0x09`), so a
  GCM-only device never needs the AES inverse cipher. Staying on CBC after
  `use_aes_gcm=true` was not tested.
- Unencrypted informs are rejected by 10.6.109 (`Plain text inform is not
  supported`, `Unencrypted payload is not allowed`, *string evidence*). This
  matches the older fxkr and jrjparks notes.

### Compression

- Device to controller: zlib (`0x02`) is what everyone sends. amd989 prefers
  snappy when the library is installed (`unifi_protocol.py:48-53`). The
  controller bundles `snappy-java-1.1.10.8.jar`. 10.6.109 also has `decompress
  failed: uncompressed size exceeds limit for default authkey`
  *(string evidence)*, so payloads sent under the default key have a size cap of
  unknown value.
- An *uncompressed* encrypted inform (flags `0x01` or `0x09` only) is
  accepted by 10.6.109, before and after adoption (**confirmed in the lab**,
  CBC and GCM). A zlib-compressed pre-adoption inform that inflates to
  5.5 KB was also accepted under the default key, so the "limit for default
  authkey" is above that. Uncompressed pre-adoption informs were only tested
  at about 0.4 KB. The fallback, should a
  later build refuse uncompressed informs, is a zlib stream made of deflate
  *stored* blocks (RFC 1950/1951:
  `78 01`, then `[BFINAL|BTYPE=00] LEN NLEN data`, then Adler-32). It is valid
  zlib that any `java.util.zip.Inflater` accepts, and it needs no compressor.
- Controller to device: TNBU's alternative controller sends `IsZLIB=false,
  IsSnappy=false` (`DeviceManagerService.cs:197-201`). Real APs and the
  USW-Pro-48-PoE accept that. unifi-stubd's client goes further and *rejects*
  compressed responses while working against 10.5.67
  (`protocol-notes.md:121-130`). That suggests stock controller responses are
  uncompressed. **Confirmed in the lab:** all 474 controller responses
  captured from 10.6.109 had flags `0x01` (CBC) or `0x09` (GCM), never zlib
  or snappy, including every `system_cfg`.

### Adoption flow (layer 3, no SSH)

1. The device informs with the default key, `default: true`, and
   `state` 0 or 1. Sources disagree here. amd989 sends a minimal JSON (0 when
   pending, 2 when adopted) at `unifi_protocol.py:139-155`. unifi-stubd uses 1
   = Default, 0 = Unknown, 2 = Managed (`protocol-notes.md:168-193`). The TNBU
   simulator sends 4 (`DeviceSimulator.cs:52`). The controller answers **HTTP
   404** and lists the device as pending (nanofi README; amd989
   `unifi_gateway.py:326-332`).
2. The admin clicks Adopt. The next inform gets `_type: setparam` with a
   `mgmt_cfg` string (newline-separated `key=value`) that contains `authkey=…`,
   `cfgversion=…`, `use_aes_gcm=true`, `inform_url`, `mgmt_url`, `stun_url`,
   `led_enabled`, `capability=notif,fastapply-bg,notif-assoc-stat`,
   `report_crash`, and `selfrun_guest_mode` (TNBU `ManagementConfig.cs:10-40`,
   `InformResponse.cs:4-13`; amd989 `unifi_gateway.py:363-389`). The reply is
   still encrypted with the *default* key. Later traffic uses the new key.
   In the lab (10.6.109) the adoption `mgmt_cfg` had exactly `capability`,
   `selfrun_guest_mode=pass`, `cfgversion`, `led_enabled=true`, `stun_url`,
   `mgmt_url`, `authkey`, `use_aes_gcm=true` and `report_crash=true`. It had
   **no `inform_url`**, so the device keeps the URL it already uses. The
   emulator sent `state` 1 while pending and 4 once adopted, and the
   controller accepted both.
3. The device informs with the new key and echoes the `cfgversion`. The
   controller sends `setparam` with `system_cfg` (the full device config) and
   `mgmt_cfg`. The device applies it and reports the new `cfgversion`. After
   that the controller sends `noop` with an `interval`. Whenever the device's
   `cfgversion` differs from the controller's, the controller resends
   `setparam` (TNBU controller logic, `DeviceManagerService.cs:216-246`).
   **Confirmed in the lab**, including the loop: a device that never echoes
   the `cfgversion` gets the full `setparam` again on every inform, never a
   `noop`, and `stat/device` shows it in state 5 (provisioning) with
   `known_cfgversion` `"0"`. The `mgmt_cfg` in this second `setparam` no
   longer carries `authkey`.
   10.6.109 strings confirm the same states (`Device adoption - initial
   mgmt_cfg sent for device[{}]`, `Device[{}] adoption - completed`,
   `wait_for_initial_inform`, *string evidence*).
4. **The `inform_url` in the JSON must use an IP address.** unifi-simulator
   found that 10.6.106 failed adoption with HTTP 400 and logged `invalid
   inform_ip` when the JSON `inform_url` used a hostname. Sending the resolved
   IPv4 address fixed it (`docs/protocol-research.md:139-157`). 10.6.109 has
   `dev[{}] invalid inform_ip {}` and `[{}] Skipped invalid inform_ip update`
   *(string evidence)*. unifi-emu documents the same failure as `invalid
   inform_ip` ([`docs/PROTOCOL.md`](https://github.com/jamesbraid/unifi-emu/blob/a538889cce/docs/PROTOCOL.md),
   "Three things that trip up an implementer").
5. Accept a key from `mgmt_cfg` only while still on the factory key. Some
   builds deliver the key as `{"_type":"cmd","cmd":"set-adopt","key":…,"uri":…}`
   instead of `setparam` (unifi-emu `docs/PROTOCOL.md`, "The two
   key-delivery channels"). `mgmt_url` is the controller's web UI link, not an
   inform URL. Treating it as one loses the controller (openUF
   `PROTOCOL-VALIDATION.md` lines 600–604).
6. Ways for the device to learn the inform URL: manual configuration (the
   web UI or CLI here), DHCP option 43 sub-option 1 carrying the IPv4 address
   (fxkr README), and the DNS name `unifi` (the default URL). Ubiquiti's help
   articles on adoption and ports
   (https://help.ui.com/hc/en-us/articles/204909754,
   https://help.ui.com/hc/en-us/articles/218506997) returned HTTP 403 to
   automated fetching, so the official wording is **not quoted here**.

### Layer-2 discovery and SSH adoption (avoid)

- Discovery packets are UDP 10001, sent to the broadcast address and to
  multicast 233.89.188.1. Format: `u8 version, u8 command, u16 length`, then
  TLVs `u8 type, u16 len, value`. Version 2 with command 6 is the periodic
  announcement (amd989 `unifi_protocol.py:270-291`, `tlv.py:18-24`; TNBU
  `DiscoveryPacket.cs:12-30`). TLV IDs come from the Wireshark source: 1 MAC,
  2 MAC+IP, 3 full firmware, 10 uptime, 11 hostname, 12 product, 16 system ID,
  18 sequence, 19 MAC, 21 model, 22 firmware, 23 default, 27 platform version,
  28 SSH port. TLV 16 "System ID" is a u16 and appears to be the uidb `sysid`
  (for example `ed36`). That match is an inference.
- A device found by L2 discovery is adopted by the controller over **SSH**:
  `/usr/bin/syswrapper.sh set-adopt <inform_url> <authkey>` (10.6.109
  `com/ubnt/service/devmgr/jRYGVjDqQ$QdjEeOBh.class`, *string evidence*; also
  jeffreykog README and unifi-stubd `protocol-notes.md:220-227`).
  unifi-simulator saw 10.6.106 log that L3 inform discovery **skips SSH
  adoption** (`docs/protocol-research.md:143-144`). An SSH server is not
  realistic on the 8051, so use L3 inform only. Discovery broadcasts can still
  be sent so that the device shows up in the controller and in the
  ubnt-discovery tools. MCU-based USWs such as the Flex Mini have no SSH (see
  [HostiFi](https://www.hostifi.com/support/unifi-how-to-adopt-the-usw-ultra)),
  so some non-SSH L2 path exists. It was not identified.

### Response types

Sources: TNBU `InformResponse.cs:1-124`, mcrute `inform_protocol.md:55-129`,
amd989 `unifi_gateway.py:189-251`. Every response has `_type` and
`server_time_in_utc`.

| `_type` | Fields | Suggested handling here |
| --- | --- | --- |
| `noop` | `interval` (seconds), optional `immediate: 1`, `live_update: true` | Schedule the next inform. `immediate` means inform now. |
| `setparam` | `mgmt_cfg` (string), `system_cfg` (string), `cfgversion`, sometimes `blocked_sta`, `port_cfg`, `analog_cfg` | Apply `mgmt_cfg` (key, URLs, GCM). Apply the `switch.*` subset of `system_cfg`. Echo `cfgversion`. |
| `upgrade` | `url`, `md5sum`, `version`, `datetime`, `device_id` | Ignore. Do not change the reported version. See the firmware notes in the model section. |
| `reboot` | optional `reboot_type: hard` | Possibly honour it. It is harmless. |
| `cmd` | `cmd`: `set-locate`, `unset-locate`, `power-cycle` (with `port`), `spectrum-scan`, mesh commands | Locate means blink the LEDs. `power-cycle` is a PoE port bounce. Ignore the rest. |
| `setdefault` | none | The operator clicked Remove. Forget the authkey and go back to the pending state. Do not factory-reset the switch (unifi-openwrt and openUF both keep the local config). |

The controller can only answer the device's next inform, so the device must
keep informing. After a `cmd`, inform again immediately (openUF
`PROTOCOL-VALIDATION.md` lines 606–638). unifi-emu also describes a
`setstate` reply carrying `port_table` / `port_overrides`, which openUF's
reading of `InformServlet` (10.4.57) does not list. Which is right for a USW
on 10.6 is **unverified**. In the lab, 10.6.109 sent only `setparam` and
`noop` to emulated switches (479 responses), including after every port
override change, so port overrides arrive in `system_cfg`. For `upgrade`, unifi-openwrt and openUF report the
offered version afterwards so that the controller stops offering it nightly.
That differs from the "keep reporting the same version" approach below. Test
both.

### Inform JSON (device to controller)

The base fields come from TNBU `BaseInformBody` (`BaseInformBody.cs:3-49`):
`mac, ip, netmask, hostname, model, model_display, version, required_version,
serial, inform_url, default, state, cfgversion, uptime, time, uplink,
isolated, locating, bootrom_version, board_rev, architecture, anon_id,
hash_id, manufacturer_id, selfrun_beacon, inform_min_interval, inform_as_notif,
notif_reason, discovery_response`. The controller rejects an inform without
`version` (`invalid inform: 'version' not found in payload`) and checks
`hash_id` (`Invalid hash_id [{}] from device[{}] inform, ignored`), both
*string evidence*.

Switch tables (TNBU `BaseInformBody.cs:51-59,234-242,265-334,398-402`):

- `port_table[]`: `port_idx` (1-based), `up`, `enable`, `speed` (Mb/s),
  `full_duplex`, `autoneg`, `media` (for example `GE`, `SFP+`), `is_uplink`,
  `rx_bytes`, `tx_bytes`, `rx_packets`, `tx_packets`, rx/tx
  errors/dropped/broadcast/multicast, `stp_state`, `stp_pathcost`,
  `speed_caps`, `flowctrl_rx/tx`, `jumbo`, `lag_member`, `aggregate_num_ports`,
  `lacp_state[]`, `poe_caps`, `port_poe`, `poe_enable`, `poe_mode`,
  `poe_power`, `poe_voltage`, `poe_current`, `poe_class`, `poe_good`,
  `sfp_found`, `sfp_vendor`, `sfp_part`, `sfp_serial`, `sfp_temperature`,
  `sfp_txpower`, `sfp_rxpower`, `dot1x_*`, `satisfaction`, `anomalies`, and
  `mac_table[]` (`mac, vlan, age, static, uptime, ip, hostname`).
  The 10.6.109 port-table class (`com/ubnt/service/v/a/vFiJoQiuQjMZUx.class`)
  references this subset: `autoneg, enable, flowctrl_rx/tx, full_duplex,
  is_uplink, jumbo, mac_table, media, poe_*, port_idx, port_poe, rx/tx_bytes,
  rx/tx_packets, satisfaction, speed, speed_caps, stp_pathcost, stp_state, up`
  *(string evidence)*.
- `lldp_table[]`: `chassis_id`, `chassis_descr`, `local_port_idx`,
  `local_port_name`, `port_id`, `port_descr`, `is_wired`. unifi-simulator
  reports these were accepted by 10.6.106 (`docs/protocol-research.md:161-175`).
- `if_table[]` for the management interface, `uplink`, `last_uplink`,
  `num_port`, `ethernet_table` (unifi-stubd `protocol-notes.md:229-260`).
  unifi-stubd says 10.5's live statistics use the uplink `rx_rate`/`tx_rate`
  in bit/s.
- Capability bitmasks: the controller reads `fw_caps`, `hw_caps`,
  `switch_caps`, `vlan_caps` and others (`devmgr/nHhxEfQQGSLumjGc.class`,
  `devmgr/TocwLJhQU.class`, *string evidence*). amd989 sends `fw_caps: 3` for
  a USG. The bit meanings are in unifi-emu's
  [`capability_bits.json`](https://github.com/jamesbraid/unifi-emu/blob/a538889cce/capability_bits.json),
  read from controller 10.4.57:
  - `port_table[].speed_caps`: 10H `1`, 10F `2`, 100H `4`, 100F `8`, 1000H
    `16`, 1000F `32`, 2.5G `64`, 5G `128`, 10G `256`, autoneg `1048576`, FEC
    `2097152`. The real USW-Pro-24 sends `1048623` on copper and `1048928` on
    SFP+ (pixiedust capture). For an RTLPlayground 2.5G copper port this
    gives `1048682` (autoneg, 10F, 100F, 1000F, 2.5G), and `288` (1G and 10G)
    for SFP+. Both were accepted by 10.6.109 in the lab (emulated), stored as
    sent, and used to validate forced speeds.
  - `switch_caps.feature_caps`: L3 routing `1`, 802.1X `2`, port isolation
    `4`, storm control `8`, LLDP-MED `16`, egress rate limit `32`, STP `64`,
    DHCP snooping `128`, IGMP snooping `256`, port security `512`, LACP `8192`,
    FEC `65536`, bridge VLAN filter `262144`, jumbo `524288`, keepalive loop
    protection `1048576`. Higher bits cover EtherLighting, MC-LAG, BGP,
    stacking, VRRP, OOB port and PTP. The USW-Pro-24 capture sends
    `{"feature_caps":1023,"max_aggregate_sessions":6,"max_l3_intf":64,"max_mirror_sessions":1,"max_reserved_routes":62,"max_static_routes":2}`.
  - `switch_caps.stp_caps`: BPDU guard `1`, auto edge `2`, port cost `8`.
    `port_table[].port_caps`: EEE `1`.

  **Claim only what the firmware can do.** A claimed bit makes the controller
  show the control and send its configuration. A claimed but unimplemented
  feature fails silently (openUF
  [`REVERSE-ENGINEERING.md`](https://github.com/yesrab/openUF/blob/3da571983b/REVERSE-ENGINEERING.md),
  "Aim and ground rules"; unifi-emu `docs/PROTOCOL.md`, "Capability
  bitmaps"). RTLPlayground already has code for STP (`64`), storm control
  (`8`), DHCP snooping (`128`), IGMP snooping (`256`), LACP (`8192`), egress
  rate limit (`32`, `rtl837x_bandwidth.c`) and mirroring. Each bit should be
  claimed only after its `system_cfg` keys are applied and tested.
- Other top-level fields in the real USW-Pro-24 inform (pixiedust capture):
  `fw_caps`, `hw_caps`, `general_temperature`, `has_fan`, `has_temperature`,
  `overheating`, `gateway_ip`, `gateway_mac`, `kernel_version`,
  `stp_priority`, `sys_stats` (`loadavg_1/5/15`, `mem_total`, `mem_used`,
  `mem_buffer`), `system-stats` (`cpu`, `mem`, `uptime` as strings),
  `satisfaction` and `total_max_power`. Its `port_table` entries nest
  `lldp_table` (`lldp_chassis_id`, `lldp_port_id`, `lldp_system_name`) per
  port. That differs from the top-level `lldp_table` described next, so
  capture both forms before choosing one.
- unifi-simulator found the controller "relatively forgiving". A minimal
  `port_table` with `port_idx/up/enable/speed/rx_bytes/tx_bytes` was enough for
  Connected and for port rates (`docs/protocol-research.md:90-101,161-175`).
  unifi-tlsg and UniFiction say the same.

## Switch-specific configuration (system_cfg)

`system_cfg` is a single string of newline-separated `key=value` lines, grouped
by `# section` comments (TNBU `SystemConfig.cs:35-99`). The only public generator
of the switch section is TNBU's own controller. Its author built it with the
MitM tool against a real controller and USW-Pro-48-PoE
(`TNBU.Core/Models/DeviceConfiguration/CfgSwitch.cs`, `CfgSwitchEntry.cs`,
`CfgSwitchVLAN.cs`):

```text
switch.dhcp_snoop.status=enabled
switch.jumboframes=disabled
switch.managementvlan=1
switch.mtu=9216
switch.stp.priority=32768
switch.stp.status=enabled
switch.stp.version=rstp
switch.port.N.name=Port N
switch.port.N.opmode=switch            # "aggregate" for LAG members, plus switch.port.N.lag=<id>
switch.port.N.poe=auto                 # or "shutdown"
switch.port.N.stp.port_mode=enabled
switch.port.N.lldpmed.opmode=enabled
switch.port.N.lldpmed.topology_notify=disabled
switch.port.N.keepalive=disabled
switch.port.N.port-security=disabled
switch.port.N.status=disabled          # only present when the port is disabled
switch.port.N.pvid=<vid>               # native/untagged VLAN
switch.vlan.status=enabled
switch.vlan.K.id=<vid>
switch.vlan.K.status=enabled
switch.vlan.K.mode=tagged              # "untagged" for VLAN 1 in TNBU
switch.vlan.K.port.N.mode=untagged     # or "tagged" / "exclude"
```

The 10.6.109 switch config builder (`com/ubnt/service/config/jocpX.class`,
*string evidence*) confirms this vocabulary and adds more:
`switch.port.\u0001` and `switch.vlan.\u0001` prefixes, `opmode`, `pvid`,
`speed`, `autoneg`, `duplex`/`full_duplex`, `flowctrl`, `eee`, `fec`,
`isolation`, `lag`/`lag_idx`, `mclag`, `stp.cost`, `stp.edge`,
`stp.bpdu_guard`, `stp.port_mode`, `lldpmed.opmode`, `lldpmed.topology_notify`,
`keepalive`, `port-security`, `switch.port.\u0001.stormctrl` (with
ucast/mcast/bcast level and rate), `switch.port.\u0001.pq.\u0001`, `voice_vlan`,
`dot1x.mode`, `dot1x.idle_timeout`, `mirror_port`, `managementvlan`,
`jumboframes`, `mtu`, `tagged`, `untagged`, `exclude`, `switch.dhcp_guard.`,
`switch.led`, `switch.poe`, `switch.qos.\u0001`, and `switch.syslog.remote.port`.
`\u0001` is a Java string-concatenation placeholder for an index. Another
config class (`ubntconf/NDxSQTDwni.class`) adds `switch.port.\u0001.trusted_port`.

The device side agrees. `bin/ubntbox` in Ubiquiti's USW-Aggregation 7.5.15
firmware (`USL8A`, `US.rtl930x`, header `UBNTUS.rtl930x.v7.5.15+17146`) parses
these per-port keys: `status`, `name`, `opmode`, `speed`, `duplex`,
`autoneg`, `flowctrl`, `eee`, `fec`, `mtu`, `pvid`, `ingressfilter`,
`isolation`, `lag`, `mirror_port`, `egress_rate_limit_kbps`,
`stormctrl.{status,type,bcast,mcast,ucast}`,
`stp.{port_mode,cost,edge,bpdu_guard,root_guard,loopback_detection}`,
`lldp.opmode`, `lldpmed.*`, `dot1x.*`, `port-security*`, `keepalive`,
`link_debounce`, `qos`, `dscp`, `acl.*`, `poe` and `mclag`. Global keys:
`switch.lag.%d.lacp`, `switch.igmp_snooping.*`, `switch.multicast.*`,
`switch.lldp.*`, `switch.qos.*`, `switch.acl.*`, `switch.dhcp_guard.*`,
`switch.radius.*`, `switch.routing_interface.*`, `switch.ptp.*`,
`switch.mclag.*`, `switch.oob.*` and `switch.etherlight.*`. The firmware
parses `switch.igmp_snooping.*`, but the 10.6.109 controller sent IGMP
snooping as per-VLAN `switch.vlan.K.igmp_*` keys in the lab. Value
vocabularies beyond those seen in the lab are **unverified**. This is the firmware for the same Realtek switch family
(RTL930x), not for RTL837x.

On VLAN membership, openUF diffed five controller states. It found that
`switch.vlan.K` is a slot whose `.id` is the VID. The per-VLAN, per-port
matrix (`switch.vlan.K.port.N.mode`) is authoritative, and an absent per-port
key means "default" (openUF `PROTOCOL-VALIDATION.md` lines 932–1006).
**Confirmed in the lab for a USW:** the slot's own `switch.vlan.K.mode` is
the default for every port without an explicit line. With all ports on the
"Allow all" default no per-port lines are sent at all. The first override on
a port emits a line for that port in every VLAN slot.

Mapping onto RTLPlayground:

| UniFi intent (port profile, port override, network) | system_cfg key | RTLPlayground feature |
| --- | --- | --- |
| Native VLAN / untagged network | `switch.port.N.pvid`, `switch.vlan.K.port.N.mode=untagged` | `vlan.c` PVID and untagged membership |
| Tagged networks ("Allow all" / custom) | `switch.vlan.K.port.N.mode=tagged` (`exclude` for blocked VLANs) | `vlan.c` tagged membership |
| Port disable | `switch.port.N.status=disabled` | port enable or disable |
| Speed / duplex / autoneg | `switch.port.N.autoneg=disabled`, `duplex=enabled` (full) or `disabled` (half), `speed=<Mb/s>`, `ld_mode=disabled` (lab) | `rtl837x_port.c` / PHY |
| Link aggregation | `opmode=aggregate`, `lag=<n>` on each member, no global `switch.lag.*` key (lab) | LACP / `link_aggregation.md` |
| STP | `switch.stp.{status,version,priority}`, `switch.port.N.stp.{port_mode,edge,bpdu_guard}` | `rtl837x_stp.c` |
| Storm control | `switch.port.N.stormctrl.{status,type,bcast,mcast,ucast}` | `rtl837x_storm.c` |
| Port mirroring | `opmode=mirror`, `mirror_port=<source>` on the destination port (lab) | mirroring |
| Port isolation | `switch.port.N.isolation=enabled` | not checked here |
| Egress rate limit | `switch.port.N.egress_rate_limit_kbps=<n>` | `rtl837x_bandwidth.c` |
| PoE mode | `switch.port.N.poe=auto/shutdown/passive24v` | `poe.h` on PoE boards |
| Management VLAN, MTU, jumbo frames | `switch.managementvlan`, `switch.mtu` (always 9216), `switch.jumboframes` | existing settings |
| IGMP snooping, DHCP snooping/guard | `switch.vlan.K.igmp_*`, `switch.multicast.*`, `switch.dhcp_snoop.status` (lab) | `rtl837x_igmp.c`, `rtl837x_dhcp_snoop.c` |

All key spellings above were observed in the lab. The full list, with the
API change that produced each one, is in
[Lab results](#lab-results-unifi-network-106109-emulated).

Unsupported settings: the protocol has no per-setting rejection. The device
acknowledges a whole `system_cfg` by echoing its `cfgversion`. Not echoing it
makes the controller re-provision on every inform and leave the device in a
provisioning loop (unifi-openwrt `docs/internals.md:73`; openUF
`PROTOCOL-VALIDATION.md` lines 529–565). So the device should always echo the
`cfgversion`, apply what it can, and clamp the rest. For example, a forced
5G or 10G speed on a 2.5G copper port becomes the fastest supported setting.
The device then reports what the hardware actually does: the real `speed` in
`port_table`, `speed_caps` limited to real modes, and `poe_caps=0`. Skipped
or clamped keys go to a local "unhandled" log, as unifi-openwrt does
(`docs/internals.md:129-135`). Whether the controller flags a mismatch
between a configured and a reported speed is **unverified**. Clamping is
rarely needed, though: the controller API refuses a forced speed or duplex
that the port's reported `speed_caps` does not include
(`api.err.InvalidInterfaceSpeedDuplex`, **confirmed in the lab**). With
honest `speed_caps` no 5G/10G setting can reach a 2.5G copper port.

Things not established:

- *(Settled in the lab for emulated devices, see
  [Lab results](#lab-results-unifi-network-106109-emulated).)*
  A real `system_cfg` captured from a stock controller for one of the
  candidate models. The pixiedust capture has one for a `US24PRO2` (Broadcom),
  and TNBU's output is a re-creation. The exact value spellings (`speed=2500`?
  `autoneg=true`?) and the size of a real file are unknown. TNBU's simulator
  saves whatever the controller sends to `system_cfg_<model>.txt`
  (`TNBU.Simulator/DeviceSimulator.cs:121-123`). Running it, or
  unifi-simulator, against a lab controller with the chosen model is the
  cheapest way to get ground truth. A typical file is **estimated** at a few KB
  for 6–10 ports. The TNBU switch section alone is about 350 bytes per port
  plus about 35 bytes per VLAN×port line, and the real file also has users,
  syslog, NTP and SSH sections.
- VLAN numbering. The controller writes 802.1Q VIDs, not network IDs, in TNBU's
  format. unifi-tlsg warns about network IDs in other fields.

## Model impersonation

### Where the controller gets the port layout

- `UniFi/dl/uidb/uidb.json` in the self-hosted 10.6.109 package
  (`version ff4e56c5…`, 698 devices). UniFi OS consoles read
  `/data/unifi-core/uidb.json` (path string in `com/ubnt/service/br/JLudBrawuIVqu.class`).
  Each USW entry has `unifi.network.model` (the inform `model` code),
  `numberOfPorts`, `ports` (`standard`, `sfp`, `plus`, `sfp28`, either as a
  count, a list or a range), an optional `diagram` (front-panel order) and
  `meta.combo`, `features.poe`, `deviceCapabilities` (for example
  `MCU_BASED`, `HAS_TEMPERATURE_SENSOR`), `knownUnsupportedFeatures` (for
  example `SWITCH_CAP_STP`), `minimumFirmwareRequired`, and `sysid`.
- `UniFi/dl/firmware/bundles.json` maps model codes to display names and
  firmware families (for example `US8` uses the `US24P250` base). The newest
  models (`USWED*`) are missing from it.
- The Java code also hardcodes model lists. The port-table class lists about
  100 USW codes, and `com/ubnt/i/g/a/XCfthFC.class` logs `Missing device mode
  [{}] features for model: {}`. The device manager logs `unable to manage
  dev[{}] with model[{}]`. All of this is *string evidence*. **Conclusion
  (inferred, not tested):** an invented model code would be rejected, and
  editing `uidb.json` alone is unlikely to be enough. Any such edit would also be
  overwritten on upgrade. Impersonating an existing model is the practical
  route. No supported way to define custom models was found, and no official
  "third-party managed switch" feature was found.
- The OpenWrt investigation disassembled the model enum (`javap -c -p`). Each
  constant carries device type, capability set, chipset, system ID, port
  count and Ethernet class. Examples: `US6XG150` is BROADCOM, 6 ports,
  GIGABIT_ETHERNET, sysid `0xeb23`. `USWED76` is REALTEK, 10 ports,
  TEN_GIGABIT_ETHERNET, sysid `0xed76`. `USL8A` is REALTEK, 8 ports, sysid
  `0xed2d`. Whether the controller changes behaviour by `chipset` is
  **unverified**.
- When `port_table` has fewer ports than the model: on an AP, openUF saw that
  the `switch.port.N` inventory in `system_cfg` follows the model's port count,
  while the Ports view and per-port settings follow the reported `port_table`
  (`PROTOCOL-VALIDATION.md` lines 932–1006). **Confirmed in the lab for a
  USW** (emulated `USWED36` reporting 9 of 10 ports): `stat/device` stores the
  9 reported ports, `system_cfg` still carries `switch.port.10.*`.

### Reporting a port layout that differs from the model

Two meanings of "override" appear in these projects. Keep them apart:

- **UniFi port overrides** (`port_overrides`, the per-port settings an admin
  sets in the controller) are controller-to-device *configuration*. They
  arrive as `switch.port.N.*` keys in `system_cfg`. They do not change the
  port layout.
- **Emulator port-layout overrides** are device-side settings that change what
  the device *reports* in `port_table`, while keeping a borrowed model code.
  unifi-emu has a per-device `ports` count that "overrides the profile port
  layout when > 0" (`device.go:63`, `resolvePorts` in `descriptor.go`, test
  `TestBuildDescriptorPortOverride` builds a 3-port `USWED74`; revision
  `a538889`). unifi-stubd profiles set "port count, names, roles, network
  groups, media, and speed layout" per device, and say mixed-speed switches
  should report the complete physical layout in `port_table`
  (`docs/en/architecture.md:101`, `docs/en/protocol-notes.md:266`; revision
  `c0f7203`).

What the controller does with a reported layout that differs from the model:

- openUF (on an AP, Network 10.6) found the Port Manager lists exactly the
  ports in the reported `port_table` (two), not the model registry's five.
  Per-port settings join on `port_table[].port_idx`. The `switch.port.N`
  name/opmode entries in `system_cfg` still follow the registry count, but
  are inventory only (`PROTOCOL-VALIDATION.md` lines 952–990, revision
  `3da5719`).
- Things that come from uidb, not from the device, presumably still follow
  the model: the front-panel diagram, product image, port-category labels
  (`standard`/`plus`/`sfp`) and the `switch.port.N` inventory.
  The inventory and the port names (`SFP+ 1/2` on `US6XG150`) are
  **confirmed in the lab** to follow the model. The UI drawing was not
  checked.

So a borrowed model code plus a port_table describing the real hardware is a
workable route when no model matches exactly. The cost is a wrong front-panel
picture. For the two chosen models (`USWED36`, `USL8LP`) the layouts already
match, so this is only needed for the other RTLPlayground layouts (for
example 5 x 2.5G + 1 x SFP+).

### Port index translation

The controller keys every per-port setting on `port_table[].port_idx`, and
`system_cfg` uses the same index in `switch.port.N.*` and
`switch.vlan.K.port.N.mode`. Firmware ports already have two numberings:
`struct machine` holds `log_to_phys_port[9]` and `phys_to_log_port[9]`
(`machine.h`). UniFi needs a third, defined per machine together with the
impersonated model:

- `unifi_to_log_port[]`: UniFi `port_idx` (1-based) to RTLPlayground logical
  port, with a sentinel for "no such port". A UniFi port without a mapping is
  reported `enable=false, up=false` and its `system_cfg` keys are ignored.
- The reverse lookup is a scan of that table, which is at most 10 entries.
- Use it in both directions: when building `port_table` (logical to UniFi),
  and when parsing `system_cfg` (UniFi to logical, then the existing
  logical-to-physical map).

Default is identity. Planned non-identity cases:

| Machine | Model | Mapping |
| --- | --- | --- |
| 8 x 2.5G + 1 x SFP+ | `USWED36` | 1–8 identity; UniFi 9 = our SFP+ port; UniFi 10 unmapped (other half of the combo slot) |
| TL-SG108PE V5 | `USL8LP` | Identity if the USL8LP PoE ports are 1–4. Otherwise map its PoE `port_idx` values to our ports 1–4 |
| 5 x 2.5G + 1 x SFP+ | `US6XG150` | 1–5 identity; UniFi 6 = our SFP+ port. UniFi shows port 5 as SFP+. Untested on real hardware |

### Candidate models from uidb 10.6.109 (USW, up to 12 ports)

| Model code | SKU / name | Ports (uidb) | Diagram | Max speed field | Notes |
| --- | --- | --- | --- | --- | --- |
| `USMINI`, `USMINI2` | USW-Flex-Mini | 5 standard | — | 1000 | `MCU_BASED`. Minimum firmware 1.2.1. |
| `USWED35` | USW-Flex-2.5G-5 | 5 standard | — | 2500 | `MCU_BASED`. Built on RTL8372N. Minimum firmware 1.1.0. |
| `USF5P` | USW-Flex | 5 standard | — | 1000 | PoE. |
| `USFXG` | USW-Flex-XG | 5 standard | `01 __ 02 03 04 05` | 1000 | |
| `US6XG150` | US-XG-6PoE | standard 1-4, plus (SFP+) 5-6 | `01 02 03 04 __ 05 06` | 1000 | PoE, temperature sensor. Minimum firmware 4.0.6. |
| `US8`, `USC8` | US-8 | 8 standard | — | 1000 | PoE pass-through. |
| `USC8P60` | US-8-60W | 8 standard | — | 1000 | PoE. |
| `USL8LP`, `USL8LPB` | USW-Lite-8-PoE | 8 standard | — | 1000 | PoE. |
| `USL8A` | USW-Aggregation | 8 plus | — | 1000 | 8x SFP+. |
| `USM8P`, `USM8P60`, `USM8P210` | USW-Ultra | 7 standard, plus [8] | — | 1000 | `MCU_BASED`. |
| `USL8MP` | USW-Mission-Critical | 9 standard | `01 … 08 _ 09` | 1000 | Smart-power and battery capabilities. |
| `USWED36` | USW-Flex-2.5G-8 | 10 standard, `meta.09.combo=[09,10]` | `01 02 03 04 05 06 07 08 __ 09 10` | 10000 | `MCU_BASED`. Minimum firmware 1.1.0. |
| `USWED37` | USW-Flex-2.5G-8-PoE | same as USWED36 | — | 10000 | PoE. |
| `US68P` | USW-Enterprise-8-PoE | 8 standard, sfp [9,10] | — | 1000 | |
| `USC8P150` | US-8-150W | 8 standard, sfp [9,10] | — | 1000 | |
| `USLP8P` | USW-Pro-8-PoE | 8 standard, plus [9,10] | — | 1000 | |
| `USWED76` | USW-Pro-XG-8-PoE | 8 standard, plus [9,10] | — | 10000 | |
| `USWED97`, `USWED98` | USW-Industrial-DIN-8(-PoE) | 8 standard, sfp [9,10] | — | 1000 | |
| `USWED74`, `USWED75` | USW-WAN(-RJ45) | 4 | — | 10000 | WAN relay. Not a general switch. |
| `USWED77` | USW-Pro-XG-10-PoE | 10 standard, plus [11,12] | — | 10000 | |

Suggested mapping for the layouts in `doc/supported_devices.md`:

Hardware test coverage: the maintainer can test `USWED36` (8 x 2.5G + 1 x SFP+)
and `USL8LP` (TL-SG108PE V5) on real switches. The `US6XG150` targets (4 + 2
and 5 + 1 boards) can only be validated against a lab controller with an
emulator. They stay **untested on real hardware** until someone with such a
board tests them.

| RTLPlayground layout | Best model | Fit |
| --- | --- | --- |
| 4 x 2.5G + 2 x SFP+ (most common) | `US6XG150` | Exact topology (4 RJ45, then 2 SFP+). The model is PoE and its RJ45 ports are 10G. Per port, report `poe_caps=0` and `port_poe=false` (no PoE), and `speed_caps=1048682` on copper (autoneg, 10F, 100F, 1000F, 2.5G) and `288` on SFP+ (1G, 10G). In the lab (emulated) the controller then sent no PoE keys and refused 5G/10G on copper through the API (`InvalidInterfaceSpeedDuplex`), while 10G stayed allowed on SFP+. What the web UI shows was not checked. |
| 4 x 2.5G + 2 x 10G RJ45 (ZX310S-4T2XT) | `US6XG150` | Ports 5 and 6 are drawn as SFP+. |
| 5 x 2.5G + 1 x SFP+ | no exact match | `US6XG150` (port 5 drawn as SFP+) or `USWED36` with ports 6–8 permanently down. |
| 5 x 2.5G (no SFP) | `USWED35` | Exact, and the same switch silicon. |
| 8 x 2.5G + 1 x SFP+ | `USWED36` (`USWED37` for PoE boards) | **Chosen.** Same physical layout as the RTLPlayground 2.5G switches (confirmed by the maintainer). The combo slot 9/10 is the uplink. Report the SFP+ as 9 or 10 and the other as down. |
| 8 x 1G, 4 PoE+ (TP-Link TL-SG108PE, RTL836x) | `USL8LP` (USW-Lite-8-PoE) | **Chosen.** 8 GbE RJ45 ports, 4 of them 802.3at PoE+, 52 W budget, 30 W per port ([UI tech specs](https://techspecs.ui.com/unifi/switching/usw-lite-8-poe), [EU datasheet](https://intellcom.net/files/pdf/1655441745753/USW_Lite_8_POE_EU_Datasheet.pdf)). uidb has no per-port PoE map (`ports.standard: 8`, `features.poe: true`), so report `poe_caps` only on ports 1–4 and `0` on 5–8. On the TL-SG108PE V5, PoE is on ports 1–4 (`machine/TPLINK_TL_SG108PE_V5.h`: `MACHINE_POE_PORTS 4`; `doc/devices/2060500741.md`). That the USW-Lite-8-PoE also numbers its PoE ports 1–4 is **unverified**. If it does not, map its PoE `port_idx` values onto our PoE ports in the firmware. The cost is that UniFi's port numbers then differ from the labels on the case. `US8` (PoE pass-through) and `USC8P60` (802.3af, not PoE+) fit less well. |

Points that make `MCU_BASED` models (USWED35/36/37, USMINI) attractive, and
some risks:

- They are Ubiquiti's own microcontroller switches, so the controller probably
  expects a smaller feature set from them. This is **unverified**. uidb has no
  `knownUnsupportedFeatures` for them, and their firmware uses 1.x version
  numbers (`minimumFirmwareRequired` 1.1.0 / 1.2.1). The 4.x/6.x/7.x versions
  belong to the Linux USW firmware.
- Their inform and `system_cfg` may differ from the Linux USWs. Nobody has
  published a capture of the real firmware. On the controller side, the lab
  showed the same `system_cfg` format for `USWED36` as for `USL8LP` and
  `US6XG150` (emulated devices).

Firmware version and update handling:

- Report a `version` at or above the model's `minimumFirmwareRequired`, and
  preferably the current release. The controller has two-phase adoption that
  can *require* an upgrade (`adoptable_when_upgraded`, `two-phase adoption:
  telling device[{}] to upgrade`), and an "unsupported" state where
  provisioning is skipped (`dev[{}] (unsupported) provisioning skipped, device
  now stale`). Both are *string evidence*. The current version per model can be
  looked up on `fw-update.ui.com` (the same API used to find the controller).
- Ignore `upgrade`, and keep reporting the same version. unifi-simulator and
  unifi-stubd do this. How the controller UI behaves when an upgrade "never
  finishes", and whether auto-update keeps retrying, is **unverified**.

## Feasibility on RTL837x / 8051

All numbers in this section come from the two images the maintainer runs:
`MOKERLINK_ZX_SWTGW218AS` (RTL8373, 8 x 2.5G + 1 x SFP+, reported as
`USWED36`) and `TPLINK_TL_SG108PE_V5` (RTL8370N, 8 x 1G with PoE on ports 1–4,
reported as `USL8LP`). Both were built from `e9f09fc5` with SNMP and
Prometheus enabled. The memory figures come from the linker's `.mem` and `.map`
files. Crypto timings come from an 8051 simulator, not from hardware.

### Measured resources

| Resource | RTL8373 (`MOKERLINK_ZX_SWTGW218AS`) | RTL8370N (`TPLINK_TL_SG108PE_V5`) |
| --- | --- | --- |
| XDATA used (`XSEG`+`XISEG`) | 20,885 B (`0x0001`–`0x5195`) | 19,919 B (`0x0001`–`0x4DCF`) |
| XDATA free below `0xBF00` | **28,010 B** | **28,976 B** |
| Internal RAM | Full. 140 B left for the stack | Full. 130 B left for the stack |
| Free code: HOME / BANK1 / BANK2 / BANK3 / BANK4 | 4,014 / 5,669 / 3,410 / **851** / 3,177 B | 4,249 / 10,155 / 14,650 / 15,712 / 9,203 B |
| Free code, total | **17,121 B**, in five pieces, the largest 5.7 KB | 53,969 B |
| Flash `0x34000`–`0x3FFFF` | Empty: room for exactly one more 48 KB bank (BANK5) before the web files at `0x40000` | Same |

Notes:

- The linker gets `--xram-size 49151` (`Makefile:336`), so `0x0000`–`0xBFFF`
  is what the firmware uses. The RTL8370N datasheet gives 48 KB of data memory
  (`doc/tl-sg108pe-v5-firmware.md`). `doc/hardware.md` says 64 KB for the
  RTL837x, but no build uses anything above `0xBFFF`. `0xBF00`–`0xBFFF` holds
  the ChaCha20 state (`crypto/chacha20.c:14`). That code is linked into BANK4,
  but nothing calls it.
- The largest XDATA users are `config_buf` (4,480 B), `outbuf` (2,500 B),
  `lldp_nb` (2,074 B), `cmd_history` (2,048 B) and `uip_buf` (1,560 B).
- `imagebuilder` packs bank N at flash `(N-1) * 0xC000 + 0x4000`, so BANK5 is
  flash `0x34000`. Neither image has a non-blank byte there. The stock
  RTL8370N firmware uses 15 code banks, so the bank register has room for
  more.
- **Memory is not the constraint. Code space on the RTL8373 build is.** The
  UniFi client does not fit into the free fragments there, so it needs BANK5:
  add `-Wl-bBANK5=0x54000` to the link line. `imagebuilder` derives the bank
  count from the image size. Use the same layout on both builds.

### TCP connections

uIP itself is cheap per connection. Each `struct uip_conn` is 30 bytes (with the
2-byte `httpd_state`). One `uip_buf` serves all connections, because uIP handles
one packet at a time. uIP does not keep sent data, so on a retransmit the
application must send the same bytes again. The httpd keeps its whole
response in the global `outbuf` (2,500 B), with global cursors (`slen`, `o_idx`,
`cont_len`, `cont_addr`, `metrics_more`). That is why the httpd can serve one
connection at a time, and why `UIP_CONF_MAX_CONNECTIONS` is 1
(`uip/uip-conf.h:93-97`: "TODO: Increase this, but also make the socket
state/buffer per-connection").

**Second connection (UniFi inform): yes.** The inform client is an active open
(`uip_connect`, `UIP_ACTIVE_OPEN` is already 1) to the controller's port 8080.
It has its own buffer and state, so the httpd does not change. Needed:

- `UIP_CONF_MAX_CONNECTIONS 2`.
- A `UIP_APPCALL` dispatcher that sends a connection whose remote port is 8080
  to `unifi_appcall()` and everything else to `httpd_appcall()`.
  `uip_tcp_appstate_t` becomes a union of both states.
- At most one port-80 connection at a time. Today the single slot enforces
  this. With more slots, a second browser connection would take a slot and
  then find the httpd's global buffer in use. Refuse a SYN to port 80 while a
  port-80 connection exists. This is a small check in the SYN path of
  `uip.c`.
- Extend the `health` TCP dump, which stops the build when `UIP_CONNS != 1`
  (`cmd_parser.c:2664`).

**Third connection (metrics): yes, on a separate port.** Today a Prometheus
scrape and the web UI take turns on the one httpd slot. To separate them:

- Serve `/metrics` on a second listening port as well
  (`UIP_CONF_MAX_LISTENPORTS 2`).
- Give that connection its own TX buffer of one MSS (1,472 B) and its own
  cursor.
- Have `metrics_fill()` write to a buffer passed in, not `outbuf`. It
  already produces the scrape chunk by chunk, so the existing retransmit
  logic carries over.

The cost is about 1.5 KB XDATA, 30 B for the connection and a few hundred
bytes of code. Making the httpd itself multi-connection would be the larger
change: every global cursor would move into per-connection state, and each
connection would need its own `outbuf`.

The uIP receive window is one MSS (`uip/uipopt.h:314`), so every transfer is
stop-and-wait. On a LAN, a 7.6 KB `system_cfg` response is six round trips,
which is fine.

### Crypto cost (measured in a simulator)

Method: forward-only AES-128 in C, AES-128-GCM with two GHASH variants, built
with SDCC 4.6.0 `--model-large` and run in ucsim (`s51`). Both variants
reproduce NIST GCM test case 2 (K = 0, IV = 0^96, P = 0^128, tag
`ab6e47d42cec13bdf53a67b21257bddf`). The cycle counts are 8051 machine cycles
(ucsim ticks / 12). The code size includes the test harness and the SDCC
`memcpy`/`memset`, so it is an upper bound.

| GHASH variant | Cycles per 16-byte block (CTR + GHASH) | Code + tables | XDATA |
| --- | --- | --- | --- |
| Bitwise | ≈168,700 | 2,318 + 256 B | ≈0.4 KB |
| 4-bit table (Shoup) | **≈72,100** (AES ≈24,000, GHASH ≈48,000) | 2,723 + 288 B | ≈0.6 KB (256 B table) |

The UniFi IV is 16 bytes, not 12, so J0 costs two more GHASH blocks per
message. That is negligible.

Wall time depends on the 8051's real instruction rate, which is **not
measured**. The DW8051 core takes 4 clocks per machine cycle. The timer
divider comment (`rtl837x_regs.h:66`) suggests a 125 MHz clock, which would
give at most 31 M cycles/s. Code is fetched from SPI flash through a small
cache, so the real rate can be lower. The table assumes 31 M and 10 M
cycles/s and uses the 4-bit variant:

| Message | Size | Cycles | At 31 M/s | At 10 M/s |
| --- | --- | --- | --- | --- |
| Full inform (lab payload, 8–10 ports) | ≈6 KB | ≈27 M | 0.9 s | 2.7 s |
| Trimmed inform (target, see below) | ≈2.5 KB | ≈11.5 M | 0.4 s | 1.2 s |
| `system_cfg`, `USWED36` default site | 5.8 KB | ≈26 M | 0.8 s | 2.6 s |
| Largest `system_cfg` seen | 7.6 KB | ≈34 M | 1.1 s | 3.4 s |
| `noop` | 163 B | ≈0.8 M | < 0.1 s | < 0.1 s |

The controller sets the inform interval through `noop` (10–32 s in the lab).
At 10 s, a full inform in C costs 9–27 % of the CPU and a trimmed one 4–12 %.
`system_cfg` only arrives when something changes. An assembly AES and GHASH,
written the way `crypto/chacha_8051.asm` is, should be several times faster
(**estimate**). Whatever the speed, the crypto must not run inside one uIP
callback, because a 1.4 KB segment is about 6.6 M cycles. It has to run from
the main loop, at most 2 blocks (≈144 K cycles, 5–14 ms) per pass. LACP's
timers run 50 times per second.

**Next measurement:** add a CLI command that times 100 AES blocks against the
5 ms system tick on each switch. This replaces the 31 M / 10 M assumption with
the real figure.

### Compact protocol design

This design fits both switches:

1. **AES-128-GCM only, forward cipher only.** The device sends GCM from its
   first inform. It sends no CBC, so it needs no inverse cipher, and it uses
   no zlib or snappy in either direction. All three are lab-confirmed.
2. **One message buffer, used half-duplex (10 KB XDATA).**
   - Outbound: the inform JSON is built into the buffer from one counter
     snapshot and encrypted in place, 2 blocks per main-loop pass. The
     40-byte header is the GCM AAD. Then it is sent, and retransmits resend
     from the buffer. Nothing is regenerated, so the JSON generator does not
     need to be deterministic.
   - Inbound: the response lands in the same buffer once the request is
     acknowledged. It is decrypted and hashed in place, 2 blocks per pass,
     and nothing is used until the tag verifies.
   - A response larger than the buffer is dropped and logged. The largest
     seen was 7.6 KB.
3. **Apply `system_cfg` through the existing CLI.**
   - Translate the keys into the commands `config.txt` already uses
     (`vlan`, `pvid`, port enable, speed and so on).
   - Run them through `cmd_parser`. Do this only after the tag verifies and
     the whole response has parsed.
   - This avoids a second implementation of every setting. The existing
     config save makes the result persistent.
4. **Trim the inform.** Send only what the controller needs. Earlier in this
   document, `port_table` with
   `port_idx`/`up`/`enable`/`speed`/`rx_bytes`/`tx_bytes` was enough, and
   unifi-simulator was Connected with 6 fields per port. The target is about
   2.5 KB, against 5–6 KB for the full lab payload (**estimate**). This
   halves the recurring crypto cost.
5. **Make nonces unique by construction.** The 16-byte IV is a 4-byte boot
   counter kept in flash, a 4-byte message counter and 8 bytes from the
   hardware RNG (`get_random_32()`, the RLDP random register). The RNG's
   quality and its state after a reset are unknown. The `authkey` survives
   reboots, and a repeated IV under one key breaks GCM.

Budget, per switch:

| Item | XDATA | Code (estimate) |
| --- | --- | --- |
| Message buffer | 10,240 B | — |
| AES round keys, GHASH table, GCM state | ≈640 B | ≈3 KB in C (measured upper bound above) |
| UniFi state (authkey, cfgversion, inform URL, counters, port snapshot) | ≈1,000 B | — |
| HTTP client, TNBU framing, inform JSON, `system_cfg` to CLI translation | — | ≈8–13 KB |
| Two more `uip_conn` slots, dispatcher | 60 B | ≈0.5 KB |
| Metrics port TX buffer | ≈1,500 B | ≈0.5 KB |
| **Total** | **≈13.4 KB of 28 KB free** | **≈12–17 KB: BANK5 on the RTL8373 build** |

### Admin password protection

What happens today:

- The password is stored in plain text: the `passwd` line of the
  configuration, at most 20 characters (`httpd/httpd.c:83`). So it is in
  every configuration download and backup.
- The login form posts it in clear (`pwd=`, `httpd/httpd.c:658`).
- A password change sends it in clear (`postCmd("passwd " + a)`,
  `html/app.js:5942`).
- The default comes from the `PASSWORD` build macro.

Constraints:

- TLS is out of reach: the handshake needs public-key operations that take
  minutes on this core.
- Browsers only offer WebCrypto (`crypto.subtle`) in a secure context, meaning
  HTTPS or localhost. The browser side therefore needs plain-JavaScript
  crypto.
- The session cookie also travels in clear, so someone sniffing the network
  can still take over a session. What can be protected is the password itself,
  and logins can be made impossible to replay.

Proposal, using only AES so it shares the UniFi AES code:

- **Stored form:** `passwd $a1$<iterations>$<salt>$<K>`, where
  K = PBKDF2(password, 16-byte random salt, iterations) with AES-CMAC-PRF-128
  (RFC 4615) as the PRF. The plain password is never stored.
- **Login:** the login page gets the salt, the iteration count and a fresh
  single-use 16-byte challenge. The browser derives K and sends
  AES-CMAC(K, challenge). The device computes the same value and compares in
  constant time. Neither the password nor K crosses the wire, a captured login
  cannot be replayed, and offline guessing costs a full PBKDF2 per guess.
- **Password change:** the browser sends the new salt, the new iteration
  count, and the new K encrypted under the old K (AES-CTR with the
  challenge as nonce) plus a CMAC tag. The device needs the forward cipher
  only.
- **Serial `passwd <password>` and the build default:** the device runs
  PBKDF2 itself with a small iteration count. At ≈24,000 cycles per AES
  block, 1,000 iterations are ≈24 M cycles, about 1–2.5 s once. Passwords set
  from the web UI can use a much larger count, because the browser does the
  work. A plain `passwd <password>` line in an existing configuration is
  still accepted and converted on the next save.
- **Cost:** about 0.3–0.6 KB of device code on top of AES (CMAC, PBKDF2 and the
  parser), and about 2–3 KB of minified JavaScript (AES, CMAC, PBKDF2). All
  of these are **estimates**.
- **What it does not protect:**
  - Anyone who reads K from the configuration can log in to this switch. They
    cannot learn the password itself, which may be reused elsewhere.
  - Session cookies can still be stolen by sniffing.
  - Keeping management on its own VLAN remains the defence for the transport.

## Lab results (UniFi Network 10.6.109, emulated)

Lab date: 2026-11-20. **All results in this section are against emulated
devices, not real switches.** They show what a stock controller sends and
accepts. They do not show what real Ubiquiti firmware for these models
sends, or what the real `USWED36` MCU firmware does with this config.

### Setup

- Controller: `ghcr.io/jamesbraid/unifi-network:seeded` (image version
  `10.6.109-1`, UniFi Network 10.6.109, classic API on 8443, inform on 8080).
  Its seeded login is `admin` / `unifi-containers-seeded`, not `admin` /
  `admin` as unifi-emu's test suite assumes. The controller ran on an
  internal Docker bridge with no outbound access.
- Devices: [unifi-emu](https://github.com/jamesbraid/unifi-emu) at `a538889`,
  built locally with small lab-only patches: a per-device JSON overlay merged
  into every inform (used to send a realistic `port_table` and
  `switch_caps`), switches for "no compression", "AES-GCM from the first
  inform" and "never echo `cfgversion`", and a dump of every request and
  response. The emulator adopted itself through the classic API
  (`SIM_ADOPT`).
- Models: `USWED36`, `USL8LP` and `US6XG150`, one device each, plus extra
  single-device probes. All three reached Connected (`stat/device` state 1).
- Capture: tcpdump in the controller's network namespace on TCP 8080 from the
  first inform on. A small decoder (stdlib Go: pcap, TCP reassembly, HTTP,
  CBC/GCM, zlib) decrypted all 495 captured exchanges with the default key
  plus the per-device `x_authkey` values, which `stat/device` returns to an
  admin. The decoded payloads matched the emulator's own dumps.

Inform payload used for the main runs (sent from the first inform on in the
fresh-adoption runs):

- `port_table[]` entries with `port_idx, name, ifname, media` (`GE` or
  `SFP+`), `up, enable, speed, full_duplex, autoneg, speed_caps, port_caps,
  is_uplink, flowctrl_rx/tx, jumbo, stp_state, stp_pathcost, lag_member`,
  rx/tx byte, packet, error, drop, broadcast and multicast counters,
  `mac_table: []`, `poe_caps`, plus `port_poe, poe_enable, poe_mode,
  poe_good, poe_power, poe_voltage, poe_current, poe_class` on PoE ports and
  `sfp_found` on SFP+ ports.
- `switch_caps`: `feature_caps` 1582078 (802.1X, isolation, storm control,
  LLDP-MED, egress rate limit, STP, DHCP snooping, IGMP snooping, port
  security, LACP, jumbo, keepalive), `max_aggregate_sessions` 4,
  `max_mirror_sessions` 1, `stp_caps` 11, `vlan_caps` 7,
  `storm_control_caps` 0.
- Top level as unifi-emu sends it (`mac, serial, model, model_display,
  version, ip, hostname, inform_url, uptime, time, cfgversion, default,
  state, fw_caps, isolated, locating, selfrun_beacon`, plus `bootrom_version,
  sys_stats, ethernet_table` after adoption) and `stp_priority`,
  `has_temperature`, `has_fan`, `overheating`. No `udapi_version` was sent.
  The controller still stored `switch_caps` for the 7.x-versioned `USL8LP`
  and `US6XG150` profiles.

### Wire format

- **Encryption.** Pending informs get HTTP 404 with `Content-Length: 0`. The
  adoption reply to a CBC inform is CBC (flags `0x01`), and to a GCM inform it
  is GCM (`0x09`), both under the default key. After `use_aes_gcm=true` every
  reply was GCM. Across the whole capture, response flags were `0x01` (7
  times) or `0x09` (467 times).
- **Compression.** No response was ever compressed. Informs were accepted
  with flags `0x01`, `0x03`, `0x09` and `0x0b`, so the device may send
  uncompressed CBC or GCM from the first inform on.
- **GCM from the first inform** with the default key works. Adoption and
  provisioning were identical to the CBC-first case.
- **HTTP.** Responses carry `Content-Type: application/x-binary` and a
  `Content-Length`, never chunked encoding. The controller keeps connections
  alive.
- **Sizes.** An encrypted GCM response is the JSON plaintext plus 56 bytes
  (40-byte header, 16-byte tag). The JSON wrapper adds about 540 bytes to the
  `system_cfg` (escaping, `mgmt_cfg`, `cfgversion`, `server_time_in_utc`).

| Case | `system_cfg` | Encrypted response |
| --- | --- | --- |
| Emulator default inform (no `switch_caps`, no `speed_caps`), any of the three models | 2178–2286 B | 2704–2816 B |
| Realistic inform with `switch_caps`, default site, `USWED36` / `USL8LP` / `US6XG150` | 5226 / 4780 / 4106 B | 5822 / 5383 / 4689 B |
| Same, after adding two VLAN networks | 5376 / 4930 / 4256 B | — |
| Largest seen (many overrides, IGMP snooping on two VLANs), `USWED36` | 6953 B | 7602 B |
| Adoption reply (`mgmt_cfg` only) | — | 408 B (CBC), 418 B (GCM) |
| `noop` | — | 163 B |

Device-to-controller informs with the realistic payload were 4.7 KB (6
ports) to 6.2 KB (10 ports) of JSON before compression.

### mgmt_cfg

Adoption reply, then the first provisioning reply. The second one has no
`authkey` and no `inform_url`:

```text
capability=notif,fastapply-bg,notif-assoc-stat
selfrun_guest_mode=pass
cfgversion=cfe5c9df7757b6f5
led_enabled=true
stun_url=stun://10.213.77.10:3478/
mgmt_url=https://10.213.77.10:8443/manage/site/default
authkey=<32 hex digits>              # adoption reply only
use_aes_gcm=true
report_crash=true
```

The response JSON is `{"_type":"setparam","mgmt_cfg":…,"server_time_in_utc":"…"}`
for adoption and `{"_type":"setparam","cfgversion":…,"system_cfg":…,"mgmt_cfg":…,"server_time_in_utc":…}`
for provisioning. `noop` looks like
`{"_type":"noop","interval":18,"live_update":false,"include_blocks":[],"server_time_in_utc":"…"}`.
The `interval` varied from 10 to 32 s as more devices were added.

### system_cfg

The three models got the same format. The `MCU_BASED` `USWED36` was not
treated differently from the Linux-based `USL8LP` and `US6XG150`. Example:
`USWED36`, realistic inform, default site with two extra VLAN networks
(IDs, keys and hashes shortened, ports 3–8 and 10 elided because they repeat
port 2):

```text
# unifi
unifi.version=10.6.109
unifi.anonymous_controller_id=<uuid>
unifi.anonymous_site_id=<uuid>
unifi.reporterid=<uuid>
unifi.siteid=<site id>
unifi.idp=enabled
unifi.mcip=239.254.127.63
unifi.key=<32 hex>
unifi.feature.always_send_crash_logs=enabled
# system
system.timezone=UTC0
locale.timezone=UTC0
# users
users.status=enabled
users.1.name=admin
users.1.password=<crypt hash of the site SSH password>
users.1.status=enabled
users.2.name=nobody
users.2.password=x
users.2.shell=/bin/false
users.2.status=enabled
# vlan
vlan.status=disabled
# bridge
bridge.status=disabled
# netconf
netconf.status=enabled
netconf.1.status=enabled
netconf.1.devname=eth0
netconf.1.ip=0.0.0.0
netconf.1.autoip.status=disabled
netconf.1.promisc=enabled
netconf.1.up=enabled
# dhcpc
dhcpc.status=enabled
dhcpc.1.status=enabled
dhcpc.1.devname=eth0
# route
route.status=enabled
# resolv
resolv.status=enabled
resolv.nameserver.1.status=disabled
resolv.nameserver.2.status=disabled
# switch
switch.power_source.status=disabled
switch.managementvlan=1
switch.wevent.idp=enabled
switch.wevent.mcip=239.254.127.63
switch.wevent.key=<32 hex, same as unifi.key>
switch.jumboframes=disabled
switch.mtu=9216
switch.stp.version=rstp
switch.stp.priority=32768
switch.stp.status=enabled
switch.dot1x.status=disabled
switch.vlan.1.id=1
switch.vlan.1.mode=untagged
switch.vlan.1.status=enabled
switch.vlan.2.id=10
switch.vlan.2.mode=tagged
switch.vlan.2.status=enabled
switch.vlan.3.id=20
switch.vlan.3.mode=tagged
switch.vlan.3.status=enabled
switch.dhcp_snoop.status=enabled
switch.routing_enabled=false
switch.port.1.name=Port 1
switch.port.1.lldpmed.opmode=enabled
switch.port.1.lldpmed.topology_notify=disabled
switch.port.1.stp.port_mode=enabled
switch.port.1.stp.edge=auto
switch.port.1.stp.bpdu_guard=disabled
switch.port.1.keepalive=disabled
switch.port.1.eee=disabled
switch.port.1.opmode=switch
switch.port.1.port-security=disabled
switch.port.2.name=Port 2
…                                   (same ten lines for ports 2–10)
# ntpclient
ntpclient.status=enabled
ntpclient.1.status=enabled
ntpclient.1.server=0.ubnt.pool.ntp.org
…                                   (servers 2–4: 1–3.ubnt.pool.ntp.org)
# misc
```

Per-model differences in the default config:

- `USL8LP`: `switch.port.N.poe=auto` (between `opmode` and `port-security`)
  on ports 1–4 only, which are the ports that reported `poe_caps` 7. Ports
  5–8 reported `poe_caps` 0 and got no `poe` line.
- `US6XG150`: ports 5 and 6 are named `SFP+ 1` and `SFP+ 2` by the
  controller (from the model, not from the inform). `poe=auto` on ports 1–4
  when they report `poe_caps` 7.
- `USWED36`: ports 1–10 as above, no `poe` lines.

Without `switch_caps` in the inform (the emulator's default) the switch
section shrinks to `power_source`, `managementvlan`, `wevent.*`,
`jumboframes`, `mtu`, the VLAN slots, `routing_enabled`, and only `name` and
`opmode` per port. The `stp.*`, `dot1x`, `dhcp_snoop`, `lldpmed`, `keepalive`,
`eee` and `port-security` keys appear only once the matching capability is
claimed. So the capability bits directly control which keys the device must
parse. When `speed_caps` is missing the controller stores 1048623 (gigabit
copper) for the port.

The `users`, `unifi.*`, `ntpclient`, `netconf`, `dhcpc`, `route` and
`resolv` sections are not switch configuration and can be ignored by
RTLPlayground. `users.1.password` is a crypt hash of the site's device SSH
password.

### Settings changed through the API, and the resulting keys

One change at a time, through `PUT /api/s/default/rest/device/<_id>`
(`port_overrides` or device fields), `PUT /api/s/default/rest/setting/<key>/<_id>`
(site settings) or a port profile. Each row is the diff of `system_cfg`
against the previous state. `N` is the overridden port, `K` a VLAN slot.
Results were identical on all three models unless a model is named.

| Change (API field) | `system_cfg` diff |
| --- | --- |
| Native VLAN 10 (`native_networkconf_id`) | `+switch.port.N.pvid=10`; a line for port N in *every* slot: `switch.vlan.<VID 10 slot>.port.N.mode=untagged`, the others `=tagged` |
| Block all tagged (`tagged_vlan_mgmt=block_all`) | other slots' `port.N.mode` become `exclude` |
| Custom tagged, VLAN 20 excluded (`tagged_vlan_mgmt=custom`, `excluded_networkconf_ids`) | VLAN 1 back to `tagged`, VLAN 20 stays `exclude` |
| Port disable (`forward=disabled`, `native_networkconf_id=""`) | `+switch.port.N.status=disabled` |
| Forced speed (`autoneg=false, speed=1000, full_duplex=true`) | `+switch.port.N.autoneg=disabled`, `+duplex=enabled`, `+speed=1000`, `+ld_mode=disabled` |
| Half duplex (`full_duplex=false, speed=100`) | `duplex=disabled`, `speed=100` |
| Speed 2500 / 5000 / 10000 | `speed=2500` / `5000` / `10000`, only where `speed_caps` allows it (see below) |
| Back to autoneg (`autoneg=true`) | the four lines above are removed |
| PoE off / passive 24 V (`poe_mode=off` / `pasv24`) | `switch.port.N.poe=shutdown` / `passive24v` (default `auto`) |
| LAG on ports A,B (`op_mode=aggregate`, `aggregate_members=[A,B]` on A) | `switch.port.{A,B}.opmode=aggregate`, `switch.port.{A,B}.lag=1`. No global LAG key. On `US6XG150` port B also inherited port A's forced-speed lines |
| Mirror (`op_mode=mirror`, `mirror_port_idx=2` on port 4) | `switch.port.4.opmode=mirror`, `+switch.port.4.mirror_port=2` |
| STP off on a port (`stp_port_mode=false`) | `stp.port_mode=disabled`, and `stp.edge` / `stp.bpdu_guard` lines removed |
| Edge + BPDU guard (`stp_edge_state=enabled`, `stp_bpdu_guard_enabled=true`) | `stp.edge=enabled`, `stp.bpdu_guard=enabled` |
| Bridge priority (device `stp_priority=4096`) | `switch.stp.priority=4096` |
| STP version (site `global_switch.stp_version`) | `switch.stp.version=stp`; `disabled` also sets `switch.stp.status=disabled` |
| Storm control, level mode, bcast 10 % | without `storm_control_caps` bit 1: `stormctrl.status=enabled`, `stormctrl.type=rate`, `stormctrl.bcast=100` (the level was not passed through). With `storm_control_caps=1` and bcast 10 / mcast 20 %: `stormctrl.type=level`, `stormctrl.bcast=10`, `stormctrl.mcast=20` |
| Storm control, rate mode (mcast 1000, ucast 2000) | `stormctrl.mcast=1000`, `stormctrl.ucast=2000` |
| Port isolation (`isolation=true`) | `+switch.port.N.isolation=enabled` |
| Egress limit (`egress_rate_limit_kbps_enabled=true`, `egress_rate_limit_kbps=10000`) | `+switch.port.N.egress_rate_limit_kbps=10000` |
| LLDP-MED off (`lldpmed_enabled=false`) | `lldpmed.opmode=disabled`, `topology_notify` line removed |
| LLDP-MED notify (`lldpmed_notify_enabled=true`) | `lldpmed.topology_notify=enabled` |
| EEE (`eee_enabled=true`) | `switch.port.N.eee=enabled` |
| Per-port flow control (`flow_control_enabled=true`) | nothing |
| Site flow control (`global_switch.flowctrl_enabled=true`) | `+switch.port.N.flowctrl=enabled` on every port |
| Port security with one MAC | `port-security=enabled`, `+port-security.mac.1=00:11:22:33:44:55` |
| Loop protection (`port_keepalive_enabled=true`) | `keepalive=enabled` |
| Port name | `switch.port.N.name=<name>` |
| Jumbo frames (site `global_switch.jumboframe_enabled`) | `switch.jumboframes=enabled`; `switch.mtu` stays 9216 |
| DHCP snooping off (site `global_switch.dhcp_snoop=false`) | `switch.dhcp_snoop.status=disabled` |
| Management VLAN (device `mgmt_network_id` = VLAN 10 network) | `switch.managementvlan=10` |
| IGMP snooping on for VLAN 1 and 10 (site `igmp_snooping.enabled`, `network_ids`) | per slot K: `switch.vlan.K.igmp_snooping=true`, `igmp_fastleave=false`, `igmp_maxresponse=10`, `igmp_mcrtrexpiretime=180`, `igmp_groupmembership=260`, `igmp_supression=false`, `igmp_querier_status=true`, `igmp_querier=` (empty); plus `switch.multicast.forward_unk_mcast_router_ports=true` |

Notes on the API:

- Device-level `stp_version`, `jumboframe_enabled` and `flowctrl_enabled`
  are refused (`api.err.SpanningTreeChangeNotAllowed`,
  `api.err.JumboFrameChangeNotAllowed`, `api.err.FlowControlChangeNotAllowed`).
  They are site settings (`global_switch`).
- A port override with only `forward=disabled` was stored as
  `forward=all`, and a port profile with `forward=disabled` was stored the
  same way. The override was only kept as disabled when it also had
  `native_networkconf_id=""` and `tagged_vlan_mgmt=block_all`. The OpenWrt
  lab isolated this on a `US6XG150`: all three fields are required, and
  `setting_preference` is not. Without `block_all` the port was stored as
  `forward=customize` and tagged in every slot. Without the empty native
  network no `status` line was sent.
- IGMP fast-leave and flood-unknown lists (`fastleave_for_network_ids`,
  `flood_unknown_multicast_for_network_ids`) produced no change. Their keys
  are still unknown.
- A forced speed or duplex not covered by the port's reported `speed_caps`
  is refused with `api.err.InvalidInterfaceSpeedDuplex` (field `port_idx`).
  Observed: 100 half refused on ports with `speed_caps` 1048682 and 1049064;
  2500/5000/10000 refused on `USL8LP` (1048623); 5000/10000 refused on
  `USWED36` copper (1048682); on SFP+ ports with `speed_caps` 288, 10000
  accepted and 100, 2500 and 5000 refused.

### Port layout differing from the model

Two fresh devices were adopted while reporting the RTLPlayground layout from
their first inform: `USWED36` with 9 ports (8 × 2.5G copper with
`speed_caps` 1048682, port 9 `SFP+` with `speed_caps` 288, no port 10) and
`US6XG150` with 4 × 2.5G copper (`speed_caps` 1048682, `poe_caps` 0) plus 2
`SFP+` (288).

- **Stored port table.** `stat/device` `port_table` held exactly the reported
  ports, with the reported `speed_caps`, `poe_caps` 0 and no `port_poe` /
  `poe_mode`. The `USWED36` had 9 entries.
- **system_cfg inventory follows the model.** The 9-port `USWED36` still got
  the full `switch.port.10.*` block. A name set on port 10 through the API
  was accepted and sent (`switch.port.10.name=ghost`). A forced speed on port
  10 was refused (`InvalidInterfaceSpeedDuplex`), because there is no
  `speed_caps` for it. The `US6XG150` ports 5 and 6 stay named `SFP+ 1/2`.
- **PoE.** With `poe_caps` 0 on every port, the fresh `US6XG150` got no
  `switch.port.N.poe` line at all, and `poe_mode=off` on such a port was
  accepted by the API but changed nothing. On the *already adopted*
  `US6XG150` whose ports changed from `poe_caps` 7 to 0, ports that had a
  PoE-related override kept a line, rewritten to `poe=shutdown`, and
  `stat/device` kept stale `port_poe` / `poe_mode` values. Report the real
  `poe_caps` from the first inform.
- **10G / 5G.** With `speed_caps` 1048682 on copper, 5000 and 10000 are
  refused by the API, so no 10G or 5G speed key can reach a 2.5G port. With
  the model-like `speed_caps` 1049064 the same port accepted 5000 and 10000.
- On already adopted devices, the controller kept a port that disappeared
  from the inform (port 10) in `stat/device` with stale `speed_caps` and
  null `up` / `speed`. LAG members show `masked: true`.
- What the web UI draws (front panel, PoE controls) was not checked. Only
  the API and `system_cfg` were.

### Not echoing cfgversion

A device that applies nothing and keeps reporting `cfgversion` `"0"`: the
controller sends the same full `setparam` (same `cfgversion`) on every
inform, never a `noop`, at a 10 s interval. `stat/device` shows state 5
(provisioning) and `known_cfgversion` `"0"`. The device is never shown as
Connected.

### Status of earlier unverified items

- Responses uncompressed: **confirmed**. Uncompressed informs accepted:
  **confirmed** (CBC and GCM). GCM from the first inform: **confirmed**.
- Adoption reply in the request's mode: **confirmed**.
- `mgmt_cfg` contains `inform_url`: **refuted** for 10.6.109.
- `setstate` for switch port config: **not seen**; port overrides arrive in
  `system_cfg`.
- Speed, duplex and autoneg value formats: **settled** (`autoneg=disabled`,
  `duplex=enabled|disabled`, `speed=<Mb/s>`, `ld_mode=disabled`).
- IGMP snooping keys: **settled**, per VLAN slot (`switch.vlan.K.igmp_*`),
  not `switch.igmp_snooping.*`.
- Fewer ports than the model, for a USW: **confirmed** to behave as openUF
  saw on an AP (stored table follows the inform, `system_cfg` inventory
  follows the model).
- `MCU_BASED` models get a reduced `system_cfg`: **refuted** at the
  controller side. `USWED36` gets the same format as the Linux USWs.
- Configured-versus-reported speed mismatch: avoided by the API, which
  refuses speeds outside `speed_caps`.

### Cross-check with the OpenWrt (LG510S-4T2XS) lab

A parallel lab for an OpenWrt switch used the same controller image (10.6.109)
and unifi-emu `a538889`, impersonating `US6XG150` and a `US8` that reports a
4 + 2 layout (`andrebrait/lg510s-firmware-lab`, `docs/unifi-adoption.md`,
commit `fffa926`, section "Lab validation"). Every overlapping result agrees
with this lab. One apparent mismatch was resolved: `switch.port.N.poe` is only
sent when the port reports `port_poe` and the other per-port PoE fields, not
`poe_caps` alone. That lab also found the following, which this lab did not
test:

- **802.1X:** site `global_switch.dot1x_portctrl_enabled` and
  `radiusprofile_id` give `switch.dot1x.status`, `switch.dot1x.dynvlan`,
  `switch.dot1x.guest_vlan`, `switch.radius.{auth,acct}.N.{ip,port,secret}`
  (the RADIUS secret in plain text) and `switch.port.N.dot1x.mode=force_authorized`
  on every port. `dot1x_ctrl=mac_based` gives `dot1x.mode=mac_based` and
  `dot1x.idle_timeout`.
- **DHCP guard:** network `dhcpguard_enabled` with `dhcpd_ip_1/2` gives
  `switch.dhcp_guard.N.{server,vlan,status}`.
- **Voice VLAN:** `voice_networkconf_id` produces no key.
- **Port profiles are flattened** into per-port keys. Editing a profile
  re-sends them. The device never sees a profile.
- **VLAN slots can renumber.** A VLAN that no port carries is dropped from
  `switch.vlan.*`, and the remaining slots can change number. The parser must
  key VLANs by `.id`, never by slot.
- **A device PUT with `port_overrides` replaces the whole list.** Ports left
  out return to defaults.
- **Link debounce:** `feature_caps2` bit 1 with site
  `global_switch.link_debounce` gives `switch.port.N.link_debounce=<ms>`.
- **Field schemas:** `api/fields/*.json` in
  `lib/internal/internal-dependencies.jar` (`Device.json` for
  `port_overrides`, `PortConf.json`, `NetworkConf.json`, `Setting.json`) list
  every override field with a validating regular expression. This is the
  complete vocabulary the controller can send.
- **Web UI (headless browser):** `poe_caps` 0 on every port hides the PoE Mode
  column, legend and per-port selector, but the model name still reads "US XG
  6 PoE". Reported 10/5/2.5/1G link speeds render correctly. The forced-speed
  list shows exactly the speeds in `speed_caps`. The SFP tab shows
  `sfp_vendor`, `sfp_part`, `sfp_serial`, `sfp_compliance`,
  `sfp_temperature` and `sfp_txpower` (read as dBm). `sfp_rxfault=true`
  raises a critical "Receive SFP Signal Loss" alert.
- **Upgrade:** devices reporting 7.4.1 were marked upgradable. A manual
  upgrade sends `{"_type":"upgrade","version","md5sum","url"}` with a Ubiquiti
  firmware URL, and the site's `mgmt.auto_upgrade` is on by default (03:00).
- **Commands:** locate arrives as `{"_type":"cmd","cmd":"set-locate"}`, and
  restart as `{"_type":"reboot","reboot_type":"soft"}`.

Neither lab ever saw a `switch.lag.N.lacp` key, or any other key that says
whether a LAG runs LACP or is static. This lab claimed the `LACP` bit
(`feature_caps` 1582078 includes 8192). No `api/fields` schema has a LACP
field. Per the OpenWrt lab, Ubiquiti's LAG FAQ
(https://help.ui.com/hc/en-us/articles/360007279753, not fetched here) says
UniFi switches support only LACP (802.3ad), with no static LAGs. So
`opmode=aggregate` with `lag=N` means an LACP bond, and the switch should run
LACP on those ports. `switch.lag.%d.lacp` in `ubntbox` is probably legacy or
used by other models. The controller reads LACP status back from
`port_table[].lacp_state`.

Neither lab saw a `setstate` reply. The OpenWrt lab's 1,028 captured replies
were 983 `noop`, 41 `setparam`, 1 `cmd`, 1 `reboot` and 1 `upgrade`. Neither
lab found what sets `igmp_fastleave`: `fastleave_for_network_ids` left it at
`false` in both.

## Open questions and gaps

1. *(Done for emulated `US6XG150`, `USWED36` and `USL8LP`, see
   [Lab results](#lab-results-unifi-network-106109-emulated); `USWED35` not
   run.)* A real `system_cfg` (and `mgmt_cfg`) from a stock controller for
   `US6XG150` / `USWED36` / `USWED35`, and its size. Quickest path: run
   unifi-emu against a lab controller with `SIM_MODELS=US6XG150` (and the
   other candidates) and decrypt the traffic with pixiedust. Then change each
   switch setting in the UI and diff `system_cfg` after each change, as openUF
   did for VLANs. That diff is the specification for the config parser.
2. *(Settled: never, on 10.6.109.)* Whether stock controller *responses* are
   ever compressed.
3. *(Settled: accepted.)* Whether an encrypted but uncompressed inform is
   accepted. Still open: the exact "limit for default authkey" (above 5.5 KB
   inflated).
4. Whether a device may stay on CBC after `use_aes_gcm=true`. Per openUF
   (10.4.57), it may not: provisioning waits for a GCM inform.
5. `switch_caps`, `speed_caps` and `port_caps` are answered by unifi-emu's
   `capability_bits.json`. The effect of each bit in the 10.6 UI, and the
   meaning of `vlan_caps`, `fw_caps` and `hw_caps` for switches, are still
   open.
6. Whether `MCU_BASED` models use a reduced inform or `system_cfg`. The
   controller sends them the full format (lab); what their real firmware
   reports is still unknown.
7. The `state` field values before and after adoption (0, 1, 2 and 4 are all in
   use across projects). 1 while pending and 4 once adopted work (lab).
8. *(Settled in the lab, see the key table there.)* Value formats for speed,
   duplex and autoneg overrides, and the IGMP snooping keys. Still open: the
   keys for IGMP fast-leave and unknown-multicast flooding, and the 802.1X
   and voice-VLAN keys, which were not exercised.
9. How the controller behaves when an `upgrade` is ignored, and whether reporting a version at or above the latest for the model stops the offers. The upgrade message itself and the nightly auto-upgrade default were captured by the OpenWrt lab.
10. *(Settled for the API and `system_cfg`, see the lab.)* How the
    controller treats a device whose `port_table` has fewer ports than the
    model. Omitting port 10 on `USWED36` works; the device must still accept
    and ignore `switch.port.10.*`. The UI view was not checked.
11. Help-centre pages for ports and adoption methods (help.ui.com) could not be
    fetched (HTTP 403).
12. The controller analysis is limited to string constants of one build
    (10.6.109). Obfuscated class names change between builds.

## Legal and terms of service

Ubiquiti's EULA (https://www.ui.com/legal/eula/) and Terms of Service
(https://ui.com/legal/termsofservice/) prohibit reverse engineering or
decompiling Ubiquiti software. The licensed-firmware agreement
(https://www.ui.com/legal/lfua) says the same. Search snippets show the EULA
has an exception clause that was cut off, and the full current wording was not
read. This research only read bundled data files and string constants. No
code was decompiled, and no Ubiquiti code or firmware is reproduced here. An
emulator implemented from the third-party documentation above does not need
any Ubiquiti code. Shipping a device that claims a Ubiquiti model code may
raise trademark questions. This is not legal advice.

## Sources

Projects (source files read at the revisions current on 2026-11-20):

- https://github.com/MartinDejmal/unifi-simulator: `README.md`,
  `docs/protocol-research.md`, `src/mtunifi/profiles.py`,
  `src/mtunifi/telemetry.py`, `src/mtunifi/snapshot.py`
- https://github.com/JohnKiller/TNBU (`ba2c22f`):
  `TNBU.Core/Models/InformPacket.cs`, `DiscoveryPacket.cs`,
  `ManagementConfig.cs`, `SystemConfig.cs`, `Inform/BaseInformBody.cs`,
  `Inform/InformResponse.cs`, `DeviceConfiguration/CfgSwitch*.cs`,
  `TNBU.Simulator/DeviceSimulator.cs`, `TNBU.Simulator/Program.cs`,
  `TNBU.GUI/Services/DeviceManagerService.cs`, `Docs/DiscoveryType.md`
- https://github.com/amd989/unifi-gateway: `unifi_protocol.py`,
  `unifi_gateway.py`, `tlv.py`
- https://github.com/konstruktor1/unifi-stubd: `docs/en/protocol-notes.md`,
  `docs/en/compatibility.md`
- https://github.com/benklop/unifi-tlsg: `README.md`, `src/unifi_tlsg/mapping/ports.py`
- https://github.com/ArmedGuy/unifiction: `README.md`
- https://github.com/mcrute/ubntmfi: `inform_protocol.md`
- https://github.com/jeffreykog/unifi-inform-protocol: `README.md`
- https://github.com/fxkr/unifi-protocol-reverse-engineering: `README.md`
- https://github.com/jda/nanofi: `README.md`, `inform/inform.go`
- https://github.com/qvr/unifi-gateway, https://github.com/stephanlascar/unifi-gateway,
  https://github.com/ZAP-Quebec/unifi-inform, https://github.com/dmke/inform-inspect,
  https://github.com/foxt/unispi, https://github.com/nitefood/python-ubnt-discovery
- https://jrjparks.github.io/unofficial-unifi-guide/protocols/inform.html and
  https://jrjparks.github.io/unofficial-unifi-guide/print.html
- Added from the OpenWrt (LG510S-4T2XS) investigation, not re-read here:
  https://github.com/antoniocosta/unifi-openwrt (`8e87294191`: `docs/internals.md`,
  `internal/inform/packet.go`), https://github.com/yesrab/openUF (`3da571983b`:
  `PROTOCOL-VALIDATION.md`, `REVERSE-ENGINEERING.md`),
  https://github.com/jamesbraid/unifi-emu (`a538889cce`: `docs/PROTOCOL.md`,
  `capability_bits.json`), https://github.com/lucavb/open-unifi (`c1d8ebc604`:
  `docs/PROTOCOL-discovery.md`), https://github.com/jda/pixiedust (`a4356227df`:
  `unifi-fresh-usw.pcap`), and the USW-Aggregation 7.5.15 firmware image
  (`bin/ubntbox`, `bin/mcad`)
- Wireshark UBDP dissector: https://gitlab.com/wireshark/wireshark/-/raw/master/epan/dissectors/packet-ubdp.c
- nmap: https://nmap.org/nsedoc/scripts/ubiquiti-discovery.html

Controller artefacts (UniFi Network Application 10.6.109, from
`https://fw-update.ui.com/api/firmware-latest?filter=eq~~product~~unifi-controller&filter=eq~~channel~~release&filter=eq~~platform~~unix`):

- `UniFi/dl/uidb/uidb.json`, `UniFi/dl/firmware/bundles.json`,
  `UniFi/lib/ace.jar!/system.properties`
- String constants (`javap -v`) from `UniFi/lib/internal/internal-dependencies.jar`:
  `com/ubnt/net/InformServlet.class`, `com/ubnt/service/config/jocpX.class`,
  `com/ubnt/service/config/ubntconf/NDxSQTDwni.class`,
  `com/ubnt/service/devmgr/l/mMObEXELV.class`,
  `com/ubnt/service/devmgr/jRYGVjDqQ$QdjEeOBh.class`,
  `com/ubnt/service/devmgr/nHhxEfQQGSLumjGc.class`,
  `com/ubnt/service/devmgr/TocwLJhQU.class`, `com/a/a/a/QdjEeOBh.class`,
  `com/ubnt/service/v/a/vFiJoQiuQjMZUx.class`, `com/ubnt/i/g/a/XCfthFC.class`,
  `com/ubnt/service/br/JLudBrawuIVqu.class`

Hardware:

- https://kloenk.eu/posts/unifree/usw-flex-mini/ (USW-Flex-Mini: Nuvoton
  Cortex-M4 plus RTL8367RB)
- https://www.servethehome.com/ubiquiti-flex-mini-2-5g-review-ubiquiti-does-a-cheap-5-port-2-5gbe-switch/
  (USW-Flex-2.5G-5: RTL8372N plus RTL8221B)
- https://www.hostifi.com/support/unifi-how-to-adopt-the-usw-ultra (no SSH on
  Flex Mini and Ultra)

Legal:

- https://www.ui.com/legal/eula/, https://ui.com/legal/termsofservice/,
  https://www.ui.com/legal/lfua

Repository facts: `Makefile:336`, `uip/uip-conf.h`, `uip/uipopt.h:233`,
`doc/hardware.md`, `doc/supported_devices.md`, `rtl837x_lldp.h`, `crypto/`.
