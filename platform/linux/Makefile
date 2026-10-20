# RTLPlayground as a Linux daemon: the shared CLI and web pages (cmd_parser.c, cmd_editor.c,
# httpd/page_impl.c) compiled unchanged with host gcc, linked against this directory's
# implementation of platform.h.
#
#   make PLATFORM=linux [BOARD=BENCH6]    from the top level, or make here
#   make test                             the console test
#
# Builds output/rtlplayground (the daemon), output/swcli (its command-line client) and, through the
# top-level html_min target, the minified web UI in $(ROOT)/output/html_min with a gzip copy
# (<name>.gz) of every file but login.html, as the 8051 image holds it. The daemon serves a
# directory like that from --www (default /usr/share/rtlplayground/www); the bench passes this one.

BOARD ?= BENCH6
ROOT   = ../..
OUT    = output

ifeq ($(origin CC),default)
CC = gcc
endif
# The warning policy of test/Makefile. -DRTLP_HOST_TEST keeps platform.h from declaring the
# 8051 memcpy/strlen/... that clash with libc. The shim goes in first so __xdata and friends
# vanish before any firmware header. -I. precedes the repository root: it holds the stand-ins
# for the two headers an 8051 build generates (version.h, html_data.h).
CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas \
         -Wno-pointer-sign -DRTLP_HOST_TEST -D_GNU_SOURCE \
         -include sdcc_shim.h -I. -I$(OUT) -I$(ROOT) -I$(ROOT)/httpd -I$(ROOT)/uip
ifeq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
CFLAGS += -Wno-builtin-declaration-mismatch
endif
# The shared files are never edited for the Linux build, so what they trip is relaxed here,
# for them alone (the same three diagnostics test/Makefile relaxes for its seam build).
SHARED_CFLAGS = -Wno-parentheses -Wno-misleading-indentation -Wno-incompatible-pointer-types

# Compiled from copies: a quoted #include is looked up next to the including file before any
# -I, so in place cmd_parser.c would pick up a version.h left in the repository root by an
# 8051 build instead of the Linux one (test/Makefile does the same for its seam check).
SHARED = cmd_parser.c cmd_editor.c page_impl.c
vpath %.c $(ROOT) $(ROOT)/httpd
LOCAL  = main.c console.c state.c nl.c bridge.c mgmt.c ports.c http.c auth.c config.c cli_sock.c stubs_2a_pending.c stubs_2b.c boards/$(BOARD).c

OBJS = $(addprefix $(OUT)/,$(LOCAL:.c=.o)) \
       $(addprefix $(OUT)/shared/,$(SHARED:.c=.o))

WWW = $(ROOT)/output/html_min

all: $(OUT)/rtlplayground $(OUT)/swcli html_min

$(OUT)/rtlplayground: $(OBJS)
	$(CC) $^ -o $@ -lcrypt -lmnl

# swcli talks to the socket and nothing else: no shared code, no board.
$(OUT)/swcli: swcli.c | $(OUT)
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Werror -D_GNU_SOURCE $< -o $@

# PLATFORM= keeps the top-level makefile from delegating straight back here.
html_min:
	$(MAKE) -C $(ROOT) PLATFORM= html_min
	@for f in $(WWW)/*; do \
		case $$f in *.gz|*/login.html) ;; *) gzip -9n -c $$f > $$f.gz || exit 1 ;; esac; \
	done

# boards/$(BOARD).c is the only file that names a board: an unknown BOARD stops here.
boards/$(BOARD).c:
	@echo "ERROR: BOARD='$(BOARD)' has no definition (expected platform/linux/$@)." >&2
	@false

$(OUT)/%.o: %.c $(OUT)/version.h | $(OUT)/boards
	$(CC) -MMD $(CFLAGS) -c $< -o $@

$(OUT)/shared/%.c: %.c | $(OUT)/shared
	cp $< $@

$(OUT)/shared/%.o: $(OUT)/shared/%.c $(OUT)/version.h $(OUT)/html_data.h
	$(CC) -MMD $(CFLAGS) $(SHARED_CFLAGS) -c $< -o $@

$(OUT) $(OUT)/boards $(OUT)/shared:
	mkdir -p $@

# page_impl.c includes the table of embedded web files an 8051 build generates and uses nothing
# from it; this platform serves html/ from disk, so the header is empty.
$(OUT)/html_data.h: | $(OUT)
	echo '/* the web UI is served from disk on Linux */' > $@

# Same text as the top-level Makefile writes, with the platform in the name.
VERSION := $(shell sed -n 's/^VERSION=//p' $(ROOT)/Makefile)
GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
# The HEAD commit date, so a rebuild of the same commit is byte-identical.
EPOCH := $(or $(SOURCE_DATE_EPOCH),$(shell git show -s --format=%ct HEAD 2>/dev/null),0)
BUILD_DATE := $(shell date -u -d @$(EPOCH) +"%Y-%m-%d %H:%M:%S" 2>/dev/null || date -u -r $(EPOCH) +"%Y-%m-%d %H:%M:%S")
$(OUT)/version.h: FORCE | $(OUT)
	@printf '%s\n' "#ifndef VERSION_H" "#define VERSION_H" \
		"#define VERSION_SW \"v$(VERSION)-$(GIT_VERSION)-linux-$(BOARD)\"" \
		"#define BUILD_DATE \"$(BUILD_DATE)\"" \
		"#endif" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

# The unit tests include only the pure headers (linkcode.h, mibmap.h, portlogic.h): no board, no
# shared code, no NIC. test_linkcode is plain C; the others need platform.h and the shim.
UNIT = test_linkcode test_mibmap test_portlogic
$(OUT)/test_linkcode: test/test_linkcode.c linkcode.h | $(OUT)
	$(CC) -std=gnu11 -O1 -Wall -Wextra -Werror -D_GNU_SOURCE -I. $< -o $@
$(OUT)/test_mibmap: test/test_mibmap.c mibmap.h | $(OUT) $(OUT)/version.h
	$(CC) $(CFLAGS) $< -o $@
$(OUT)/test_portlogic: test/test_portlogic.c portlogic.h | $(OUT) $(OUT)/version.h
	$(CC) $(CFLAGS) $< -o $@

test: all $(addprefix $(OUT)/,$(UNIT))
	@for t in $(UNIT); do $(OUT)/$$t || exit 1; done
	sh test/console_test.sh

clean:
	rm -rf $(OUT)

.SECONDARY:
.PHONY: all test clean FORCE html_min
FORCE:

-include $(OBJS:.o=.d)
