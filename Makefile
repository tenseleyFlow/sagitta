CC      ?= cc
BUILD   ?= build
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins
FUZZ_ITERS ?= 200000
FUZZ_SEED  ?= 1

ifneq ($(filter 1,$(SAN)),)
ifneq ($(filter 1,$(VALGRIND)),)
$(error SAN=1 and VALGRIND=1 are mutually exclusive)
endif
endif

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
LDFLAGS :=

# Sanitized and plain objects must never mix: use SAN=1 BUILD=build-san.
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
LDFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
endif

UNIT_RUN := $(BUILD)/unit_tests
ifeq ($(VALGRIND),1)
UNIT_RUN := valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite $(BUILD)/unit_tests
endif

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

FUZZ_LIB_OBJ := $(BUILD)/tests/fuzz/fuzzlib.o
FUZZ_UTF8_OBJ := $(BUILD)/tests/fuzz/fuzz_utf8.o
FUZZ_GRAPHEME_OBJ := $(BUILD)/tests/fuzz/fuzz_grapheme.o
FUZZ_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(FUZZ_LIB_OBJ)

PERF_UNICODE_OBJ := $(BUILD)/tests/perf/perf_unicode.o
PERF_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(PERF_UNICODE_OBJ)

BUILD_DIRS := $(sort $(dir $(OBJ) $(UNIT_OBJ) $(FUZZ_LIB_OBJ) \
                $(FUZZ_UTF8_OBJ) $(FUZZ_GRAPHEME_OBJ) $(PERF_UNICODE_OBJ)))

# A content mismatch makes FORCE a normal prerequisite of every object built
# by this invocation.  The stamp recipe also removes objects not reachable
# from the requested target (notably main.o during `make test`), so a later
# target cannot reuse macros from the previous module selection.
STAMP_MODULES := $(file <$(BUILD)/mods.stamp)
ifneq ($(STAMP_MODULES),$(MODULES))
MODULE_FORCE := FORCE
endif

.DEFAULT_GOAL := all
.PHONY: all test clean install dirs FORCE test-script test-pty fuzz \
        unicode-tables perf-unicode

all: $(BUILD)/sagitta $(BUILD)/sag

$(BUILD)/sagitta: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

$(BUILD)/sag: $(BUILD)/sagitta
	ln -sf sagitta $@

$(BUILD)/unit_tests: $(UNIT_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UNIT_LINK_OBJ)

$(BUILD)/fuzz_utf8: $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ)

$(BUILD)/fuzz_grapheme: $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ)

$(BUILD)/perf_unicode: $(PERF_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_LINK_OBJ)

$(BUILD)/gen-unicode-tables: scripts/gen-unicode-tables.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: $(BUILD)/unit_tests $(BUILD)/sagitta
	$(UNIT_RUN)
	scripts/check-sigsafe.sh
	scripts/smoke.sh $(BUILD)/sagitta

fuzz: $(BUILD)/fuzz_utf8 $(BUILD)/fuzz_grapheme
	$(BUILD)/fuzz_utf8 --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_grapheme --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

perf-unicode: $(BUILD)/perf_unicode
	$(BUILD)/perf_unicode

unicode-tables: $(BUILD)/gen-unicode-tables
	$< ucd/16.0.0 > src/unicode/tables.c

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

test-script:
	@echo 'error: script-test runner lands in Sprint 37 (sag --batch)'; exit 1

test-pty:
	@echo 'error: pty-test harness lands in Sprint 6'; exit 1

-include $(OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(FUZZ_LIB_OBJ:.o=.d) \
         $(FUZZ_UTF8_OBJ:.o=.d) $(FUZZ_GRAPHEME_OBJ:.o=.d) \
         $(PERF_UNICODE_OBJ:.o=.d)

FORCE:
