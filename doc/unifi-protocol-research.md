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
- Nothing here was tested against a real controller by this project. Claims
  taken from other projects' lab results are attributed to them. Items marked
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
5. **It looks feasible on the 8051, but the hard parts are TCP and memory, not
   crypto.** The inform POST is several KB (estimated 4–8 KB uncompressed for
   5–10 ports with small MAC tables). Device-to-controller compression can be
   zlib "stored" blocks (only Adler-32, no deflate). Two independent sources
   indicate controller responses are encrypted but *not* compressed, so no
   inflate is needed. This is **unverified** against a stock controller. The
   blockers are elsewhere. uIP is configured for one TCP connection, and the
   httpd already uses it. uIP regenerates data on retransmit. The `system_cfg`
   response must be decrypted, tag-checked and parsed as a stream, or spooled to
   flash, before anything is applied.
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
- Whether an *uncompressed* encrypted inform (flags `0x01` or `0x09` only) is
  accepted is **unverified**. TNBU's decoder handles it
  (`InformPacket.cs:94-96`), but the stock controller has not been checked.
  A safe option is a zlib stream made of deflate *stored* blocks (RFC 1950/1951:
  `78 01`, then `[BFINAL|BTYPE=00] LEN NLEN data`, then Adler-32). It is valid
  zlib that any `java.util.zip.Inflater` accepts, and it needs no compressor.
