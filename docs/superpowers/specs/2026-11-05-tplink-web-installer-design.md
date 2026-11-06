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

TP-Link's update format is the Realtek OEM format that RTLPlayground's
existing `installer/` already produces for RTL837x boards: payload at
`0x1002` and `0x1c000`, header copy at `0x1d000`, the same two checksums. The
TL-SG108PE V5 installer therefore reuses `installer/` unchanged in structure:
`installer.c`, `crtstart.asm`, `updatebuilder.c` and the `Makefile`, producing
`installer/output/rtlplayground_oem_upgrade.bin` as for other boards.

Only what the TP-Link loader and the RTL8370N require changes:

| Change | Why |
| --- | --- |
| `updatebuilder -m 50451085 -r 332256ff` | TP-Link's header magic and trailing marker; both are existing command-line options |
| Installer reset through the RTL8370N 16-bit register write to `0x1322` | `REG_SET(RTL837X_REG_RESET, 1)` writes a 32-bit value to an RTL837x address |
| Build selection for the TL-SG108PE V5 in `installer/Makefile` | The installer is RTL837x-only today |

Everything else follows the existing installer: the native image is carried
directly after the header copy (flash `0x1d014`), and the installer copies 120
sectors (`0x78000` bytes) from there to flash `0`, ascending, then resets.
It never writes at or above `0x78000`, so the identity block and TP-Link's
settings stay untouched. The board already reads its MAC from `0x1fc000`.

The remaining RTL837x-specific startup in `installer.c` (clock, flash
controller, serial) is kept where RTLPlayground's own startup on the
TL-SG108PE V5 already does the same; the implementation plan checks each call.

After installation, flash `0x80000` holds byte `0x62fec` of the staged image.
RTLPlayground's updater treats `00 40` there as a pending update; the build
checks those two bytes are not `00 40`.

## Testing

No new host test: the changes are build flags and one register write.

Hardware test on the stock unit, each step with its own approval:

1. Read the stock unit's flash twice with the clip; the two hashes must match.
2. Upload `rtlplayground_oem_upgrade.bin` through the stock web interface.
3. Confirm RTLPlayground boots, answers on the network with the unit's own
   factory MAC, and that ports, LEDs and PoE work.
4. Read the flash with the clip again. `[0x1fc000, 0x200000)` must match the
   backup byte for byte.
5. On failure, restore with the clip: the unit's own backup for the identity
   area, `.5`'s image or the backup for the rest.

## Risks

1. **No recovery without the clip once copying starts.** As in the existing
   installer, sector 0 is erased first, so a power loss or a failed write
   during the copy leaves no boot code.
2. **Code-RAM residency is a static assumption.** If the installer executes
   from flash rather than from loaded code RAM, the first erase destroys it.
   The existing RTL837x installer relies on the same behavior.
3. **Boot-code revision:** the analysis covers one 1.0.1 dump. A unit with a
   different boot code might map or check the upload differently.

## Open questions

- Whether the stock web server accepts the package is untested.
