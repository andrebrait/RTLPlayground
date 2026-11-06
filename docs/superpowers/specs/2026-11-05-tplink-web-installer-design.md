# TL-SG108PE V5: install RTLPlayground from the stock web interface

Status: design approved in chat on 2026-11-05; this document awaits review.

## Goal

A TL-SG108PE V5 running TP-Link's stock firmware (1.0.x) can be switched to
RTLPlayground by uploading one file through the stock web interface's firmware
upgrade page. No case opening, clip or programmer is needed for a successful
install. The clip remains the recovery path when an install fails.

## Non-goals

- Translating stock settings (IP address, passwords, VLANs) into RTLPlayground
  configuration. The installed firmware starts with its built-in defaults.
- Returning from RTLPlayground to the stock firmware through a web upload.
- Supporting other TP-Link models or hardware revisions.
- Changing the existing RTL837x OEM installer (`installer/`).

## Evidence this design relies on

From static analysis of the stock boot code in one 1.0.1 flash dump and the
three official 1.0.x update images. The full notes are in
`notes/web-upgrade-research.md` of the `docs/tl-sg108pe-v5-firmware` worktree.

1. The stock web interface switches into TP-Link's loader (`/fupgrade0.cgi`).
   The loader receives the upload (`/httpupg.cgi?cmd=fw_upgrade`) in chunks and
   writes each chunk to flash as it arrives.
2. Before writing, the loader checks only the header magic (`0x50451085`) and
   the header checksum. It checks no RSA signature and no payload checksum.
3. The loader maps update-file byte `F` to flash: `F` in `[0x14, 0x3012)` goes
   to `0x1002 + (F - 0x14)`; `F >= 0x3012` goes to `0x1c000 + (F - 0x3012)`.
   It refuses writes to `[0, 0x1000)` and `[0x4000, 0x1c000)`, and nothing else.
4. On the next boot, TP-Link's boot code verifies the header magic, the header
   checksum and the 32-bit byte sum of the whole declared payload, then jumps to
   CODE `0x1000`. A failed check enters the loader again.
5. The boot prefix `00 40` makes the chip load 16 KiB of common code into code
   RAM. Code in CODE `0x1000-0x3fff` keeps running while its backing flash is
   erased, provided it calls nothing outside that region.
6. TP-Link's boot code forwards only the Timer 0 interrupt.
7. The identity block at `0x1fc000` holds the factory MAC address and a
   per-unit signature; TP-Link's settings are at `0x1ff000`. The stock loader's
   protection table does not cover them.

Items 4 and 5 are static findings. Neither has been exercised with a custom
image on hardware.

## Design

### 1. Package builder: `tools/tplink_upgrade.py`

Input: an RTLPlayground TL-SG108PE V5 image (exactly `0x80000` bytes, starting
with `00 40`, valid native CRC16 at `0x7fffe`) and the linked installer code.

Output: an update file with the exact stock length `0x10f354` and this layout:

| Update file | Flash after the stock loader writes it | Content |
| --- | --- | --- |
| `[0x0, 0x14)` | not written | header |
| `[0x14, 0x3012)` | `[0x1002, 0x4000)` | installer code, then `0xff` |
| `[0x4012, 0x4026)` | `[0x1d000, 0x1d014)` | copy of the header |
| `[0x67012, 0x77012)` | `[0x80000, 0x90000)` | `0xff` |
| `[0x77012, 0xf7012)` | `[0x90000, 0x110000)` | RTLPlayground image |
| all other bytes | as mapped | `0xff` |

Header fields, big-endian:

```
[0x00:0x04] magic           0x50451085
[0x04:0x08] payload length  file length - 0x14
[0x08:0x0c] header checksum sum(header[0:8]) + sum(header[12:20])
[0x0c:0x10] payload sum     sum(file[0x14:]), with file[0x4012:0x4026] counted as 20 x 0xff
[0x10:0x14] marker          copied from the newest stock image (0x332256ff)
```

The `0xff` range at `0x80000` matters: RTLPlayground's own updater
(`check_and_flash_update_image()`) treats `00 40` at `0x80000` as a pending
update. Leaving it blank prevents a second reflash after the first native boot.

The builder refuses an input image of the wrong length, prefix or CRC, or one
that does not contain the board's model string `TP-Link TL-SG108PE V5`, and
an installer that does not fit `[0x14, 0x3012)`.

### 2. Installer: `installer/tplink/`

