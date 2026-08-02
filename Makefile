CC      ?= cc
BUILD   ?= build
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins
FUZZ_ITERS ?= 200000
FUZZ_SEED  ?= 1
FUZZ_SECONDS ?=
TEXTBUF_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                      0x9e3779b97f4a7c15 0xd1b54a32d192ed03
TEXTBUF_FUZZ_MIXES ?= typing paste undo
FUZZ_LONG_SECONDS ?= 450
TORTURE_SIGKILL_ITERS ?= 500
FIXTURE_DIR ?= $(BUILD)/fixtures
FIXTURE_MANIFEST ?= tests/perf/fixtures.sha
PERF_RUNNER_ID ?= local-$(shell uname -m)-$(shell uname -s | tr A-Z a-z)
PERF_BASELINE ?= tests/perf/baselines/perf-x86_64-linux-gnu.txt
LATENCY_BASELINE ?= tests/perf/baselines/latency-x86_64-linux-gnu.txt
PERF_ADVISORY ?= 0

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
          -MMD -MP -Isrc -Itests/pty -Itests/fuzz \
          -DSAG_WITH_LSP=$(if $(filter lsp,$(MODULES)),1,0) \
          -DSAG_WITH_AI=$(if $(filter ai,$(MODULES)),1,0) \
          -DSAG_WITH_FUSS=$(if $(filter fuss,$(MODULES)),1,0) \
          -DSAG_WITH_PLUGINS=$(if $(filter plugins,$(MODULES)),1,0)
LDFLAGS :=
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
SHARED_FLAG := -dynamiclib
DL_LIBS :=
else
SHARED_FLAG := -shared
DL_LIBS := -ldl
endif

# Sanitized and plain objects must never mix: use SAN=1 BUILD=build-san.
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
LDFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
endif

UNIT_RUN := $(BUILD)/unit_tests
PTY_RUN  := $(BUILD)/pty_runner
# Plain compiler lanes cover intentional abort contracts. Instrumented lanes
# exclude them because their deliberately unreleased process state is noise.
UNIT_DEATH_EXCLUDES := \
  --exclude piece_line_iterator_rejects_other_buffer \
  --exclude piece_checker_rejects_corruption \
  --exclude piece_live_iterator_rejects_edit \
  --exclude register_block_producer_hard_errors_with_sprint17 \
  --exclude log_bug_prehook \
  --exclude mark_generational_handles \
  --exclude multicursor_edit_guard_names_sprint17 \
  --exclude undo_multi_reason_names_sprint17 \
  --exclude undo_filter_reason_names_sprint19 \
  --exclude undo_replace_reason_names_sprint21 \
  --exclude undo_macro_reason_names_sprint34 \
  --exclude undo_lsp_reason_names_sprint47 \
  --exclude undo_save_rejects_open_transaction \
  --exclude cmd_registry_rejects_invalid_descriptors \
  --exclude render_invalid_cells_are_bugs
ifeq ($(VALGRIND),1)
VALGRIND_RUN := valgrind --quiet --error-exitcode=99 --leak-check=full \
                 --errors-for-leak-kinds=definite --track-fds=yes \
                 --child-silent-after-fork=yes
UNIT_RUN := SAG_TEST_INSTRUMENTED=1 $(VALGRIND_RUN) \
            $(BUILD)/unit_tests $(UNIT_DEATH_EXCLUDES) && \
            SAG_TORTURE_CLEAN_ONLY=1 $(VALGRIND_RUN) \
            --trace-children=yes $(BUILD)/unit_tests \
            --filter save_fault_shim_contract
PTY_RUN  := valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite --track-fds=yes \
            $(BUILD)/pty_runner
endif

ifeq ($(SAN),1)
UNIT_RUN := SAG_TORTURE_CLEAN_ONLY=1 SAG_TEST_INSTRUMENTED=1 \
            $(BUILD)/unit_tests $(UNIT_DEATH_EXCLUDES)
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

