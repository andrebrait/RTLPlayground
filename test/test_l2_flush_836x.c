/*
 * test_l2_flush_836x.c - port_l2_forget_port on the RTL836x. rtl836x_port.c is compiled
 * unmodified; only the register controller is modelled. A write to FORCE_FLUSH (chip ports
 * 0-7) or FORCE_FLUSH1 (chip ports 8-10) latches a flush command with the mode and type that
 * L2_FLUSH_CTRL2 holds at that moment. The command's busy flag is reported for a set number of
 * status reads, and the L2 table loses the matching entries only when the flag clears, so a
 * driver that returns early leaves entries behind.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rtl837x_common.h"
#include "machine.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_port.h"

#if SOC_FAMILY != RTL836X
#error "this test is for the RTL836x build"
#endif

/* Register names and masks are the contract; the values are restated here, not taken from the header */
_Static_assert(MACHINE_MAX_PORT == 7 && CPU_PORT == 9, "front ports 0-7, the NIC at logical port 9");
_Static_assert(RTL836X_REG_VLAN_MSTI0_CTRL0 == 0x0a00 && RTL836X_REG_VLAN_MSTI0_CTRL1 == 0x0a01,
	       "MSTI0 port state registers");
_Static_assert(RTL836X_REG_FORCE_FLUSH1 == 0x0a35 && RTL836X_REG_FORCE_FLUSH == 0x0a36 &&
	       RTL836X_REG_L2_FLUSH_CTRL1 == 0x0a37 && RTL836X_REG_L2_FLUSH_CTRL2 == 0x0a38,
	       "L2 flush registers");
_Static_assert(FORCE_FLUSH_BUSY_MASK == 0xff00 && FORCE_FLUSH1_BUSY_MASK == 0x0038 &&
	       LUT_FLUSH_MODE_MASK == 0x0003 && LUT_FLUSH_TYPE_MASK == 0x0004,
	       "flush busy flags and CTRL2 mode and type fields");

/* The SFRs the register primitives use; sdcc_shim.h leaves the headers' declarations extern */
volatile uint8_t SFR_DATA_0, SFR_DATA_8, SFR_DATA_16, SFR_DATA_24;
volatile uint16_t SFR_DATA_U16;

#define EXP_BUSY_LOW		0xff00	/* FORCE_FLUSH: busy of chip ports 0-7 */
#define EXP_BUSY_EXT		0x0038	/* FORCE_FLUSH1: busy of chip ports 8-10 */
#define EXP_CTRL2_MODE		0x0003	/* 0: port */
#define EXP_CTRL2_TYPE		0x0004	/* 1: static entries are flushed too */
#define WRITE_TRACE_MAX		64
/* A bounded wait reads two status registers per poll; any bound up to this many reads passes */
#define STATUS_READS_MAX	1024

static int failures;
#define CHECK(cond, name) do { \
	if (cond) printf("PASS  %s\n", name); \
	else { printf("FAIL  %s\n", name); failures++; } \
} while (0)

/* ---------- register controller and flush engine ---------- */

static uint16_t regfile[0x4000];
static uint16_t stuck[0x4000];		/* status bits that stay set whatever the engine does */

struct wr {
	uint16_t addr, value;
};
static struct wr wr_trace[WRITE_TRACE_MAX];
static unsigned ntrace, reg_writes;
static unsigned status_reads, busy_reports, diagnostics;
static unsigned writes_in_flight, busy_bit_writes, mode_cmds;
static unsigned flush_busy_reads;	/* busy reports the next command makes before it completes */

/* L2 table: entries on chip ports, dynamic or static */
struct l2_entry {
	bool valid, dynamic;
	uint8_t chip;
};
static struct l2_entry l2[16];
static unsigned nl2;

/* The flush command in flight: the chip ports it flushes, the busy flag in the status register
 * it was written to, and the mode and type latched from CTRL2 when it was written */
static bool cmd_active, cmd_dynamic_only;
static uint16_t cmd_reg, cmd_chips, cmd_busy;
static unsigned cmd_mode, cmd_reads_left;