A small 8051 program linked at CODE `0x1000`, using the existing
`installer/crtstart.asm` entry relocation. It runs once, started by TP-Link's
boot code.

Rules:

- Interrupts disabled for the whole run; no UART output; no calls outside CODE
  `0x1000-0x3fff`.
- Only the flash controller SFRs are used. They are shared with the stock code
  and with RTLPlayground's `rtl837x_flash.c`.
- Switch registers use the RTL8370N 16-bit access (`0xa2/a3` address, `0xa4/a5`
  data, `0xa6/a7` result) with the stock settling delays.
- Reset is `write16(0x1322, 0x0001)`, followed by an endless loop.
- Every flash wait has a bounded timeout.

Sequence:

1. **Preflight.** Verify the staged image at `0x90000`: prefix `00 40` and
   native CRC16 over `0x80000` bytes. Verify the six bytes at `0x1fc000` form
   a globally administered unicast MAC. On any failure, erase only flash
   sector `0x1d000`, which holds the embedded header, then reset. The next
   boot then fails the stock checksum and enters TP-Link's loader, which
   accepts a fresh upload. Without this, the stock boot code would start the
   failing installer again on every power cycle.
2. **Copy sector by sector,** 4 KiB at a time: read source sector into XDATA,
   erase destination, program, read back, compare. Order:

   | Step | Destination | Recovery if power fails during the step |
   | --- | --- | --- |
   | a | `[0x1c000, 0x80000)` | stock loader still boots; checksum fails, loader accepts a new upload |
   | b | `[0x1000, 0x4000)` | same as a |
   | c | `[0x4000, 0x1c000)` | clip only: TP-Link boot banks are gone |
   | d | `[0x0, 0x1000)` | clip only |

3. On any timeout or read-back mismatch: stop in an endless loop. No reset, no
   further erase, so the source copy at `0x90000` stays intact.
4. After step d, reset. RTLPlayground boots from the new image.

The installer never erases or programs at or above `0x80000`, and so it
cannot touch the staged source, the identity block or TP-Link's settings.

The stale staged image at `0x90000` and leftover stock data above it are not
erased. RTLPlayground ignores them, and its own web upgrade later overwrites
`0x80000-0xfffff` normally.

### 3. Board

`machine/TPLINK_TL_SG108PE_V5.c` already sets `.mac_flash_offset = 0x1fc000`, so
the installed firmware uses the unit's factory MAC. No change is needed.

## Testing

### Host tests

- **Builder:** for each official image available locally, the builder's
  checksum functions reproduce the stored header checksum and payload sum in
  both header copies. A generated package must map the native image to flash
  `0x90000` and pass the same checks. Without local stock images, the test
  uses a synthetic image and states that the stock comparison was skipped.
- **Installer:** compiled for the host against a simulated 2 MiB flash
  (`test/test_tplink_installer.c`). Cases:
  - a successful run produces the expected flash below `0x80000` and leaves
    everything from `0x80000` up unchanged, including the identity block;
  - a corrupt staged image, a bad CRC or a blank MAC erases only sector
    `0x1d000` and nothing else;
  - a read-back mismatch or timeout injected in each step stops the run with no
    further erase;
  - the sector order is a, b, c, d.

### Hardware test on the stock unit

Every step needs its own approval.

1. Read the stock unit's flash twice with the clip; the two hashes must match.
2. Upload the package through the stock web interface.
3. Confirm RTLPlayground boots, answers on the network with the unit's own
   factory MAC, and that ports, LEDs and PoE work.
4. Read the flash with the clip again. `[0x1fc000, 0x200000)` must match the
   backup byte for byte.
5. On failure, restore with the clip: the unit's own backup for the identity
   area, `.5`'s image or the backup for the rest.

## Risks

1. **Power loss during steps c and d** leaves no boot code. Only the clip
   recovers. The window is the time to rewrite 100 KiB, a few seconds.
2. **Code-RAM residency is a static assumption.** If the installer executes
   from flash rather than from loaded code RAM, erasing step b destroys it.
   The host test cannot detect this; the hardware test with the clip ready can.
3. **Boot-code revision:** the analysis covers one 1.0.1 dump. A unit with a
   different boot code might map or check the upload differently.
4. **Watchdog:** no watchdog was found in the stock boot path, but none was
   ruled out either. A watchdog reset mid-copy behaves like a power loss.

## Open questions

- Whether the stock web server accepts the package at all is untested. The
  file has the exact stock length and valid checksums, so it should look like a
  normal update.
- How long steps a to d take on the real flash.
