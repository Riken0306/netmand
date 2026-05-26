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
#   make clean      — remove all build artifacts
#
# Cross-compile example:
#   make CC=arm-linux-gnueabihf-gcc
# =============================================================================

# --- Toolchain ---------------------------------------------------------------
CC       ?= gcc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -pedantic
CFLAGS   += -D_GNU_SOURCE
LDFLAGS  ?=
LDLIBS   ?= -lpthread

# --- Directories -------------------------------------------------------------
SRCDIR    = src
BUILDDIR  = build
OBJDIR    = $(BUILDDIR)/obj
CONFDIR   = conf
TOOLDIR   = tools
TESTDIR   = tests

# --- Source files (Step 1: core only) ----------------------------------------
CORE_SRCS = $(SRCDIR)/core/main.c

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
