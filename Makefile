CC      ?= cc
BUILD   ?= build
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins

KNOWN_MODS := lsp ai fuss plugins
BAD_MODS   := $(filter-out $(KNOWN_MODS),$(MODULES))
ifneq ($(BAD_MODS),)
$(error unknown MODULES '$(BAD_MODS)' (known: $(KNOWN_MODS)))
endif

# Public module names do not always match their source directories.
MODDIR_lsp     := lsp
MODDIR_ai      := ai
MODDIR_fuss    := git
MODDIR_plugins := plug

CFLAGS := -std=c11 -pedantic -Wall -Wextra -Werror -Wvla -g -O2 \
          -MMD -MP -Isrc \
          -DSAG_WITH_LSP=$(if $(filter lsp,$(MODULES)),1,0) \
          -DSAG_WITH_AI=$(if $(filter ai,$(MODULES)),1,0) \
          -DSAG_WITH_FUSS=$(if $(filter fuss,$(MODULES)),1,0) \
          -DSAG_WITH_PLUGINS=$(if $(filter plugins,$(MODULES)),1,0)

# Keep source and link order deterministic across filesystems.
CORE_SRC := $(shell find src -path 'src/mod/*' -prune -o -name '*.c' \
              -print | sort)
MOD_SRC  := src/mod/mods.c \
  $(foreach m,$(filter $(KNOWN_MODS),$(MODULES)), \
    $(filter-out %/shim.c,$(sort $(wildcard src/mod/$(MODDIR_$(m))/*.c)))) \
  $(foreach m,$(filter-out $(MODULES),$(KNOWN_MODS)), \
    $(sort $(wildcard src/mod/$(MODDIR_$(m))/shim.c)))
SRC      := $(CORE_SRC) $(MOD_SRC)
OBJ      := $(SRC:%.c=$(BUILD)/%.o)

UNIT_SRC := $(sort $(wildcard tests/unit/*.c))
UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
UNIT_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(UNIT_OBJ)

BUILD_DIRS := $(sort $(dir $(OBJ) $(UNIT_OBJ)))

# A content mismatch makes FORCE a normal prerequisite of every object built
# by this invocation.  The stamp recipe also removes objects not reachable
# from the requested target (notably main.o during `make test`), so a later
# target cannot reuse macros from the previous module selection.
STAMP_MODULES := $(file <$(BUILD)/mods.stamp)
ifneq ($(STAMP_MODULES),$(MODULES))
MODULE_FORCE := FORCE
endif

.DEFAULT_GOAL := all
.PHONY: all test clean install dirs FORCE test-script test-pty fuzz

all: $(BUILD)/sagitta $(BUILD)/sag

$(BUILD)/sagitta: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(BUILD)/sag: $(BUILD)/sagitta
	ln -sf sagitta $@

$(BUILD)/unit_tests: $(UNIT_LINK_OBJ)
	$(CC) $(CFLAGS) -o $@ $(UNIT_LINK_OBJ)

test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

# Check the literal selection on every invocation, but preserve the stamp's
# mtime when it is unchanged so objects are not rebuilt spuriously.
$(BUILD)/mods.stamp: FORCE | dirs
	@if ! printf '%s\n' '$(MODULES)' | cmp -s - $@; then \
		rm -f $(OBJ) $(OBJ:.o=.d) $(UNIT_OBJ) $(UNIT_OBJ:.o=.d); \
		printf '%s\n' '$(MODULES)' > $@; \
	fi

$(BUILD)/%.o: %.c $(BUILD)/mods.stamp $(MODULE_FORCE) | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

dirs:
	mkdir -p $(BUILD_DIRS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD)/sagitta $(DESTDIR)$(PREFIX)/bin/sagitta
	ln -sf sagitta $(DESTDIR)$(PREFIX)/bin/sag

clean:
	rm -rf $(BUILD)

test-script test-pty fuzz:
	@echo "make: $@ is not implemented until Sprint 1" >&2
	@false

-include $(OBJ:.o=.d) $(UNIT_OBJ:.o=.d)

FORCE:
