# TL-SG108PE V5 Web Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `installer/output/rtlplayground_oem_upgrade.bin` for the TL-SG108PE V5 so it installs RTLPlayground from the stock TP-Link web interface.

**Architecture:** Reuse `installer/` unchanged in structure. `make -C installer MACHINE=TPLINK_TL_SG108PE_V5` compiles `installer.c` for the RTL8370N (16-bit register write for the reset) and passes TP-Link's header magic and marker to the existing `updatebuilder` options.

**Tech Stack:** SDCC 8051, GNU make, the existing `updatebuilder.c`.

**Spec:** `docs/superpowers/specs/2026-11-05-tplink-web-installer-design.md`

## Global Constraints

- Header magic `0x50451085`, marker `0x332256ff` (`updatebuilder -m 50451085 -r 332256ff`).
- RTL8370N reset: `REG_SET(RTL836X_REG_CHIP_RESET, CHIP_RESET_CHIP_RST)` (`0x1322` = `0x0001`), as in `rtlplayground.c:1296`.
- The default build (no `MACHINE`) must produce byte-identical `output/rtlinstaller.ihx` and `output/rtlplayground_oem_upgrade.bin` to before the change.
- The installer copies 120 sectors from `0x1d014` to `0`; it never writes at or above `0x78000`.
- No hardware step runs without Andre's explicit approval for that step.

## Review Focus

1. **Wrong native image:** `installer/Makefile` packs `../output/rtlplayground.bin`, the symlink to the last firmware build. Building the installer after a build for another machine would pack the wrong board's firmware. Task 1 adds a Make check that the image contains `TP-Link TL-SG108PE V5` when `MACHINE=TPLINK_TL_SG108PE_V5`.
2. **Reset that returns:** if the RTL8370N register poll ends before the reset latches, `installer()` returns into the startup code. Task 1 adds `for (;;);` after the RTL836x reset.
3. **Stock loader rejects the package:** checked only by the hardware upload in Task 2; Task 1 verifies the header checksum and payload sum against the stock image's own values with the same formula.
4. **Configuration embedded in the image:** the build embeds `config.txt` unless `CONFIG=` is set. Task 2 states which configuration the test image carries.

The startup calls `installer()` makes before copying (`CKCON = 0`, `MAP_FLASH_PAGE_TO_XDATA = 0`, `SFR_b9`/`SFR_ba`, `IE`/`EIE = 0`, `setup_serial_timer1()`, `flash_init(0)`) match RTLPlayground's own `main()` and `check_and_flash_update_image()` on the TL-SG108PE V5, which run on `.5`; they need no change. The serial baud rate is wrong at 62.5 MHz, but output is optional and `write_char()` only polls `TI`.

---

### Task 1: Build the installer for the TL-SG108PE V5

**Files:**
- Modify: `installer/Makefile`
- Modify: `installer/installer.c:384` (the reset at the end of `installer()`)
- Modify: `README.md` installer section (lines 100-106 and 168-191): document `make -C installer MACHINE=TPLINK_TL_SG108PE_V5`
- Modify: `CHANGELOG.md`: one bullet under the TL-SG108PE V5 port entry

**Interfaces:**
- Produces: `make -C installer MACHINE=TPLINK_TL_SG108PE_V5` writes `installer/output/rtlplayground_oem_upgrade.bin`.

- [ ] **Step 1: Record the default build output**

Run: `make -C installer && cp installer/output/rtlinstaller.ihx /tmp/rtlinstaller-before.ihx && cp installer/output/rtlplayground_oem_upgrade.bin /tmp/oem-before.bin`
(through `/tmp/port8370/dmake -C installer`; the native image symlink must point at any built image.)
Expected: both files exist.

- [ ] **Step 2: `installer/Makefile`**

When `MACHINE` is set, add `-DMACHINE_NAME=$(MACHINE) -I.. -I../machine` to `CC_FLAGS`. When `MACHINE=TPLINK_TL_SG108PE_V5`, pass `-m 50451085 -r 332256ff` to `updatebuilder`, and fail the package rule unless `grep -q 'TP-Link TL-SG108PE V5' ../$(BUILDDIR)/rtlplayground.bin`.

- [ ] **Step 3: `installer/installer.c` reset**

Replace `REG_SET(RTL837X_REG_RESET, 1);` with the `#if SOC_FAMILY == RTL836X` / `#else` pair from `rtlplayground.c:1295-1299`; in the RTL836x branch, follow the reset with `for (;;);`.

- [ ] **Step 4: Default build unchanged**

Run: `make -C installer clean && make -C installer && cmp installer/output/rtlinstaller.ihx /tmp/rtlinstaller-before.ihx && cmp installer/output/rtlplayground_oem_upgrade.bin /tmp/oem-before.bin`
Expected: no output, exit 0.

- [ ] **Step 5: Build the TL-SG108PE V5 firmware and installer**

Run: the TL-SG108PE V5 firmware build (`MACHINE=TPLINK_TL_SG108PE_V5 RTL8370_DUMP=/Users/andre/sg108pe-dump/pre-blink.bin`), then `make -C installer clean && make -C installer MACHINE=TPLINK_TL_SG108PE_V5`.
Expected: `installer/output/rtlplayground_oem_upgrade.bin` is `0x84026` bytes.

- [ ] **Step 6: Wrong-image refusal**

Run: build the firmware for `MOKERLINK_ZX_SWTGW218AS`, then `make -C installer MACHINE=TPLINK_TL_SG108PE_V5`.
Expected: the package rule fails; no `.bin` is written.

- [ ] **Step 7: Package layout and checksums (throwaway script, not committed)**

With Python, check on the Step 5 package:
- `bytes[0:4] == 50 45 10 85`, `bytes[0x10:0x14] == 33 22 56 ff`, and `bytes[0x4012:0x4026] == bytes[0:0x14]`;
- the header checksum `sum(h[0:8]) + sum(h[12:20])` and the payload sum (bytes from `0x14`, with `[0x4012, 0x4026)` counted as 20 × `0xff`) match the stored values;
- the same two formulas reproduce the stored values of `~/Downloads/SG108PE(UN) 5.0_1.0.1 Build 20260330 Rel.42395_up.bin`;
- `bytes[0x4026:0x84026]` equals the native image.
Expected: all checks pass.

- [ ] **Step 8: Check the reset in the generated code**

Run: `grep -n -B2 -A12 '0x1322\|#0x13' installer/output/installer.asm`
Expected: the RTL836x `REG_SET` loads `0x00` into `SFR_DATA_24` and `0x01` into `SFR_DATA_16`, calls `_reg_write` with `0x1322`, then loops on itself.

- [ ] **Step 9: Commit**

```bash
git add installer/Makefile installer/installer.c README.md CHANGELOG.md
git commit -m "installer: build the OEM installer for the TL-SG108PE V5"
```

### Task 2: Install on the stock unit (hardware, approval-gated)

**Files:**
- Modify: `CHANGELOG.md` (the bullet from Task 1): record the hardware result, including anything that failed.

- [ ] **Step 1: Back up the stock unit** (approval: clip read)

With the unit unpowered and the clip on U3 at 3.3 V: `flashrom -p ch341a_spi -c EN25QH16 -r stock2-a.bin` and `-r stock2-b.bin`.
Expected: identical SHA-256; the file starts with `00 40`; bytes at `0x1fc000` are a MAC different from `.5`'s `3c:78:95:1f:6f:c2`.

- [ ] **Step 2: Choose the embedded configuration**

Ask Andre which configuration the test image embeds (`config.txt` default or a per-unit file with a static address). Rebuild firmware and installer with it.

- [ ] **Step 3: Upload** (approval: upload)

Remove the clip, power the unit, open the stock web interface, run the firmware upgrade with `rtlplayground_oem_upgrade.bin`.
Expected: the switch reboots and answers at the configured address with RTLPlayground's web interface.

- [ ] **Step 4: Check the unit**

Read `/information.json` and `/status.json`.
Expected: `hw_ver` is `TP-Link TL-SG108PE V5`; `mac_address` equals the backup's bytes at `0x1fc000`; linked ports show their links.

- [ ] **Step 5: Read back the identity area** (approval: clip read)

Unpowered, clip on: `flashrom -p ch341a_spi -c EN25QH16 -r stock2-after.bin`.
Expected: `stock2-after.bin[0x1fc000:]` equals `stock2-a.bin[0x1fc000:]`; `stock2-after.bin[0:0x78000]` equals the native image's first `0x78000` bytes.

- [ ] **Step 6: On failure, restore** (approval: clip write)

Write `stock2-a.bin` back with flashrom and verify.

- [ ] **Step 7: Commit the recorded result**

```bash
git add CHANGELOG.md
git commit -m "docs: record TL-SG108PE V5 install from stock firmware"
```
