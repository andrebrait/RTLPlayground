# Host unit-test harness for RTLPlayground firmware logic.
#
# Compiles selected firmware translation units with host gcc + AddressSanitizer,
# force-including sdcc_shim.h to erase 8051 keywords, and links them against
# mocked hardware edges (support.c). No SDCC, no flashing, no hardware.
#
#   make        build + run all tests
#   make run    run tests
#   make clean

CC       = gcc
SRCROOT  = ..
# -include shim first so __xdata/__code/__banked vanish before any firmware header.
CFLAGS   = -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas \
           -Wno-pointer-sign \
           -DRTLP_HOST_TEST \
           -fsanitize=address,undefined -fno-omit-frame-pointer \
           -include sdcc_shim.h -Istub -I. -I$(SRCROOT) -I$(SRCROOT)/httpd -I$(SRCROOT)/uip
ifeq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
CFLAGS  += -Wno-builtin-declaration-mismatch
endif
LDFLAGS  = -fsanitize=address,undefined

BUILDDIR = build

# One binary per subsystem under test.
TESTS = $(BUILDDIR)/test_cmd_editor \
        $(BUILDDIR)/test_port_tables \
        $(BUILDDIR)/test_page_json \
        $(BUILDDIR)/test_httpd_tx \
        $(BUILDDIR)/test_tick_gate \
        $(BUILDDIR)/test_lacp \
        $(BUILDDIR)/test_golden

# The table tests link the register mock and one environment file; stub/
# stands in for the two headers a firmware build generates.
TABLE_ENV = support.c hw_mock.c env_tables.c $(SRCROOT)/cmd_editor.c \
            $(SRCROOT)/rtl837x_port.c $(SRCROOT)/rtl837x_bandwidth.c $(SRCROOT)/rtl837x_storm.c \
            $(SRCROOT)/httpd/page_impl.c

all: run

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# cmd_editor.c is compiled UNMODIFIED from the firmware tree.
$(BUILDDIR)/test_cmd_editor: test_cmd_editor.c support.c $(SRCROOT)/cmd_editor.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# rtl837x_port.c, the bandwidth and storm units and httpd/page_impl.c are compiled UNMODIFIED from the firmware tree.
$(BUILDDIR)/test_port_tables: test_port_tables.c $(TABLE_ENV) hw_mock.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -Wno-incompatible-pointer-types $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILDDIR)/test_tick_gate: test_tick_gate.c support.c $(SRCROOT)/cmd_editor.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/test_page_json: test_page_json.c $(TABLE_ENV) hw_mock.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -Wno-incompatible-pointer-types $(filter %.c,$^) -o $@ $(LDFLAGS)

# cmd_parser.c, the trap unit and the table environment are compiled UNMODIFIED; golden_env.c stubs what the parser
# dispatches into elsewhere. cmd_parser.c has its own definitions of the editor
# buffers support.c also defines, so common symbols merge, and it needs three
# diagnostics relaxed that the other units do not trigger.
GOLDEN_SRC = test_golden.c golden_env.c $(TABLE_ENV) $(SRCROOT)/cmd_parser.c \
             $(SRCROOT)/rtl837x_trap.c
$(BUILDDIR)/test_golden: $(GOLDEN_SRC) hw_mock.h support.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fcommon -Wno-parentheses -Wno-misleading-indentation -Wno-incompatible-pointer-types \
		-DGOLDEN_DIR='"$(CURDIR)/golden"' $(filter %.c,$(GOLDEN_SRC)) -o $@ $(LDFLAGS)

# rtl837x_lacp.c is compiled UNMODIFIED from the firmware tree.
$(BUILDDIR)/test_lacp: test_lacp.c $(SRCROOT)/rtl837x_lacp.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# httpd/httpd.c and uip/uip.c are compiled UNMODIFIED from the firmware tree;
# this test carries the environment below them itself.
$(BUILDDIR)/test_httpd_tx: test_httpd_tx.c $(SRCROOT)/httpd/httpd.c $(SRCROOT)/uip/uip.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS)

# The seam check: the three shared files (cmd_parser.c, cmd_editor.c,
# httpd/page_impl.c) compiled with seam_stub/ ahead of every other include
# path, so the register and SFR headers are poisoned (#error), and linked
# against platform_mock.c only. It builds once the shared files reach the
# hardware solely through platform.h. Not part of `run` until that holds.
# The files are compiled from a copy: a quoted #include is looked up next to
# the including file before any -I, so in place cmd_parser.c would find the
# real ../rtl837x_regs.h and never see the poison.
SEAM_SHARED = $(SRCROOT)/cmd_parser.c $(SRCROOT)/cmd_editor.c $(SRCROOT)/httpd/page_impl.c
SEAMDIR     = $(BUILDDIR)/seam
$(BUILDDIR)/test_platform_seam: test_platform_seam.c platform_mock.c $(SEAM_SHARED) $(SRCROOT)/platform.h | $(BUILDDIR)
	mkdir -p $(SEAMDIR) && cp $(SEAM_SHARED) $(SEAMDIR)/
	$(CC) -Iseam_stub $(CFLAGS) -Wno-parentheses -Wno-misleading-indentation -Wno-incompatible-pointer-types \
		test_platform_seam.c platform_mock.c $(addprefix $(SEAMDIR)/,$(notdir $(SEAM_SHARED))) -o $@ $(LDFLAGS)

seam_check: $(BUILDDIR)/test_platform_seam
	$<

run: $(TESTS)
	@fail=0; for t in $(TESTS); do \
		echo "=== $$t ==="; \
		$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; else echo "ALL TESTS PASSED"; fi

clean:
	rm -rf $(BUILDDIR)

.PHONY: all run clean seam_check