UNIT_SRC := $(filter-out tests/unit/fakeclip.c, \
              $(sort $(wildcard tests/unit/*.c)))
UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
FAKECLIP := $(BUILD)/fakeclip
PTY_VT_OBJ := $(BUILD)/tests/pty/vt.o
PTY_SNAPSHOT_OBJ := $(BUILD)/tests/pty/snapshot.o
PTY_ORACLE_OBJ := $(PTY_VT_OBJ) $(PTY_SNAPSHOT_OBJ)
PTY_HARNESS_OBJ := $(BUILD)/tests/pty/harness.o
PTY_REGISTRY_OBJ := $(BUILD)/tests/pty/registry.o
PTY_RUNNER_OBJ := $(BUILD)/tests/pty/runner.o
PTY_DEMO_OBJ := $(BUILD)/tests/pty/demo_paint.o
PTY_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) \
                $(PTY_ORACLE_OBJ) $(PTY_HARNESS_OBJ) $(PTY_REGISTRY_OBJ) \
                $(PTY_RUNNER_OBJ)
PTY_DEMO_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(PTY_DEMO_OBJ)
TEXT_FUZZ_SUPPORT_OBJ := $(BUILD)/tests/fuzz/oracle.o \
                         $(BUILD)/tests/fuzz/shrink.o
UNIT_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(UNIT_OBJ) \
                 $(PTY_ORACLE_OBJ) $(PTY_HARNESS_OBJ) \
                 $(TEXT_FUZZ_SUPPORT_OBJ)

FUZZ_LIB_OBJ := $(BUILD)/tests/fuzz/fuzzlib.o
FUZZ_UTF8_OBJ := $(BUILD)/tests/fuzz/fuzz_utf8.o
FUZZ_GRAPHEME_OBJ := $(BUILD)/tests/fuzz/fuzz_grapheme.o
FUZZ_INPUT_OBJ := $(BUILD)/tests/fuzz/fuzz_input.o
FUZZ_GRID_OBJ := $(BUILD)/tests/fuzz/fuzz_grid.o
FUZZ_VT_OBJ := $(BUILD)/tests/fuzz/fuzz_vt.o
FUZZ_UNDO_OBJ := $(BUILD)/tests/fuzz/fuzz_undo.o
FUZZ_TEXTBUF_OBJ := $(BUILD)/tests/fuzz/fuzz_textbuf.o
FUZZ_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
FUZZ_LINK_OBJ := $(FUZZ_CORE_OBJ) $(FUZZ_LIB_OBJ)

PERF_UNICODE_OBJ := $(BUILD)/tests/perf/perf_unicode.o
PERF_RENDER_OBJ := $(BUILD)/tests/perf/perf_render.o
PERF_PIECE_OBJ := $(BUILD)/tests/perf/perf_piece.o
PERF_CURSOR_OBJ := $(BUILD)/tests/perf/perf_cursor.o
PERF_UNDO_OBJ := $(BUILD)/tests/perf/perf_undo.o
PERF_TEXTBUF_OBJ := $(BUILD)/tests/perf/perf_textbuf.o
PERF_LATENCY_OBJ := $(BUILD)/tests/perf/latency.o
PERF_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
GEN_BIGFILE_OBJ := $(BUILD)/scripts/gen-bigfile.o

TORTURE_CHILD_OBJ := $(BUILD)/tests/torture/sag-torture.o
TORTURE_DRIVER_OBJ := $(BUILD)/tests/torture/kill9.o
TORTURE_LIVE_OBJ := $(BUILD)/tests/torture/sag-live-torture.o
TORTURE_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
TORTURE_CHILD := $(BUILD)/sag-torture
TORTURE_DRIVER := $(BUILD)/kill9
TORTURE_LIVE := $(BUILD)/sag-live-torture
FAULTSHIM := $(BUILD)/tests/torture/faultshim.so

BUILD_DIRS := $(sort $(dir $(OBJ) $(UNIT_OBJ) $(FUZZ_LIB_OBJ) \
                $(FUZZ_UTF8_OBJ) $(FUZZ_GRAPHEME_OBJ) $(FUZZ_INPUT_OBJ) \
                $(FUZZ_GRID_OBJ) $(FUZZ_VT_OBJ) $(FUZZ_UNDO_OBJ) \
                $(FUZZ_TEXTBUF_OBJ) $(TEXT_FUZZ_SUPPORT_OBJ) \
                $(PTY_ORACLE_OBJ) \
                $(PTY_HARNESS_OBJ) $(PTY_REGISTRY_OBJ) $(PTY_RUNNER_OBJ) \
                $(PTY_DEMO_OBJ) $(PERF_UNICODE_OBJ) $(PERF_RENDER_OBJ) \
                $(PERF_PIECE_OBJ) $(PERF_CURSOR_OBJ) $(PERF_UNDO_OBJ) \
                $(PERF_TEXTBUF_OBJ) $(GEN_BIGFILE_OBJ) \
                $(TORTURE_CHILD_OBJ) \
                $(TORTURE_DRIVER_OBJ) $(TORTURE_LIVE_OBJ) $(FAULTSHIM)))

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
        fuzz-textbuf fuzz-long fixtures fixtures-quick fixtures-verify \
        fixtures-verify-quick \
        unicode-tables perf perf-unicode perf-render perf-piece perf-cursor \
        perf-undo perf-textbuf perf-huge perf-update perf-baseline-guard \
        perf-gate-selftest perf-latency \
        torture torture-build

all: $(BUILD)/sagitta $(BUILD)/sag

$(BUILD)/sagitta: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

$(BUILD)/sag: $(BUILD)/sagitta
	ln -sf sagitta $@

$(BUILD)/unit_tests: $(UNIT_LINK_OBJ) $(FAKECLIP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UNIT_LINK_OBJ)

$(BUILD)/pty_runner: $(PTY_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PTY_LINK_OBJ)

$(BUILD)/demo_paint: $(PTY_DEMO_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PTY_DEMO_LINK_OBJ)

$(BUILD)/fuzz_utf8: $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ)

$(BUILD)/fuzz_grapheme: $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ)

$(BUILD)/fuzz_input: $(FUZZ_LINK_OBJ) $(FUZZ_INPUT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_INPUT_OBJ)

$(BUILD)/fuzz_grid: $(FUZZ_LINK_OBJ) $(FUZZ_GRID_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_GRID_OBJ)

$(BUILD)/fuzz_vt: $(FUZZ_LINK_OBJ) $(PTY_VT_OBJ) $(FUZZ_VT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(PTY_VT_OBJ) \
		$(FUZZ_VT_OBJ)

$(BUILD)/fuzz_undo: $(FUZZ_CORE_OBJ) $(FUZZ_UNDO_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) $(FUZZ_UNDO_OBJ)

$(BUILD)/fuzz_textbuf: $(FUZZ_CORE_OBJ) $(TEXT_FUZZ_SUPPORT_OBJ) \
                       $(FUZZ_TEXTBUF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) \
		$(TEXT_FUZZ_SUPPORT_OBJ) $(FUZZ_TEXTBUF_OBJ)

$(BUILD)/gen-bigfile: $(GEN_BIGFILE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(GEN_BIGFILE_OBJ)

$(BUILD)/perf_textbuf: $(PERF_CORE_OBJ) $(PERF_TEXTBUF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_TEXTBUF_OBJ)

$(BUILD)/perf_unicode: $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ)

$(BUILD)/perf_render: $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ)

$(BUILD)/perf_piece: $(PERF_CORE_OBJ) $(PERF_PIECE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_PIECE_OBJ)

$(BUILD)/perf_cursor: $(PERF_CORE_OBJ) $(PERF_CURSOR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_CURSOR_OBJ)

$(BUILD)/perf_undo: $(PERF_CORE_OBJ) $(PERF_UNDO_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNDO_OBJ)

$(BUILD)/perf_latency: $(PERF_CORE_OBJ) $(PERF_LATENCY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_LATENCY_OBJ)

$(TORTURE_CHILD): $(TORTURE_CORE_OBJ) $(TORTURE_CHILD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_CORE_OBJ) \
		$(TORTURE_CHILD_OBJ)

$(TORTURE_DRIVER): $(TORTURE_DRIVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_DRIVER_OBJ)

$(TORTURE_LIVE): $(TORTURE_CORE_OBJ) $(TORTURE_LIVE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_CORE_OBJ) \
		$(TORTURE_LIVE_OBJ)

$(FAULTSHIM): tests/torture/faultshim.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC $(SHARED_FLAG) -o $@ $< $(DL_LIBS)

$(BUILD)/gen-unicode-tables: scripts/gen-unicode-tables.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(FAKECLIP): tests/unit/fakeclip.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: $(BUILD)/unit_tests $(BUILD)/sagitta test-pty torture-build
	$(UNIT_RUN)
	scripts/check-cmd-dispatch.sh
	scripts/check-input.sh
	scripts/check-render.sh
	scripts/check-sigsafe.sh
	scripts/smoke.sh $(BUILD)/sagitta

fuzz: $(BUILD)/fuzz_utf8 $(BUILD)/fuzz_grapheme $(BUILD)/fuzz_input \
      $(BUILD)/fuzz_grid $(BUILD)/fuzz_vt $(BUILD)/fuzz_undo \
      fuzz-textbuf
	$(BUILD)/fuzz_utf8 --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_grapheme --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_input --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_grid --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_vt --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_undo --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	@if [ -n "$(FUZZ_SECONDS)" ]; then \
		$(BUILD)/fuzz_input --seconds=$(FUZZ_SECONDS) --seed=$(FUZZ_SEED); \
	fi

fuzz-textbuf: $(BUILD)/fuzz_textbuf
	$(BUILD)/fuzz_textbuf --replay tests/fuzz/replay-smoke.trace
	@set -eu; \
	for seed in $(TEXTBUF_FUZZ_SEEDS); do \
		for mix in $(TEXTBUF_FUZZ_MIXES); do \
			$(BUILD)/fuzz_textbuf --iters=$(FUZZ_ITERS) \
				--seed=$$seed --mix=$$mix; \
		done; \
	done

fuzz-long: $(BUILD)/fuzz_textbuf
	@set -eu; \
	for mix in typing paste undo lines; do \
		seed=0x$$(od -An -N8 -tx8 /dev/urandom | tr -d ' \n'); \
		$(BUILD)/fuzz_textbuf --iters=6 --seconds=$(FUZZ_LONG_SECONDS) \
			--seed=$$seed --mix=$$mix; \
	done

perf-unicode: $(BUILD)/perf_unicode
	$(BUILD)/perf_unicode

perf-render: $(BUILD)/perf_render
	$(BUILD)/perf_render

perf-piece: $(BUILD)/perf_piece
	$(BUILD)/perf_piece

perf: perf-unicode perf-render perf-piece perf-cursor perf-undo perf-textbuf \
      perf-latency

perf-cursor: $(BUILD)/perf_cursor
	$(BUILD)/perf_cursor

perf-undo: $(BUILD)/perf_undo
	$(BUILD)/perf_undo

perf-latency: $(BUILD)/perf_latency
	$(BUILD)/perf_latency --baseline $(LATENCY_BASELINE)

fixtures-quick: $(BUILD)/gen-bigfile
	@mkdir -p $(FIXTURE_DIR); \
	awk '!/^#/ && $$1 ~ /^100m-/ { print $$1, $$2, $$3, $$4 }' \
		$(FIXTURE_MANIFEST) | \
	while read profile bytes seed hash; do \
		out=$(FIXTURE_DIR)/$$profile.bin; \
		stamp=$(FIXTURE_DIR)/$$profile-$$bytes-$$seed.stamp; \
		if [ ! -f "$$out" ] || [ ! -f "$$stamp" ] || \
		   [ $(BUILD)/gen-bigfile -nt "$$stamp" ]; then \
			$(BUILD)/gen-bigfile --profile "$$profile" --size "$$bytes" \
				--seed "$$seed" --output "$$out"; \
			touch "$$stamp"; \
		fi; \
	done

fixtures: $(BUILD)/gen-bigfile
	@mkdir -p $(FIXTURE_DIR); \
	awk '!/^#/ { print $$1, $$2, $$3, $$4 }' $(FIXTURE_MANIFEST) | \
	while read profile bytes seed hash; do \
		out=$(FIXTURE_DIR)/$$profile.bin; \
		stamp=$(FIXTURE_DIR)/$$profile-$$bytes-$$seed.stamp; \
		if [ ! -f "$$out" ] || [ ! -f "$$stamp" ] || \
		   [ $(BUILD)/gen-bigfile -nt "$$stamp" ]; then \
			$(BUILD)/gen-bigfile --profile "$$profile" --size "$$bytes" \
				--seed "$$seed" --output "$$out"; \
			touch "$$stamp"; \
		fi; \
	done

fixtures-verify-quick: fixtures-quick
	@set -eu; \
	awk '!/^#/ && $$1 ~ /^100m-/ { print $$1, $$4 }' \
		$(FIXTURE_MANIFEST) | \
	while read profile hash; do \
		$(BUILD)/gen-bigfile --verify \
			$(FIXTURE_DIR)/$$profile.bin 0x$$hash; \
	done

fixtures-verify: fixtures
	@set -eu; \
	awk '!/^#/ { print $$1, $$4 }' $(FIXTURE_MANIFEST) | \
	while read profile hash; do \
		$(BUILD)/gen-bigfile --verify \
			$(FIXTURE_DIR)/$$profile.bin 0x$$hash; \
	done

perf-textbuf: $(BUILD)/perf_textbuf fixtures-quick
	SAG_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_textbuf \
		--fixtures $(FIXTURE_DIR) --baseline $(PERF_BASELINE) \
		--runner-id $(PERF_RUNNER_ID)

perf-huge: $(BUILD)/perf_textbuf fixtures
	SAG_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_textbuf \
		--fixtures $(FIXTURE_DIR) --baseline $(PERF_BASELINE) \
		--runner-id $(PERF_RUNNER_ID) --huge

perf-update: $(BUILD)/perf_textbuf fixtures
	SAG_PERF_ADVISORY=1 $(BUILD)/perf_textbuf --fixtures $(FIXTURE_DIR) \
		--baseline $(PERF_BASELINE) --runner-id $(PERF_RUNNER_ID) \
		--huge --update

perf-baseline-guard:
	scripts/perf-baseline-guard.sh

perf-gate-selftest: $(BUILD)/perf_textbuf fixtures-quick
	@if SAG_PERF_INJECT_OPEN_DELAY=1 SAG_PERF_ADVISORY=0 \
		$(BUILD)/perf_textbuf --fixtures $(FIXTURE_DIR) \
		--baseline $(PERF_BASELINE) \
		--runner-id perf-x86_64-linux-gnu; then \
		echo 'error: performance gate accepted injected delay' >&2; \
		exit 1; \
	else \
		echo 'perf-gate-selftest: injected delay rejected'; \
	fi

torture-build: $(TORTURE_CHILD) $(TORTURE_LIVE) $(TORTURE_DRIVER) $(FAULTSHIM)

torture: torture-build
	SAG_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_CHILD)) \
		$(abspath $(FAULTSHIM))
	SAG_TORTURE_CHECKER=$(abspath $(TORTURE_CHILD)) \
	SAG_TORTURE_LANE=live-editor \
	SAG_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_LIVE)) \
		$(abspath $(FAULTSHIM))

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

test-pty: $(BUILD)/pty_runner $(BUILD)/demo_paint $(BUILD)/sagitta
	$(PTY_RUN) --demo $(abspath $(BUILD)/demo_paint) \
		--sagitta $(abspath $(BUILD)/sagitta)

-include $(OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(FUZZ_LIB_OBJ:.o=.d) \
         $(FUZZ_UTF8_OBJ:.o=.d) $(FUZZ_GRAPHEME_OBJ:.o=.d) \
         $(FUZZ_INPUT_OBJ:.o=.d) $(FUZZ_GRID_OBJ:.o=.d) \
         $(FUZZ_VT_OBJ:.o=.d) $(FUZZ_UNDO_OBJ:.o=.d) \
         $(FUZZ_TEXTBUF_OBJ:.o=.d) $(TEXT_FUZZ_SUPPORT_OBJ:.o=.d) \
         $(PTY_ORACLE_OBJ:.o=.d) \
         $(PTY_HARNESS_OBJ:.o=.d) $(PTY_REGISTRY_OBJ:.o=.d) \
         $(PTY_RUNNER_OBJ:.o=.d) $(PTY_DEMO_OBJ:.o=.d) \
         $(PERF_UNICODE_OBJ:.o=.d) $(PERF_RENDER_OBJ:.o=.d) \
         $(PERF_PIECE_OBJ:.o=.d) $(PERF_CURSOR_OBJ:.o=.d) \
         $(PERF_UNDO_OBJ:.o=.d) $(PERF_TEXTBUF_OBJ:.o=.d) \
         $(PERF_LATENCY_OBJ:.o=.d) \
         $(GEN_BIGFILE_OBJ:.o=.d) \
         $(TORTURE_CHILD_OBJ:.o=.d) \
	 $(TORTURE_DRIVER_OBJ:.o=.d) $(TORTURE_LIVE_OBJ:.o=.d)

FORCE:
