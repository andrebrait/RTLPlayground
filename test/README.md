# Host unit-test harness

Compile and test individual firmware translation units on the build host with
**gcc + AddressSanitizer + UBSan** — no SDCC, no flashing, no hardware. This is
the fast inner loop for the logic-level bugs in `../../NOTES/08-findings.md`
(parser, line editor, HTTP header, DHCP): edit → `make` → see red/green in
seconds instead of a minutes-long flash-and-reboot cycle.

## Run
```
cd test
make            # build + run all tests (ASan/UBSan on)
make clean
```
Exit status is non-zero if any check fails, a sanitizer trips, or the watchdog
fires — so it drops straight into CI.

## How it works
- **`sdcc_shim.h`** (force-included) erases SDCC 8051 keywords (`__xdata`,
  `__code`, `__banked`, …) so firmware sources compile under host gcc. It changes
  no logic — the code under test is byte-for-byte the firmware source.
- **`hw_mock.c` / `hw_mock.h`** simulate the ASIC edge: a flat register file
  behind `reg_read`/`reg_write` and the `SFR_DATA_*` registers, with a table
  engine that performs a VLAN or L2 operation the moment `TBL_CTRL` is written,
  fetches a MIB counter on `STAT_GET` and drops entries on `L2_TBL_FLUSH_CTRL`.
  Nothing ever reports busy, so a polling loop runs once. The header states the
  VLAN and L2 entry layouts independently of the firmware, so a test can hold
  what `rtl837x_port.c` writes against what `page_impl.c` reads.
- **`env_tables.c`** carries the globals and leaf calls those two modules link
  against; **`stub/`** stands in for the two headers only a firmware build
  generates, so the harness needs no SDCC build first.
- **`support.c` / `support.h`** mock the hardware edges: the 16-byte serial ring
  (`sbuf`), the command/history buffers, and the character-output sink
  (`write_char` etc.). Buffers are sized **exactly** as on target, so ASan
  redzones catch the same off-by-one overflows the 8051 hits.
- Builds define **`RTLP_HOST_TEST`**, which hides the firmware's libc-named
  prototypes (`memset`/`strlen`/…) in `rtl837x_common.h` so they don't clash with
  glibc. Argument order matches libc, so on-host callers transparently use the C
  library. This guard is compiled out of normal firmware builds — zero on-target
  effect.
- A **SIGALRM watchdog** (`watchdog_arm`) turns an infinite-loop bug into a fast
  test failure instead of a hung runner.

## Current coverage
| Test binary | TU under test | Findings exercised |
|-------------|---------------|--------------------|
| `test_cmd_editor` | `cmd_editor.c` | **C4** — full-line hang + `cmd_buffer` 1-byte overflow; basic entry & backspace regressions |
| `test_port_tables` | `rtl837x_port.c` | VLAN entry layout and round trip, PVID register sharing, static multicast and management entries, per-port flush, trunk membership and hash seed |
| `test_page_json` | `httpd/page_impl.c` + `rtl837x_port.c` | `/vlan.json`, `/vlanlist`, `/l2.json` (walk, wrap marker, paging inside `outbuf`), 64-bit counters in `/status.json` and `/counters.json` |
| `test_httpd_tx` | `httpd/httpd.c` + `uip/uip.c` | a GET of a static file against a client that moves its receive window: ACK accounting, continuation chunks out of flash, retransmission |
| `test_lacp` | `rtl837x_lacp.c` | state machines against a simulated 802.3ad partner: convergence, mis-cabling, expiry, re-election, two groups, delivery by trap |
| `test_golden` | `cmd_parser.c` + `httpd/page_impl.c` + `rtl837x_port.c`, bandwidth, storm, trap | every JSON page and a set of CLI commands on one register fixture, byte-compared with `golden/*.txt`. Subsystems not linked (PHY, SFP, flash, IGMP, STP, LACP, ...) are stubs in `golden_env.c` that print `[stub:<name>]`. `GOLDEN_UPDATE=1 build/test_golden` rewrites the goldens; review the diff before committing it |
| `test_platform_seam` | `cmd_parser.c` + `cmd_editor.c` + `httpd/page_impl.c` | the platform seam, see below |

## The platform seam
`cmd_parser.c`, `cmd_editor.c` and `httpd/page_impl.c` are shared code: they reach
the hardware only through `../platform.h` (plus their own headers, `httpd.h`, C
headers and the hardware-free module headers such as `rtl837x_stp.h`).
`make` runs `test_platform_seam` (`make seam_check` runs just it). It compiles
copies of the three files with `seam_stub/` first on the include path, where
`rtl837x_regs.h`, `rtl837x_sfr.h` and `8051.h` are `#error`, and links them with
`platform_mock.c` alone, so a register access, or a call to a function the platform does not
declare, fails to compile or link. A header that is included through another header slips past
the stubs, so `gcc -MM` also lists every header the copies reach and the check fails
if one of them is `rtl837x_regs.h`, `rtl837x_sfr.h`, `8051.h`, `rtl837x_common.h` (the `reg_*`/`phy_*`/`sds_*`
primitives), `rtl837x_port.h`, `rtl837x_phy.h`, `rtl837x_flash.h`, `sfp.h` or `phy.h` (register-level declarations). `sfp_pins_last` and the
other `extern` variables in `platform.h` are state the platform provides, not register access:
shared code may read and write them.

To add a platform function: declare it in `platform.h` (a `__banked` function takes the first
argument in the usual way and every further one `__xdata`; a result wider than a byte comes
back in `port_buf`), implement it in an `rtl837x_*.c` module, add a do-nothing or fixed-value
definition to `platform_mock.c`, and, if its output matters to a page, a check to
`test_platform_seam.c`. Never include a register header from a shared file; if a constant is
needed, move it into `platform.h`.

## Adding a test for another module
1. Write `test_<module>.c` with `main()` driving the module's entry points and
   `CHECK(cond, msg)` assertions.
2. Add any missing mocked symbols to `support.c` (only what the linker asks for).
3. Add a target in the `Makefile` listing `test_<module>.c support.c ../<module>.c`.
4. If the module calls a firmware libc-name (`memcpy`/`strcpy`/…), it will use the
   C library on host (compatible arg order); provide a `fw_*`-style mock only if
   the semantics differ in a way the test cares about.

Good next targets (pure logic, high finding density): `cmd_parser.c` (C6/C8),
the HTTP header parser in `httpd/httpd.c` (S1), `dhcp.c` (S5).