static void flush_complete(void)
{
	cmd_active = false;
	for (unsigned i = 0; i < nl2; i++)
		if (l2[i].valid && ((cmd_chips >> l2[i].chip) & 1) && (l2[i].dynamic || !cmd_dynamic_only))
			l2[i].valid = false;
}

static void flush_issue(uint16_t reg, uint16_t chips, uint16_t busy)
{
	uint16_t ctrl2 = regfile[RTL836X_REG_L2_FLUSH_CTRL2];

	cmd_active = true;
	cmd_reg = reg;
	cmd_chips = chips;
	cmd_busy = busy;
	cmd_mode = ctrl2 & EXP_CTRL2_MODE;
	cmd_dynamic_only = !(ctrl2 & EXP_CTRL2_TYPE);
	cmd_reads_left = flush_busy_reads;
	if (cmd_mode)
		mode_cmds++;
}

void reg_write(uint16_t addr)
{
	uint16_t v = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;

	reg_writes++;
	if (ntrace < WRITE_TRACE_MAX) {
		wr_trace[ntrace].addr = addr;
		wr_trace[ntrace].value = v;
		ntrace++;
	}
	if (cmd_active)
		writes_in_flight++;
	if (addr == RTL836X_REG_FORCE_FLUSH) {
		if (v & EXP_BUSY_LOW)
			busy_bit_writes++;
		if (v & 0x00ff)
			flush_issue(addr, v & 0x00ff, (uint16_t)((v & 0x00ff) << 8));
	} else if (addr == RTL836X_REG_FORCE_FLUSH1) {
		if (v & EXP_BUSY_EXT)
			busy_bit_writes++;
		if (v & 0x0007)
			flush_issue(addr, (uint16_t)((v & 0x0007) << 8), (uint16_t)((v & 0x0007) << 3));
	}
	regfile[addr] = v;
}

void reg_read(uint16_t addr)
{
	uint16_t v = regfile[addr] | stuck[addr];

	if (addr == RTL836X_REG_FORCE_FLUSH || addr == RTL836X_REG_FORCE_FLUSH1) {
		status_reads++;
		if (cmd_active && addr == cmd_reg) {
			if (cmd_reads_left) {
				cmd_reads_left--;
				busy_reports++;
				v |= cmd_busy;
			} else {
				flush_complete();
			}
		}
	}
	SFR_DATA_U16 = v;
	SFR_DATA_8 = v >> 8;
	SFR_DATA_0 = v;
}

void print_string(const char *s)
{
	(void)s;
	diagnostics++;
}

/* ---------- scenario helpers ---------- */

static void reset_model(void)
{
	memset(regfile, 0, sizeof regfile);
	memset(stuck, 0, sizeof stuck);
	memset(l2, 0, sizeof l2);
	memset(wr_trace, 0, sizeof wr_trace);
	cmd_active = false;
	ntrace = reg_writes = status_reads = busy_reports = diagnostics = 0;
	writes_in_flight = busy_bit_writes = mode_cmds = 0;
	nl2 = 0;
	flush_busy_reads = 1;
}

static unsigned l2_add(unsigned chip, bool dynamic)
{
	l2[nl2] = (struct l2_entry){ .valid = true, .dynamic = dynamic, .chip = chip };
	return nl2++;
}

static unsigned trace_count(uint16_t addr)
{
	unsigned n = 0;

	for (unsigned i = 0; i < ntrace; i++)
		if (wr_trace[i].addr == addr)
			n++;
	return n;
}

static bool trace_has(uint16_t addr, uint16_t value)
{
	for (unsigned i = 0; i < ntrace; i++)
		if (wr_trace[i].addr == addr && wr_trace[i].value == value)
			return true;
	return false;
}

/* True when every write of the call went to one of the three named registers */
static bool trace_only(uint16_t a, uint16_t b, uint16_t c)
{
	for (unsigned i = 0; i < ntrace; i++)
		if (wr_trace[i].addr != a && wr_trace[i].addr != b && wr_trace[i].addr != c)
			return false;
	return true;
}

/* ---------- scenarios ---------- */

/* Dynamic entries of the one port go; its static entry and the other ports keep theirs */
static void scenario_dynamic_entries_of_one_port(void)
{
	reset_model();
	regfile[RTL836X_REG_L2_FLUSH_CTRL2] = 0xa0a5;	/* mode 1, static included; bits 15, 13, 7, 5 are not the flush's */

	unsigned target = l2_add(0, true);
	unsigned target2 = l2_add(0, true);
	unsigned static0 = l2_add(0, false);
	unsigned other1 = l2_add(1, true);
	unsigned other7 = l2_add(7, true);
	unsigned cpu = l2_add(10, true);

	port_l2_forget_port(0);

	CHECK(!l2[target].valid && !l2[target2].valid, "A1 both dynamic entries of port 0 are flushed");
	CHECK(l2[static0].valid, "A2 the static entry of port 0 stays");
	CHECK(l2[other1].valid && l2[other7].valid && l2[cpu].valid,
	      "A3 dynamic entries of chip ports 1, 7 and 10 stay");
	CHECK(regfile[RTL836X_REG_L2_FLUSH_CTRL2] == 0xa0a0, "A4 CTRL2 mode and type cleared, its other bits kept");
	CHECK(trace_has(RTL836X_REG_FORCE_FLUSH, 0x0001) && trace_count(RTL836X_REG_FORCE_FLUSH1) == 0,
	      "A5 port 0 is bit 0 of FORCE_FLUSH and FORCE_FLUSH1 is not written");
	CHECK(trace_only(RTL836X_REG_L2_FLUSH_CTRL1, RTL836X_REG_L2_FLUSH_CTRL2, RTL836X_REG_FORCE_FLUSH)
	      && busy_bit_writes == 0 && mode_cmds == 0 && writes_in_flight == 0,
	      "A6 only the flush registers are written, with no busy bits, in port mode, and none while busy");
	CHECK(diagnostics == 0, "A7 a clean flush prints nothing");
}

/* Each logical front port 0-7 flushes its own chip port and no other */
static void scenario_each_front_port(void)
{
	bool ok = true;

	for (unsigned p = 0; p <= MACHINE_MAX_PORT; p++) {
		reset_model();
		unsigned own = l2_add(p, true);
		unsigned other = l2_add((p + 1) % 8, true);

		port_l2_forget_port(p);
		ok = ok && !l2[own].valid && l2[other].valid && trace_has(RTL836X_REG_FORCE_FLUSH, PORT_BIT(p));
	}
	CHECK(ok, "B1 each logical front port 0-7 flushes its own chip port and no other");
}

/* Logical CPU port 9 is chip port 10, which lives in FORCE_FLUSH1 bit 2 with busy flag 0x20 */
static void scenario_cpu_port(void)
{
	reset_model();
	regfile[RTL836X_REG_L2_FLUSH_CTRL2] = EXP_CTRL2_TYPE;	/* static entries included: must not be flushed */

	unsigned cpu_dyn = l2_add(10, true);
	unsigned cpu_static = l2_add(10, false);
	unsigned front = l2_add(0, true);

	port_l2_forget_port(CPU_PORT);

	CHECK(!l2[cpu_dyn].valid && l2[cpu_static].valid && l2[front].valid,
	      "C1 logical CPU port 9 flushes the dynamic entries of chip port 10 only");
	CHECK(trace_has(RTL836X_REG_FORCE_FLUSH1, 0x0004) && trace_count(RTL836X_REG_FORCE_FLUSH) == 0,
	      "C2 chip port 10 is bit 2 of FORCE_FLUSH1 (0x0a35) and FORCE_FLUSH is not written");
	CHECK(trace_only(RTL836X_REG_L2_FLUSH_CTRL1, RTL836X_REG_L2_FLUSH_CTRL2, RTL836X_REG_FORCE_FLUSH1)
	      && busy_bit_writes == 0 && mode_cmds == 0 && busy_reports == 1 && diagnostics == 0,
	      "C3 the wait was on FORCE_FLUSH1's busy flag, with no busy bits written");
}

/* Logical 8, 10 and 255 have no chip port to flush: refused before any write */
static void scenario_refused_ports(void)
{
	static const uint8_t refused[] = { 8, 10, 255 };
	bool ok = true;

	for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		reset_model();
		unsigned lo = l2_add(0, true);
		unsigned ext = l2_add(8, true);
		unsigned cpu = l2_add(10, true);

		port_l2_forget_port(refused[i]);
		ok = ok && reg_writes == 0 && l2[lo].valid && l2[ext].valid && l2[cpu].valid;
	}
	CHECK(ok, "D1 logical ports 8, 10 and 255 are refused: no register written, no entry removed");
}

/* A flush on another front port stays busy: refuse, write nothing, bounded wait, diagnostic */
static void scenario_busy_front_port_blocks(void)
{
	reset_model();
	stuck[RTL836X_REG_FORCE_FLUSH] = (uint16_t)(PORT_BIT(3) << 8);	/* chip port 3 busy, never clears */
	unsigned target = l2_add(0, true);

	port_l2_forget_port(0);

	CHECK(reg_writes == 0 && l2[target].valid, "E1 a flush busy on another port: nothing written, entry kept");
	CHECK(status_reads <= STATUS_READS_MAX && diagnostics >= 1,
	      "E2 the refusal is bounded and reported");
}

/* The same for the extended register: chip port 9 busy in FORCE_FLUSH1 blocks logical CPU port 9 */
static void scenario_busy_cpu_port_blocks(void)
{
	reset_model();
	stuck[RTL836X_REG_FORCE_FLUSH1] = 0x0010;	/* chip port 9 busy, never clears */
	unsigned target = l2_add(10, true);

	port_l2_forget_port(CPU_PORT);

	CHECK(reg_writes == 0 && l2[target].valid && status_reads <= STATUS_READS_MAX && diagnostics >= 1,
	      "E3 an extended-port flush busy: nothing written, bounded wait, reported");
}

/* A flush issued earlier is still busy: it completes before any control changes, then this port flushes */
static void scenario_flush_in_flight_completes_first(void)
{
	reset_model();
	flush_busy_reads = 3;
	unsigned prior = l2_add(5, true);
	unsigned target = l2_add(0, true);

	cmd_active = true;
	cmd_reg = RTL836X_REG_FORCE_FLUSH;
	cmd_chips = PORT_BIT(5);
	cmd_busy = (uint16_t)(PORT_BIT(5) << 8);
	cmd_dynamic_only = true;
	cmd_mode = 0;
	cmd_reads_left = 3;

	port_l2_forget_port(0);

	CHECK(!l2[prior].valid && !l2[target].valid,
	      "F1 the flush in flight completes first; then this port is flushed");
	CHECK(writes_in_flight == 0 && !cmd_active, "F2 no register written while the earlier flush was busy");
}

/* Completion busy for five polls: the call waits it out before returning */
static void scenario_completion_waited_out(void)
{
	reset_model();
	flush_busy_reads = 5;
	unsigned target = l2_add(2, true);
	unsigned keep = l2_add(2, false);

	port_l2_forget_port(2);

	CHECK(!l2[target].valid && l2[keep].valid, "H1 a flush busy for five polls is waited out before return");
	CHECK(busy_reports == 5 && diagnostics == 0 && trace_has(RTL836X_REG_FORCE_FLUSH, PORT_BIT(2)),
	      "H2 five busy reports, the command written once, no diagnostic");
}

/* A flush that never completes: the call returns after a bounded wait, issues nothing again */
static void scenario_never_completes(void)
{
	reset_model();
	flush_busy_reads = 1000000;
	unsigned target = l2_add(4, true);

	port_l2_forget_port(4);

	CHECK(status_reads <= STATUS_READS_MAX && diagnostics >= 1,
	      "G1 a flush that never completes returns after a bounded wait, with a diagnostic");
	CHECK(reg_writes == 3 && trace_count(RTL836X_REG_FORCE_FLUSH) == 1,
	      "G2 the command is written once and nothing is re-sent");
	CHECK(l2[target].valid, "G3 the driver removes no entries itself; the engine owns the flush");
}

int main(void)
{
	alarm(20);	/* a wait that never returns fails the run instead of hanging it */

	scenario_dynamic_entries_of_one_port();
	scenario_each_front_port();
	scenario_cpu_port();
	scenario_refused_ports();
	scenario_busy_front_port_blocks();
	scenario_busy_cpu_port_blocks();
	scenario_flush_in_flight_completes_first();
	scenario_completion_waited_out();
	scenario_never_completes();

	printf("test_l2_flush_836x: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
