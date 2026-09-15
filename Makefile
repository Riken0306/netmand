# =============================================================================
# netmand — Makefile
#
# Builds the netmand daemon and (later) netmandctl tool.
# Supports cross-compilation via CC=<cross-compiler> on the command line.
#
# Targets:
#   make            — build everything (daemon + tools)
#   make netmand    — build just the daemon
#   make tests      — build and run unit tests
#   make check      — CI: gcc, clang, ASan/UBSan, cross-build
#   make clean      — remove all build artifacts
#
# Cross-compile example:
#   make CC=arm-linux-gnueabihf-gcc
# =============================================================================

# --- Toolchain ---------------------------------------------------------------
CC       ?= gcc

# ':=' not '?=': an inherited CFLAGS from the environment must not be able to
# replace this line and take -Werror with it.  Callers add flags through
# EXTRA_CFLAGS / EXTRA_LDFLAGS instead.
CFLAGS   := -std=c99 -Wall -Wextra -Werror -pedantic -D_GNU_SOURCE
CFLAGS   += $(EXTRA_CFLAGS)
LDFLAGS  += $(EXTRA_LDFLAGS)
LDLIBS   ?= -lpthread

# --- Directories -------------------------------------------------------------
SRCDIR    = src
BUILDDIR  = build
OBJDIR    = $(BUILDDIR)/obj
CONFDIR   = conf
TOOLDIR   = tools
TESTDIR   = tests

# --- Source files (Step 1: core only) ----------------------------------------
CORE_SRCS = $(SRCDIR)/core/main.c \
            $(SRCDIR)/core/priv.c

# Collect all source files — add more as modules are implemented.
SRCS      = $(CORE_SRCS)
OBJS      = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))
DEPS      = $(OBJS:.o=.d)

# --- Outputs -----------------------------------------------------------------
DAEMON    = $(BUILDDIR)/netmand

# =============================================================================
# Default target
# =============================================================================
.PHONY: all
all: $(DAEMON)

# =============================================================================
# Daemon binary
# =============================================================================
$(DAEMON): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "==> Built $@"

# =============================================================================
# Object file compilation with automatic dependency tracking (-MMD -MP)
# =============================================================================
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -I$(SRCDIR) -c -o $@ $<

# =============================================================================
# Build directories
# =============================================================================
$(BUILDDIR):
	@mkdir -p $@

$(OBJDIR):
	@mkdir -p $@

# =============================================================================
# Tests (stub — will be fleshed out in Step 2+)
# =============================================================================
.PHONY: tests
tests:
	@echo "==> No tests defined yet (coming in Step 2: Logger)"

# =============================================================================
# CI — build under every toolchain we ship on.  Cross-compile breakage found
# at Step 16 is expensive; found here it is a one-line fix.
# A toolchain that is not installed is reported and skipped, never silently
# passed over.
# =============================================================================
CROSS_CC  ?= arm-linux-gnueabihf-gcc
CHECK_CCS ?= gcc clang $(CROSS_CC)

.PHONY: check
check:
	@rc=0; \
	for cc in $(CHECK_CCS); do \
	    if command -v $$cc >/dev/null 2>&1; then \
	        echo "==> check: building with $$cc"; \
	        $(MAKE) --no-print-directory clean >/dev/null && \
	        $(MAKE) --no-print-directory CC=$$cc || rc=1; \
	    else \
	        echo "==> check: SKIPPED, $$cc not installed"; \
	    fi; \
	done; \
	echo "==> check: building with ASan + UBSan"; \
	$(MAKE) --no-print-directory clean >/dev/null && \
	$(MAKE) --no-print-directory EXTRA_CFLAGS="-fsanitize=address,undefined -g" all tests || rc=1; \
	$(MAKE) --no-print-directory clean >/dev/null; \
	if [ $$rc -eq 0 ]; then echo "==> check: OK"; else echo "==> check: FAILED"; fi; \
	exit $$rc

# =============================================================================
# Install (optional, for Buildroot integration)
# =============================================================================
PREFIX    ?= /usr
DESTDIR   ?=

.PHONY: install
install: $(DAEMON)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 755 $(DAEMON) $(DESTDIR)$(PREFIX)/sbin/netmand
	install -d $(DESTDIR)/etc/netmand
	install -m 644 $(CONFDIR)/netmand.conf $(DESTDIR)/etc/netmand/netmand.conf
	@echo "==> Installed to $(DESTDIR)$(PREFIX)/sbin/netmand"

# =============================================================================
# Clean
# =============================================================================
.PHONY: clean
clean:
	rm -rf $(BUILDDIR)
	@echo "==> Cleaned"

# =============================================================================
# Include auto-generated dependency files
# =============================================================================
-include $(DEPS)

# =============================================================================
# Convenience: print variables for debugging the build system
# =============================================================================
.PHONY: print-%
print-%:
	@echo "$* = $($*)"
