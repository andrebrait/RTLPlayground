# RTLPlayground as a Linux daemon: the shared CLI and web pages (cmd_parser.c, cmd_editor.c,
# httpd/page_impl.c) compiled unchanged with host gcc, linked against this directory's
# implementation of platform.h.
#
#   make PLATFORM=linux [BOARD=BENCH6]    from the top level, or make here
#   make test                             unit tests (none yet) and the console test

BOARD ?= BENCH6
ROOT   = ../..
OUT    = output

CC ?= gcc
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
LOCAL  = main.c console.c state.c ports.c stubs_2a_pending.c stubs_2b.c boards/$(BOARD).c

OBJS = $(addprefix $(OUT)/,$(LOCAL:.c=.o)) \
       $(addprefix $(OUT)/shared/,$(SHARED:.c=.o))

all: $(OUT)/rtlplayground

$(OUT)/rtlplayground: $(OBJS)
	$(CC) $^ -o $@

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

test: all
	sh test/console_test.sh

clean:
	rm -rf $(OUT)

.SECONDARY:
.PHONY: all test clean FORCE
FORCE:

-include $(OBJS:.o=.d)