- Controller to device: TNBU's alternative controller sends `IsZLIB=false,
  IsSnappy=false` (`DeviceManagerService.cs:197-201`). Real APs and the
  USW-Pro-48-PoE accept that. unifi-stubd's client goes further and *rejects*
  compressed responses while working against 10.5.67
  (`protocol-notes.md:121-130`). That suggests stock controller responses are
  uncompressed. amd989 and the TNBU simulator still decode zlib and snappy
  responses, so this is **likely but unverified**.

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
3. The device informs with the new key and echoes the `cfgversion`. The
   controller sends `setparam` with `system_cfg` (the full device config) and
   `mgmt_cfg`. The device applies it and reports the new `cfgversion`. After
   that the controller sends `noop` with an `interval`. Whenever the device's
   `cfgversion` differs from the controller's, the controller resends
   `setparam` (TNBU controller logic, `DeviceManagerService.cs:216-246`).
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
on 10.6 is **unverified**. For `upgrade`, unifi-openwrt and openUF report the
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
    for SFP+. Both are **derived, not tested**.
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
`switch.mclag.*`, `switch.oob.*` and `switch.etherlight.*`. This answers which
key carries IGMP snooping. Value vocabularies beyond those in the capture are
**unverified**. This is the firmware for the same Realtek switch family
(RTL930x), not for RTL837x.

On VLAN membership, openUF diffed five controller states. It found that
`switch.vlan.K` is a slot whose `.id` is the VID. The per-VLAN, per-port
matrix (`switch.vlan.K.port.N.mode`) is authoritative, and an absent per-port
key means "default" (openUF `PROTOCOL-VALIDATION.md` lines 932–1006).

Mapping onto RTLPlayground:

| UniFi intent (port profile, port override, network) | system_cfg key | RTLPlayground feature |
| --- | --- | --- |
| Native VLAN / untagged network | `switch.port.N.pvid`, `switch.vlan.K.port.N.mode=untagged` | `vlan.c` PVID and untagged membership |
| Tagged networks ("Allow all" / custom) | `switch.vlan.K.port.N.mode=tagged` | `vlan.c` tagged membership |
| Port disable | `switch.port.N.status=disabled` | port enable or disable |
| Speed / duplex / autoneg | `switch.port.N.speed`, `autoneg`, `duplex` (exact values **unverified**) | `rtl837x_port.c` / PHY |
| Link aggregation | `opmode=aggregate`, `lag` | LACP / `link_aggregation.md` |
| STP | `switch.stp.*`, `switch.port.N.stp.*` | `rtl837x_stp.c` |
| Storm control | `switch.port.N.stormctrl*` | `rtl837x_storm.c` |
| Port mirroring | `mirror_port` | mirroring |
| Port isolation | `isolation` | not checked here |
| PoE mode | `switch.port.N.poe=auto/shutdown` | `poe.h` on PoE boards |
| Management VLAN, MTU, jumbo frames | `switch.managementvlan`, `switch.mtu`, `switch.jumboframes` | existing settings |
| IGMP snooping, DHCP snooping/guard | `switch.dhcp_snoop.status`, `switch.dhcp_guard.*` (IGMP key not seen) | `rtl837x_igmp.c`, `rtl837x_dhcp_snoop.c` |

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
between a configured and a reported speed is **unverified**.

Things not established:

- A real `system_cfg` captured from a stock controller for one of the
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
  (`PROTOCOL-VALIDATION.md` lines 932–1006). For a USW this is
  **unverified**.

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
  **Unverified** for a USW.

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
| 4 x 2.5G + 2 x SFP+ (most common) | `US6XG150` | Exact topology (4 RJ45, then 2 SFP+). The model is PoE and its RJ45 ports are 10G. Per port, report `poe_caps=0` and `port_poe=false` (no PoE), and `speed_caps=1048682` on copper (autoneg, 10F, 100F, 1000F, 2.5G) and `288` on SFP+ (1G, 10G). That the UI then hides PoE controls and offers no speeds above 2.5G on copper is **unverified**, because uidb still says `features.poe: true`. uidb's `ethernetMaxSpeed` of 1000 is odd for an XG product, so **check whether the UI shows 2.5G/10G**. |
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
  published a capture.

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

The resource facts come from this repo. XDATA is about 64 KB, with
`--xram-size 49151` for the linker (`Makefile:336`). Code is banked over 4 MB of
flash (`doc/hardware.md`). uIP has `UIP_CONF_BUFFER_SIZE 1556`,
`UIP_CONF_MAX_CONNECTIONS 1` and `UIP_CONF_MAX_LISTENPORTS 1`
(`uip/uip-conf.h:97,119,133`). `UIP_ACTIVE_OPEN` is 1 (`uip/uipopt.h:233`), but
no module currently calls `uip_connect`. The existing crypto is ChaCha20 only
(`crypto/`). DNS (`dns.c`), DHCP, LLDP neighbours (`rtl837x_lldp.h:115-139`) and
flash write access are already present.

| Need | Option | Cost estimate | Risk |
| --- | --- | --- | --- |
| AES-128 | Forward cipher only, when GCM is used | About 1.5–3 KB code, 256 B S-box (code space), key schedule on the fly or 176 B XDATA. **Estimate.** | Low. |
| GHASH | GF(2^128) multiply. Bitwise, or 4-bit Shoup tables (256 B XDATA per key) | Thousands of 8051 instructions per 16-byte block. A few KB of payload means a few hundred blocks. **Estimate.** | Medium (CPU time). Fine at a 10–30 s interval. |
| AES-CBC instead | Needs the inverse cipher for responses (another 256 B table and InvMixColumns) | More code than GCM | The controller pushes `use_aes_gcm=true`. Whether a device may stay on CBC is **unverified**. |
| IV / nonce | 16 random bytes per packet | — | **Security-critical for GCM.** A repeated nonce under one key breaks it. Needs an entropy source or a persisted counter. |
| Compression (TX) | zlib stored blocks: 2-byte header, 5 bytes per ≤65535-byte block, Adler-32 | Trivial | Low. Possibly not needed at all if uncompressed is accepted (**unverified**). |
| Decompression (RX) | Probably none, since responses look uncompressed (TNBU, unifi-stubd) | 0 | If wrong: inflate needs about 2–4 KB of code and an output window that may not fit, so the response would have to be spooled to flash. |
| JSON (TX) | Printf-style generator, run twice (a counting pass for Content-Length and TNBU length, then an emit pass) from a counter snapshot taken once per inform | Small | uIP regenerates data on retransmit, so output must be deterministic. GCM-CTR can seek to any block. The tag is computed once and kept (16 B). 64-bit counters need `long long` or manual decimal formatting. |
| JSON (RX) | Streaming tokenizer. Decrypt each segment with CTR, unescape the `system_cfg` string, and feed a line parser for `key=value` | Small to medium | The GCM tag is only known at the end. **Do not apply anything before the tag verifies.** Either stage parsed settings in XDATA and commit after the tag, or spool the ciphertext to a flash scratch area and parse in a second pass. |
| HTTP client | Hand-written `POST /inform HTTP/1.1`, `Content-Length`, read the status line and the body | Small | uIP needs `UIP_CONNS` ≥ 2, or an inform client that shares the slot with httpd. |
| Payload size | Synthetic estimate: about 4.4 KB JSON for 5 ports and 8.1 KB for 10 ports, each port with ~30 fields and 3 MAC entries, plus one LLDP entry | 3–6 TCP segments | Drop optional fields first. unifi-simulator was Connected with only 6 fields per port. |
| Response size | `setparam` with `system_cfg`: **unknown**. Estimated as a few KB to tens of KB. | — | Streaming is mandatory. Do not buffer it whole. |
| Memory | Snapshot counters (~10 ports × 8 × 8 B), staged config, AES/GHASH state, inform state (key 16 B, cfgversion ≤ 32 B, URL) | About 1–2 KB XDATA | Current free XDATA was **not measured**, because the build has no `.mem` map for the main image. |

Overall: a minimal "Connected with live port stats" version looks practical:
AES-GCM, stored-zlib, a TX JSON generator, `noop` and `setparam` handling for
`mgmt_cfg` only, and L3 adoption. Applying `system_cfg` (VLANs, PVIDs,
enable/disable, speed) is the larger second step. It depends on a streaming
parser and on capturing a real `system_cfg` for the chosen model.

## Open questions and gaps

1. A real `system_cfg` (and `mgmt_cfg`) from a stock controller for
   `US6XG150` / `USWED36` / `USWED35`, and its size. Quickest path: run
   unifi-emu against a lab controller with `SIM_MODELS=US6XG150` (and the
   other candidates) and decrypt the traffic with pixiedust. Then change each
   switch setting in the UI and diff `system_cfg` after each change, as openUF
   did for VLANs. That diff is the specification for the config parser.
2. Whether stock controller *responses* are ever compressed. Two sources say
   no; one stock capture would settle it.
3. Whether an encrypted but uncompressed inform is accepted, and the "limit
   for default authkey" on payload size before adoption.
4. Whether a device may stay on CBC after `use_aes_gcm=true`. Per openUF
   (10.4.57), it may not: provisioning waits for a GCM inform.
5. `switch_caps`, `speed_caps` and `port_caps` are answered by unifi-emu's
   `capability_bits.json`. The effect of each bit in the 10.6 UI, and the
   meaning of `vlan_caps`, `fw_caps` and `hw_caps` for switches, are still
   open.
6. Whether `MCU_BASED` models use a reduced inform or `system_cfg`.
7. The `state` field values before and after adoption (0, 1, 2 and 4 are all in
   use across projects).
8. Value formats for speed, duplex and autoneg overrides. The IGMP snooping
   key family is `switch.igmp_snooping.*` (Ubiquiti `ubntbox`); its values are
   still unknown.
9. How the controller UI and auto-update behave when an `upgrade` is ignored.
10. How the controller treats a device whose `port_table` has fewer ports
    than the model (for example 9 on `USWED36`). Reporting the extra ports as
    `enable=false, up=false` is the safe default.
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
