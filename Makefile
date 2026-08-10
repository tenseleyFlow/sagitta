CC      ?= cc
BUILD   ?= build
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins
FUZZ_ITERS ?= 200000
FUZZ_SEED  ?= 1
FUZZ_SECONDS ?=
CMDPARSE_FUZZ_ITERS ?= 1000000
TEXTBUF_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                      0x9e3779b97f4a7c15 0xd1b54a32d192ed03
TEXTBUF_FUZZ_MIXES ?= typing paste undo
UNITS_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                    0x9e3779b97f4a7c15 0xd1b54a32d192ed03
MULTICURSOR_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                          0x9e3779b97f4a7c15 0xd1b54a32d192ed03
MULTICURSOR_FUZZ_OPS ?= 100000
# Sprint 27: 100 000 mouse events per seed, four seeds.  Sessions rather
# than a raw iteration count, because each session drives a whole editor
# and the events are what the gate is counted in.
MOUSE_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                    0x9e3779b97f4a7c15 0xd1b54a32d192ed03
MOUSE_FUZZ_EVENTS ?= 100000
#
# fuzz_groups is counted in SESSIONS, and not with FUZZ_ITERS.
#
# Its run_session builds a whole editor and then drives 400 membership
# steps and 400 picker steps, so one "iteration" costs ~50 ms plain and
# ~80 ms under the sanitizers -- three orders of magnitude more than an
# iteration of a per-operation target like fuzz_utf8.  Sharing
# FUZZ_ITERS=200000 with those therefore asked for 200 000 whole editor
# sessions, about 4.5 hours under ASan: the sanitize lane sat at 4 h 27 m
# against GitHub's 6 h ceiling with this one campaign still running, and
# had never once reached the end of it.  fuzz_panes and fuzz_tabs share
# the session shape but not the cost, and stay on FUZZ_ITERS.
#
# Four seeds beat more sessions on one seed for reaching new states, so
# the budget buys seeds first -- the same trade fuzz-mouse makes.
GROUPS_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                     0x9e3779b97f4a7c15 0xd1b54a32d192ed03
GROUPS_FUZZ_SESSIONS ?= 250
FUZZ_LONG_SECONDS ?= 450
TORTURE_SIGKILL_ITERS ?= 500
FIXTURE_DIR ?= $(BUILD)/fixtures
FIXTURE_MANIFEST ?= tests/perf/fixtures.sha
PERF_RUNNER_ID ?= local-$(shell uname -m)-$(shell uname -s | tr A-Z a-z)
PERF_BASELINE ?= tests/perf/baselines/perf-x86_64-linux-gnu.txt
LATENCY_BASELINE ?= tests/perf/baselines/latency-x86_64-linux-gnu.txt
SCRIPT_SUITE_BASELINE ?= tests/perf/baselines/script-x86_64-linux-gnu.txt
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

#
# _FORTIFY_SOURCE IS ON EVERYWHERE, DELIBERATELY.
#
# It is what turns glibc's warn_unused_result attributes on, and those
# are the difference between a distro whose gcc enables it by default
# (Ubuntu, and therefore CI) and one whose gcc does not (Arch, and
# therefore several developers).  Without it here, `(void)write(...)`
# compiles locally and fails the build on every CI gcc lane — which is
# exactly what happened, repeatedly, and each round cost a full CI
# cycle to learn one call site.
#
# Setting it in the one place both sides read means a developer sees
# the same errors CI will.  It needs an optimiser, which -O2 provides;
# the sanitizer lane drops to -O1 below and glibc simply ignores it
# there.
CFLAGS := -std=c11 -pedantic -Wall -Wextra -Werror -Wvla -g -O2 \
          -D_FORTIFY_SOURCE=2 \
          -DSAG_RUNTIME_DIR_DEFAULT='"$(PREFIX)/share/sagitta/runtime"' \
          -MMD -MP -Isrc -Itests -Itests/pty -Itests/fuzz \
          -DSAG_WITH_LSP=$(if $(filter lsp,$(MODULES)),1,0) \
          -DSAG_WITH_AI=$(if $(filter ai,$(MODULES)),1,0) \
          -DSAG_WITH_FUSS=$(if $(filter fuss,$(MODULES)),1,0) \
          -DSAG_WITH_PLUGINS=$(if $(filter plugins,$(MODULES)),1,0)

# Sprint 30 DoD 1: the Fletch VM's computed-goto dispatcher.  The label
# table is a GNU extension, so -pedantic rejects it under -std=c11 --
# and dropping the whole tree to gnu11 to accommodate one file would
# stop the other 200 from being checked against the standard we claim.
# So the relaxation is scoped to vm.c and nothing else, applied as a
# target-specific override below.
#
# -std=gnu11 alone is NOT enough: gcc's -pedantic still rejects `&&label`
# under it ("taking the address of a label is non-standard"), so vm.o
# also drops -pedantic in this lane.  Everything else -- -Wall -Wextra
# -Werror -Wvla -- still applies to it, and the default FL_CGOTO=0 build
# compiles the same file fully pedantic, so nothing goes unchecked.
#
# Off by default.  Both modes must produce identical results (DoD 5),
# which is what makes this an optimisation rather than a second
# semantics: `make FL_CGOTO=1 test` is a CI lane, not a build people
# have to choose between.
ifeq ($(FL_CGOTO),1)
CFLAGS += -DFL_COMPUTED_GOTO=1
endif

# The executed-opcode trace, for the differential-dispatch gate only.
# Compiled out by default: a push per instruction is not something the
# release VM carries so a test can watch it.
ifeq ($(CFLAGS_FL_TRACE),1)
CFLAGS += -DFL_VM_TRACE=1
endif

#
# Sprint 32 §8/§9: the VM's self-checks.
#
# fl_chunk_check after every compile, and the per-instruction invariant
# checks in the dispatch loop.  OFF by default because the second group
# is on the hot path and 02-fletch.md req 7 has no room for it; ON in
# the sanitize and fuzz lanes, which is where a compiler bug should be
# caught.  The checker itself is always COMPILED so the tests can drive
# it directly.
#
ifeq ($(FL_CHECKS),1)
CFLAGS += -DFL_VM_CHECKS=1
endif

#
# -lm for src/fl/stdmath.c.  libm is part of the C standard library, not
# a new dependency in the bespoke-first sense -- glibc 2.34+ folds it
# into libc and the flag is then a harmless no-op, while older glibc and
# the BSDs still need it.
#
LDFLAGS :=
#
# LDLIBS, NOT LDFLAGS, and it goes AFTER the objects.
#
# The linker resolves a library against the objects that
# PRECEDE it, so -lm in LDFLAGS -- which every rule expands
# before -o -- satisfies nothing. It linked anyway on a box
# whose glibc folds libm into libc (2.34+), and failed on CI,
# which is the worst possible split: green locally, eight
# lanes red on push.
#
LDLIBS := -lm
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
# The torture shim is LD_PRELOADed, which puts it ahead of the ASan
# runtime in the child's initial library list.  ASan then declines to
# install its interceptors, the shim's fault injection does not take,
# and the save-torture invariant fails for a reason that has nothing to
# do with saving.  kill9 already accepts a preload prefix; tell it where
# the runtime lives.  -print-file-name echoes its argument back when it
# finds nothing, which is how the fallbacks below are detected.
#
# ONLY GCC NEEDS ONE, and asking the question compiler-blind is a trap.
#
# GCC's ASan is a SHARED library, so a child that LD_PRELOADs a fault
# shim must name libasan first or ASan installs no interceptors.  Clang
# links its ASan STATICALLY, so there is no runtime to order against —
# and preloading a shared one on top of a static one aborts the child
# with "Your application is linked against incompatible ASan runtimes".
#
# The trap: `clang -print-file-name=libasan.so` SUCCEEDS.  It finds
# GCC's runtime through clang's gcc-toolchain search path, so a probe
# that just asks $(CC) for libasan hands clang the wrong library and the
# spawned-child tests fail with an assertion that says nothing about
# why.  So the probe runs for the GCC family only.
ASAN_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -ci clang)
ifeq ($(ASAN_IS_CLANG),0)
ASAN_RT := $(shell $(CC) -print-file-name=libasan.so)
ifeq ($(ASAN_RT),libasan.so)
ASAN_RT :=
endif
else
ASAN_RT :=
endif
ifneq ($(ASAN_RT),)
CFLAGS  += -DSAG_ASAN_RUNTIME='"$(ASAN_RT)"'
endif
endif

UNIT_RUN := $(BUILD)/unit_tests
# Deferred (=, not :=) so $(SAG_PTY_BUDGET_MS) resolves at USE time,
# after the VALGRIND/else branches below have chosen a value.  Without
# the prefix the plain lanes passed nothing and silently inherited
# runner.c's 180 s fallback.
PTY_RUN   = SAG_PTY_BUDGET_MS=$(SAG_PTY_BUDGET_MS) $(BUILD)/pty_runner
PTY_PREP :=
PTY_LOG_REDIRECT :=
# Plain compiler lanes cover intentional abort contracts. Instrumented lanes
# exclude them because their deliberately unreleased process state is noise.
UNIT_DEATH_EXCLUDES := \
  --exclude piece_line_iterator_rejects_other_buffer \
  --exclude piece_checker_rejects_corruption \
  --exclude piece_live_iterator_rejects_edit \
  --exclude log_bug_prehook \
  --exclude mark_generational_handles \
  --exclude multicursor_edit_guard_requires_multi_transaction \
  --exclude multicursor_deferred_guards_name_their_sprints \
  --exclude block_syntax_install_names_sprint40 \
  --exclude ctxmenu_a_row_handler_reading_a_payload_is_a_bug \
  --exclude undo_filter_reason_names_sprint19 \
  --exclude undo_replace_reason_names_sprint21 \
  --exclude undo_macro_reason_names_sprint34 \
  --exclude undo_lsp_reason_names_sprint47 \
  --exclude undo_save_rejects_open_transaction \
  --exclude cmd_registry_rejects_invalid_descriptors \
  --exclude render_invalid_cells_are_bugs
ifeq ($(VALGRIND),1)
# MEASURED, not guessed: the full suite takes 1147 s under valgrind on a
# quiet developer machine (206 cases, QUIET_SCALE=8 — scaling the settles
# is what makes it slow, and is not optional; see quiet_scale()).  CI
# runners are slower still, so this is roughly 3x headroom.  It is a
# wall-clock ceiling, not a latency budget: nothing measures against it.
SAG_PTY_BUDGET_MS ?= 3600000
SAG_PTY_CASE_BUDGET_MS ?= 60000
SAG_SCRIPT_BUDGET_MS ?= 600000
# A settle infers "done" from silence, and valgrind makes the editor
# silent for far longer than it is idle.  See quiet_scale() in
# tests/pty/harness.c.
SAG_PTY_QUIET_SCALE ?= 8
VALGRIND_RUN := valgrind --quiet --error-exitcode=99 --leak-check=full \
                 --errors-for-leak-kinds=definite --track-fds=yes \
                 --child-silent-after-fork=yes
VALGRIND_TRACE_SKIP := \
    --trace-children-skip='/bin/*,/usr/bin/*,/usr/lib/*,/sbin/*'
UNIT_RUN := SAG_TEST_INSTRUMENTED=1 $(VALGRIND_RUN) \
            $(BUILD)/unit_tests $(UNIT_DEATH_EXCLUDES) && \
            SAG_TORTURE_CLEAN_ONLY=1 $(VALGRIND_RUN) \
            --trace-children=yes $(BUILD)/unit_tests \
            --filter save_fault_shim_contract
#
# --trace-children-skip: TRACE OUR EDITOR, NOT THE COMMANDS IT RUNS.
#
# --trace-children=yes is needed because the pty runner FORKS the editor,
# and the editor is what we are checking.  But the editor also shells out
# (Sprint 19's jobs), and valgrind followed into those too — where two
# things went wrong.  Their leaks are not ours: the lane reported
# "8 bytes definitely lost" inside /usr/bin/sort.  Worse,
# --error-exitcode=99 REPLACED the command's real exit status, so
# s19_filter_nonzero_keeps_buffer read `E: filter: exit 99` where the
# golden says `exit 2` — the gate was rewriting the thing under test.
#
# The editor is spawned by absolute path out of $(BUILD), so skipping the
# system prefixes leaves it traced and takes /bin/sh and its descendants
# out.
#
# THE TWO EXCLUSIONS ARE BOTH DELIBERATE-DEATH CASES.  Each ends the
# child on purpose — a SEGV, and s32's injected VM fault — so the process
# never unwinds and --track-fds reports the terminal's pipes as open at
# exit.  That is the point of those cases, not a defect in them, and
# --error-exitcode=99 would overwrite the exit status they exist to
# assert.  Both still run in every other lane, sanitize included.
PTY_RUN  := SAG_PTY_BUDGET_MS=$(SAG_PTY_BUDGET_MS) \
            SAG_PTY_CASE_BUDGET_MS=$(SAG_PTY_CASE_BUDGET_MS) \
            SAG_PTY_QUIET_SCALE=$(SAG_PTY_QUIET_SCALE) \
            SAG_PTY_EXCLUDE=notepad_restore_segv,s32_bug_restores_the_terminal \
            valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite --track-fds=yes \
            --trace-children=yes \
            $(VALGRIND_TRACE_SKIP) \
            --log-fd=9 $(BUILD)/pty_runner
PTY_PREP := ulimit -c 0 &&
PTY_LOG_REDIRECT := 9>&2
else
# MEASURED, like the valgrind ceiling above, and for the same reason: the
# suite has grown into the runner's built-in default.
#
# tests/pty/runner.c falls back to RUNNER_BUDGET_MS = 180000 when nothing
# sets this, and only the valgrind branch above ever did — so every plain
# lane has quietly been running against 180 s.  The 206-case suite takes
# 132 s on a quiet developer machine, which is 73% of that before a CI
# runner's slower cores are considered, and fletch-dispatch duly ran out
# on the last case with "global budget exhausted after 180000 ms" while
# the pty lane on the same commit finished.  That is a ceiling being
# brushed, not a hang.
#
# 600 s is ~4.5x the measured time.  It is a wall-clock ceiling on a
# HANG, not a latency budget — nothing measures against it, and the
# per-case budget is what bounds a single stuck case.
SAG_PTY_BUDGET_MS ?= 600000
ifeq ($(SAN),1)
# The 10,000-replacement migration case is deliberately CPU-heavy under
# per-instruction VM checks plus ASan/UBSan; this is a hang ceiling only.
SAG_SCRIPT_BUDGET_MS ?= 120000
else
SAG_SCRIPT_BUDGET_MS ?= 10000
endif
endif

ifeq ($(SAN),1)
UNIT_RUN := SAG_TORTURE_CLEAN_ONLY=1 SAG_TEST_INSTRUMENTED=1 \
            $(BUILD)/unit_tests $(UNIT_DEATH_EXCLUDES)
endif

# Keep source and link order deterministic across filesystems.
CORE_SRC := $(filter-out src/ws/fl_emit.c src/ws/fl_parse.c \
                         src/ws/state_legacy.c, \
              $(shell find src -path 'src/mod/*' -prune -o -name '*.c' \
                -print | sort))
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
STATE_LEGACY_OBJ := $(BUILD)/tests/unit/state_legacy.o

# Sprint 36: activate the independent Fletch arm in the frozen-corpus
# differential.  The hand-written parser remains visible only to tests.
$(BUILD)/tests/unit/test_state_differential.o: CFLAGS += \
  -DSAG_HAVE_FLETCH_STATE=1
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
                 $(TEXT_FUZZ_SUPPORT_OBJ) $(STATE_LEGACY_OBJ)

FUZZ_LIB_OBJ := $(BUILD)/tests/fuzz/fuzzlib.o
FUZZ_UTF8_OBJ := $(BUILD)/tests/fuzz/fuzz_utf8.o
FUZZ_GRAPHEME_OBJ := $(BUILD)/tests/fuzz/fuzz_grapheme.o
FUZZ_INPUT_OBJ := $(BUILD)/tests/fuzz/fuzz_input.o
FUZZ_GRID_OBJ := $(BUILD)/tests/fuzz/fuzz_grid.o
FUZZ_VT_OBJ := $(BUILD)/tests/fuzz/fuzz_vt.o
FUZZ_UNDO_OBJ := $(BUILD)/tests/fuzz/fuzz_undo.o
FUZZ_TEXTBUF_OBJ := $(BUILD)/tests/fuzz/fuzz_textbuf.o
FUZZ_UNITS_OBJ := $(BUILD)/tests/fuzz/fuzz_units.o
FUZZ_MULTICURSOR_OBJ := $(BUILD)/tests/fuzz/fuzz_multicursor.o
FUZZ_CMDPARSE_OBJ := $(BUILD)/tests/fuzz/fuzz_cmdparse.o
FUZZ_RECOMPILE_OBJ := $(BUILD)/tests/fuzz/fuzz_re_compile.o
FUZZ_REQUOTE_OBJ := $(BUILD)/tests/fuzz/fuzz_re_quote.o
FUZZ_SEARCH_OBJ := $(BUILD)/tests/fuzz/fuzz_search.o
FUZZ_PANES_OBJ := $(BUILD)/tests/fuzz/fuzz_panes.o
FUZZ_TABS_OBJ := $(BUILD)/tests/fuzz/fuzz_tabs.o
FUZZ_GROUPS_OBJ := $(BUILD)/tests/fuzz/fuzz_groups.o
FUZZ_REDIFF_OBJ := $(BUILD)/tests/fuzz/fuzz_re_diff.o
FUZZ_FUZZY_OBJ := $(BUILD)/tests/fuzz/fuzz_fuzzy.o
FUZZ_STATE_OBJ := $(BUILD)/tests/fuzz/fuzz_state.o
FUZZ_GITIGNORE_OBJ := $(BUILD)/tests/fuzz/fuzz_gitignore.o
FUZZ_MOUSE_OBJ := $(BUILD)/tests/fuzz/fuzz_mouse.o
FUZZ_FLLEX_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_lex.o
FUZZ_FLPARSE_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_parse.o
FUZZ_FLSTD_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_std.o
FUZZ_FLVM_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_vm.o
FUZZ_FLAPI_OBJ := $(BUILD)/tests/fuzz/fuzz_flapi.o
FUZZ_RECORD_OBJ := $(BUILD)/tests/fuzz/fuzz_record.o
RE_REF_OBJ := $(BUILD)/tests/fuzz/re_ref.o
FUZZ_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
FUZZ_LINK_OBJ := $(FUZZ_CORE_OBJ) $(FUZZ_LIB_OBJ)

PERF_UNICODE_OBJ := $(BUILD)/tests/perf/perf_unicode.o
PERF_RENDER_OBJ := $(BUILD)/tests/perf/perf_render.o
PERF_SCROLL_OBJ := $(BUILD)/tests/perf/scroll.o
PERF_PIECE_OBJ := $(BUILD)/tests/perf/perf_piece.o
PERF_CURSOR_OBJ := $(BUILD)/tests/perf/perf_cursor.o
PERF_UNDO_OBJ := $(BUILD)/tests/perf/perf_undo.o
PERF_TEXTBUF_OBJ := $(BUILD)/tests/perf/perf_textbuf.o
PERF_LATENCY_OBJ := $(BUILD)/tests/perf/latency.o
PERF_JOBSTREAM_OBJ := $(BUILD)/tests/perf/jobstream.o
PERF_REPATH_OBJ := $(BUILD)/tests/perf/re_pathological.o
PERF_RETHRU_OBJ := $(BUILD)/tests/perf/re_throughput.o
PERF_SEARCHLAT_OBJ := $(BUILD)/tests/perf/search_latency.o
PERF_UNITS_OBJ := $(BUILD)/tests/perf/perf_units.o
PERF_MULTICURSOR_OBJ := $(BUILD)/tests/perf/multicursor.o
PERF_CMDCOMP_OBJ := $(BUILD)/tests/perf/perf_cmdcomp.o
FL_SMOKE_OBJ := $(BUILD)/tests/perf/fl_smoke.o
PERF_STATE_OBJ := $(BUILD)/tests/perf/perf_state.o
PERF_FINDER_OBJ := $(BUILD)/tests/perf/finder.o
PERF_MOUSE_OBJ := $(BUILD)/tests/perf/mouse.o
LIVE_PTY_OBJ := $(BUILD)/tests/support/live_pty.o
PERF_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
PERF_FLETCH_OBJ := $(BUILD)/tests/perf/perf_fletch.o
PERF_RECORD_OBJ := $(BUILD)/tests/perf/perf_record.o
PERF_BATCH_OBJ := $(BUILD)/tests/perf/batch.o
PERF_SCRIPT_SUITE_OBJ := $(BUILD)/tests/perf/script_suite.o
FLETCH_RUN_OBJ := $(BUILD)/tests/fletch/run.o
SCRIPT_RUNNER_OBJ := $(BUILD)/tests/script/runner.o
FLETCH_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
ROUNDTRIP_OBJ := $(BUILD)/tests/roundtrip/gen.o \
                 $(BUILD)/tests/roundtrip/runner.o
ROUNDTRIP_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) \
                      $(ROUNDTRIP_OBJ)
GEN_BIGFILE_OBJ := $(BUILD)/scripts/gen-bigfile.o

TORTURE_CHILD_OBJ := $(BUILD)/tests/torture/sag-torture.o
TORTURE_DRIVER_OBJ := $(BUILD)/tests/torture/kill9.o
TORTURE_LIVE_OBJ := $(BUILD)/tests/torture/sag-live-torture.o
TORTURE_BATCH_OBJ := $(BUILD)/tests/torture/batch_kill9.o
TORTURE_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
TORTURE_CHILD := $(BUILD)/sag-torture
TORTURE_DRIVER := $(BUILD)/kill9
TORTURE_LIVE := $(BUILD)/sag-live-torture
TORTURE_BATCH := $(BUILD)/batch-kill9
FAULTSHIM := $(BUILD)/tests/torture/faultshim.so

BUILD_DIRS := $(sort $(dir $(OBJ) $(UNIT_OBJ) $(FUZZ_LIB_OBJ) \
                $(FUZZ_UTF8_OBJ) $(FUZZ_GRAPHEME_OBJ) $(FUZZ_INPUT_OBJ) \
                $(FUZZ_GRID_OBJ) $(FUZZ_VT_OBJ) $(FUZZ_UNDO_OBJ) \
                $(FUZZ_TEXTBUF_OBJ) $(TEXT_FUZZ_SUPPORT_OBJ) \
                $(FUZZ_UNITS_OBJ) \
                $(FUZZ_MULTICURSOR_OBJ) \
                $(FUZZ_CMDPARSE_OBJ) $(FUZZ_RECOMPILE_OBJ) \
                $(FUZZ_REDIFF_OBJ) $(RE_REF_OBJ) \
                $(PTY_ORACLE_OBJ) \
                $(PTY_HARNESS_OBJ) $(PTY_REGISTRY_OBJ) $(PTY_RUNNER_OBJ) \
                $(PTY_DEMO_OBJ) $(PERF_UNICODE_OBJ) $(PERF_RENDER_OBJ) \
                $(PERF_PIECE_OBJ) $(PERF_CURSOR_OBJ) $(PERF_UNDO_OBJ) \
                $(PERF_TEXTBUF_OBJ) $(PERF_LATENCY_OBJ) \
                $(PERF_JOBSTREAM_OBJ) $(PERF_REPATH_OBJ) \
                $(PERF_RETHRU_OBJ) \
                $(LIVE_PTY_OBJ) \
                $(PERF_UNITS_OBJ) \
                $(PERF_MULTICURSOR_OBJ) \
                $(PERF_CMDCOMP_OBJ) \
                $(PERF_STATE_OBJ) \
                $(PERF_FINDER_OBJ) $(PERF_MOUSE_OBJ) \
                $(GEN_BIGFILE_OBJ) $(FLETCH_RUN_OBJ) $(SCRIPT_RUNNER_OBJ) \
                $(ROUNDTRIP_OBJ) \
                $(PERF_FLETCH_OBJ) $(PERF_RECORD_OBJ) $(PERF_BATCH_OBJ) \
                $(PERF_SCRIPT_SUITE_OBJ) \
                $(FUZZ_RECORD_OBJ) \
                $(TORTURE_CHILD_OBJ) \
                $(TORTURE_DRIVER_OBJ) $(TORTURE_LIVE_OBJ) \
                $(TORTURE_BATCH_OBJ) $(FAULTSHIM)))

# A content mismatch makes FORCE a normal prerequisite of every object built
# by this invocation.  The stamp recipe also removes objects not reachable
# from the requested target (notably main.o during `make test`), so a later
# target cannot reuse macros from the previous module selection.
STAMP_MODULES := $(file <$(BUILD)/mods.stamp)
ifneq ($(STAMP_MODULES),$(MODULES))
MODULE_FORCE := FORCE
endif

.DEFAULT_GOAL := all
.PHONY: all check test clean install dirs FORCE test-script \
        test-script-determinism test-script-budget test-pty fuzz \
        fuzz-textbuf fuzz-units fuzz-multicursor fuzz-cmdparse fuzz-long \
        fuzz-mouse fuzz-groups fuzz-record test-record-corpus \
        fixtures fixtures-quick fixtures-verify \
        fixtures-verify-quick \
        unicode-tables perf perf-unicode perf-render perf-piece perf-cursor \
        perf-units perf-multicursor perf-cmdcomp perf-state perf-finder \
        perf-mouse perf-record \
        perf-batch perf-batch-selftest \
        perf-undo perf-textbuf perf-huge perf-update perf-baseline-guard \
        perf-gate-selftest perf-latency perf-latency-selftest \
        torture torture-build torture-live-check torture-batch \
        fl-perf-smoke fl-dispatch-parity fl-gc-stress \
        test-fletch test-roundtrip test-roundtrip-coverage \
        test-fletch-roundtrip fletch-ledger \
        bench-fletch \
        test-fletch-dispatch test-fletch-gc-stress test-fletch-determinism

all: $(BUILD)/sagitta $(BUILD)/sag

$(BUILD)/sagitta: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/sag: $(BUILD)/sagitta
	ln -sf sagitta $@

$(BUILD)/unit_tests: $(UNIT_LINK_OBJ) $(FAKECLIP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UNIT_LINK_OBJ) $(LDLIBS)

$(BUILD)/pty_runner: $(PTY_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PTY_LINK_OBJ) $(LDLIBS)

$(BUILD)/demo_paint: $(PTY_DEMO_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PTY_DEMO_LINK_OBJ) $(LDLIBS)

$(BUILD)/fuzz_utf8: $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_UTF8_OBJ) $(LDLIBS)

$(BUILD)/fuzz_grapheme: $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_GRAPHEME_OBJ) $(LDLIBS)

$(BUILD)/fuzz_input: $(FUZZ_LINK_OBJ) $(FUZZ_INPUT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_INPUT_OBJ) $(LDLIBS)

$(BUILD)/fuzz_grid: $(FUZZ_LINK_OBJ) $(FUZZ_GRID_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(FUZZ_GRID_OBJ) $(LDLIBS)

$(BUILD)/fuzz_vt: $(FUZZ_LINK_OBJ) $(PTY_VT_OBJ) $(FUZZ_VT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) $(PTY_VT_OBJ) \
		$(FUZZ_VT_OBJ) $(LDLIBS)

$(BUILD)/fuzz_undo: $(FUZZ_CORE_OBJ) $(FUZZ_UNDO_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) $(FUZZ_UNDO_OBJ) $(LDLIBS)

$(BUILD)/fuzz_textbuf: $(FUZZ_CORE_OBJ) $(TEXT_FUZZ_SUPPORT_OBJ) \
                       $(FUZZ_TEXTBUF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) \
		$(TEXT_FUZZ_SUPPORT_OBJ) $(FUZZ_TEXTBUF_OBJ) $(LDLIBS)

$(BUILD)/fuzz_units: $(FUZZ_CORE_OBJ) $(FUZZ_UNITS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) $(FUZZ_UNITS_OBJ) $(LDLIBS)

$(BUILD)/fuzz_multicursor: $(FUZZ_LINK_OBJ) $(FUZZ_MULTICURSOR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_MULTICURSOR_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fl_lex: $(FUZZ_LINK_OBJ) $(FUZZ_FLLEX_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLLEX_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fl_parse: $(FUZZ_LINK_OBJ) $(FUZZ_FLPARSE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLPARSE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fl_vm: $(FUZZ_LINK_OBJ) $(FUZZ_FLVM_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLVM_OBJ) $(LDLIBS)

$(BUILD)/fuzz_flapi: $(FUZZ_LINK_OBJ) $(FUZZ_FLAPI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLAPI_OBJ) $(LDLIBS)

$(BUILD)/fuzz_record: $(FUZZ_LINK_OBJ) $(FUZZ_RECORD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_RECORD_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fl_std: $(FUZZ_LINK_OBJ) $(FUZZ_FLSTD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLSTD_OBJ) $(LDLIBS)

$(BUILD)/fuzz_tabs: $(FUZZ_LINK_OBJ) $(FUZZ_TABS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_TABS_OBJ) $(LDLIBS)

$(BUILD)/fuzz_groups: $(FUZZ_LINK_OBJ) $(FUZZ_GROUPS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_GROUPS_OBJ) $(LDLIBS)

$(BUILD)/fuzz_panes: $(FUZZ_LINK_OBJ) $(FUZZ_PANES_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_PANES_OBJ) $(LDLIBS)

$(BUILD)/fuzz_search: $(FUZZ_LINK_OBJ) $(FUZZ_SEARCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_SEARCH_OBJ) $(LDLIBS)

$(BUILD)/fuzz_re_quote: $(FUZZ_LINK_OBJ) $(FUZZ_REQUOTE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_REQUOTE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_re_compile: $(FUZZ_LINK_OBJ) $(FUZZ_RECOMPILE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_RECOMPILE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_re_diff: $(FUZZ_LINK_OBJ) $(FUZZ_REDIFF_OBJ) $(RE_REF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_REDIFF_OBJ) $(RE_REF_OBJ) $(LDLIBS)

$(BUILD)/fuzz_cmdparse: $(FUZZ_LINK_OBJ) $(FUZZ_CMDPARSE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_CMDPARSE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fuzzy: $(FUZZ_LINK_OBJ) $(FUZZ_FUZZY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FUZZY_OBJ) $(LDLIBS)

$(BUILD)/fuzz_state: $(FUZZ_LINK_OBJ) $(FUZZ_STATE_OBJ) $(STATE_LEGACY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_STATE_OBJ) $(STATE_LEGACY_OBJ) $(LDLIBS)

$(BUILD)/fuzz_gitignore: $(FUZZ_LINK_OBJ) $(FUZZ_GITIGNORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_GITIGNORE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_mouse: $(FUZZ_LINK_OBJ) $(FUZZ_MOUSE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_MOUSE_OBJ) $(LDLIBS)

$(BUILD)/gen-bigfile: $(GEN_BIGFILE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(GEN_BIGFILE_OBJ) $(LDLIBS)

$(BUILD)/perf_fletch: $(PERF_CORE_OBJ) $(PERF_FLETCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_FLETCH_OBJ) $(LDLIBS)

$(BUILD)/perf_record: $(PERF_CORE_OBJ) $(PERF_RECORD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_RECORD_OBJ) $(LDLIBS)

$(BUILD)/perf_batch: $(PERF_BATCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_BATCH_OBJ) $(LDLIBS)

$(BUILD)/perf_script_suite: $(PERF_SCRIPT_SUITE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_SCRIPT_SUITE_OBJ) $(LDLIBS)

$(BUILD)/fletch_run: $(FLETCH_CORE_OBJ) $(FLETCH_RUN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FLETCH_CORE_OBJ) \
		$(FLETCH_RUN_OBJ) $(LDLIBS)

$(BUILD)/script_runner: $(SCRIPT_RUNNER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCRIPT_RUNNER_OBJ) $(LDLIBS)

$(BUILD)/roundtrip_runner: $(ROUNDTRIP_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ROUNDTRIP_LINK_OBJ) $(LDLIBS)

$(BUILD)/perf_textbuf: $(PERF_CORE_OBJ) $(PERF_TEXTBUF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_TEXTBUF_OBJ) $(LDLIBS)

$(BUILD)/perf_unicode: $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ) $(LDLIBS)

$(BUILD)/perf_render: $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ) $(LDLIBS)

$(BUILD)/perf_scroll: $(PERF_CORE_OBJ) $(PERF_SCROLL_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_SCROLL_OBJ) $(LDLIBS)

$(BUILD)/perf_piece: $(PERF_CORE_OBJ) $(PERF_PIECE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_PIECE_OBJ) $(LDLIBS)

$(BUILD)/perf_cursor: $(PERF_CORE_OBJ) $(PERF_CURSOR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_CURSOR_OBJ) $(LDLIBS)

$(BUILD)/perf_undo: $(PERF_CORE_OBJ) $(PERF_UNDO_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNDO_OBJ) $(LDLIBS)

$(BUILD)/perf_latency: $(PERF_LATENCY_OBJ) $(LIVE_PTY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_LATENCY_OBJ) \
		$(LIVE_PTY_OBJ) $(LDLIBS)

$(BUILD)/perf_re_throughput: $(PERF_RETHRU_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_RETHRU_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_search_latency: $(PERF_SEARCHLAT_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_SEARCHLAT_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_re_pathological: $(PERF_REPATH_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_REPATH_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_jobstream: $(PERF_JOBSTREAM_OBJ) $(LIVE_PTY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_JOBSTREAM_OBJ) \
		$(LIVE_PTY_OBJ) $(LDLIBS)

$(BUILD)/perf_units: $(PERF_CORE_OBJ) $(PERF_UNITS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNITS_OBJ) $(LDLIBS)

$(BUILD)/perf_multicursor: $(PERF_CORE_OBJ) $(PERF_MULTICURSOR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_MULTICURSOR_OBJ) $(LDLIBS)

$(BUILD)/perf_cmdcomp: $(PERF_CORE_OBJ) $(PERF_CMDCOMP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_CMDCOMP_OBJ) $(LDLIBS)

$(BUILD)/fl_smoke: $(PERF_CORE_OBJ) $(FL_SMOKE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(FL_SMOKE_OBJ) $(LDLIBS)

$(BUILD)/perf_state: $(PERF_CORE_OBJ) $(PERF_STATE_OBJ) $(STATE_LEGACY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_STATE_OBJ) $(STATE_LEGACY_OBJ) $(LDLIBS)

$(BUILD)/perf_mouse: $(PERF_CORE_OBJ) $(PERF_MOUSE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_MOUSE_OBJ) $(LDLIBS)

$(BUILD)/perf_finder: $(PERF_CORE_OBJ) $(PERF_FINDER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_FINDER_OBJ) $(LDLIBS)

$(TORTURE_CHILD): $(TORTURE_CORE_OBJ) $(TORTURE_CHILD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_CORE_OBJ) \
		$(TORTURE_CHILD_OBJ) $(LDLIBS)

$(TORTURE_DRIVER): $(TORTURE_DRIVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_DRIVER_OBJ) $(LDLIBS)

$(TORTURE_LIVE): $(TORTURE_LIVE_OBJ) $(LIVE_PTY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_LIVE_OBJ) \
		$(LIVE_PTY_OBJ) $(LDLIBS)

$(TORTURE_BATCH): $(TORTURE_BATCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TORTURE_BATCH_OBJ) $(LDLIBS)

$(FAULTSHIM): tests/torture/faultshim.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC $(SHARED_FLAG) -o $@ $< \
		$(DL_LIBS) $(LDLIBS)

$(BUILD)/gen-unicode-tables: scripts/gen-unicode-tables.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(FAKECLIP): tests/unit/fakeclip.c | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

#
# THE FAST TIER -- what to run before every commit.
#
# `make test` is the thorough one and costs several minutes, most of it
# the 206-case pty suite (132 s) and the torture live-check.  Running
# that after every edit is how an afternoon disappears, so this is the
# inner loop: the unit suite, the Fletch conformance corpus and every
# grep-style gate, which together take about 65 s and catch the large
# majority of ordinary breakage.
#
# WHAT IT DELIBERATELY OMITS, so nobody mistakes green here for green:
# the pty goldens (any rendering or input change needs `make test-pty`),
# the torture live-check, the sanitizer lanes, and valgrind.  Run the
# full `make test` before pushing a chunk, not after every commit -- and
# see the valgrind job in .github/workflows/ci.yml for when that lane
# has to be asked for by hand.
#
check: $(BUILD)/unit_tests $(BUILD)/sagitta test-fletch test-script
	$(UNIT_RUN)
	scripts/bans.sh
	scripts/check-cmd-dispatch.sh
	scripts/check-fl-choke.sh
	scripts/check-input.sh
	scripts/check-render.sh
	scripts/check-sigsafe.sh
	scripts/smoke.sh $(BUILD)/sagitta
	@echo "check: ok (fast tier -- pty, torture, sanitizers and valgrind NOT run)"

test: $(BUILD)/unit_tests $(BUILD)/sagitta test-pty test-fletch test-script \
      test-roundtrip test-record-corpus torture-build
	$(UNIT_RUN)
	scripts/bans.sh
	scripts/check-cmd-dispatch.sh
	scripts/check-fl-choke.sh
	scripts/check-input.sh
	scripts/check-render.sh
	scripts/check-sigsafe.sh
	scripts/smoke.sh $(BUILD)/sagitta
	$(MAKE) --no-print-directory torture-live-check BUILD=$(BUILD) \
		CC=$(CC) SAN=$(SAN) VALGRIND=$(VALGRIND)

fuzz: $(BUILD)/fuzz_utf8 $(BUILD)/fuzz_grapheme $(BUILD)/fuzz_input \
      $(BUILD)/fuzz_grid $(BUILD)/fuzz_vt $(BUILD)/fuzz_undo \
      $(BUILD)/fuzz_re_compile $(BUILD)/fuzz_re_diff \
      $(BUILD)/fuzz_re_quote $(BUILD)/fuzz_search \
      $(BUILD)/fuzz_panes $(BUILD)/fuzz_tabs \
      $(BUILD)/fuzz_fuzzy $(BUILD)/fuzz_state \
      $(BUILD)/fuzz_gitignore \
      $(BUILD)/fuzz_fl_lex $(BUILD)/fuzz_fl_parse \
      $(BUILD)/fuzz_fl_std $(BUILD)/fuzz_fl_vm \
      $(BUILD)/fuzz_flapi \
      fuzz-textbuf fuzz-units fuzz-multicursor fuzz-cmdparse \
      fuzz-mouse fuzz-groups fuzz-record
	$(BUILD)/fuzz_utf8 --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_grapheme --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_input --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_grid --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_vt --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_undo --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_re_compile --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_re_quote --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_search --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_panes --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_tabs --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_re_diff --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_fuzzy --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_state --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_gitignore --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_fl_lex --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_fl_parse --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_fl_std --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_fl_vm --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_flapi --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	@if [ -n "$(FUZZ_SECONDS)" ]; then \
		$(BUILD)/fuzz_input --seconds=$(FUZZ_SECONDS) --seed=$(FUZZ_SEED); \
	fi

fuzz-groups: $(BUILD)/fuzz_groups
	@set -eu; \
	for seed in $(GROUPS_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_groups --iters=$(GROUPS_FUZZ_SESSIONS) \
			--seed=$$seed; \
	done

fuzz-mouse: $(BUILD)/fuzz_mouse
	@set -eu; \
	iters=$$(( ($(MOUSE_FUZZ_EVENTS) + 24999) / 25000 )); \
	for seed in $(MOUSE_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_mouse --iters=$$iters --seed=$$seed; \
	done

fuzz-units: $(BUILD)/fuzz_units
	@set -eu; \
	for seed in $(UNITS_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_units --iters=$(FUZZ_ITERS) --seed=$$seed; \
	done

# Sanitizer contention can push a valid case past fuzzlib's per-input
# watchdog, so instrumented seeds run serially while the plain lane stays
# parallel.
fuzz-multicursor: $(BUILD)/fuzz_multicursor
	@set -eu; \
	iters=$$(( ($(MULTICURSOR_FUZZ_OPS) + 127) / 128 )); \
	if [ "$(SAN)" = "1" ]; then \
		for seed in $(MULTICURSOR_FUZZ_SEEDS); do \
			$(BUILD)/fuzz_multicursor --iters=$$iters \
				--seed=$$seed; \
		done; \
	else \
		pids=""; \
		for seed in $(MULTICURSOR_FUZZ_SEEDS); do \
			$(BUILD)/fuzz_multicursor --iters=$$iters \
				--seed=$$seed & \
			pids="$$pids $$!"; \
		done; \
		status=0; \
		for pid in $$pids; do \
			wait $$pid || status=1; \
		done; \
		exit $$status; \
	fi

fuzz-cmdparse: $(BUILD)/fuzz_cmdparse
	$(BUILD)/fuzz_cmdparse --iters=$(CMDPARSE_FUZZ_ITERS) \
		--seed=$(FUZZ_SEED)

fuzz-record: $(BUILD)/fuzz_record
	$(BUILD)/fuzz_record --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

test-record-corpus: $(BUILD)/fuzz_record
	$(BUILD)/fuzz_record --corpus-only

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

perf-scroll: $(BUILD)/perf_scroll
	$(BUILD)/perf_scroll

perf-piece: $(BUILD)/perf_piece
	$(BUILD)/perf_piece

perf: perf-unicode perf-render perf-scroll perf-piece perf-cursor perf-undo perf-textbuf \
      perf-latency perf-jobstream perf-re-pathological \
      perf-re-throughput perf-search-latency \
      perf-units perf-multicursor perf-cmdcomp perf-state perf-finder \
      perf-mouse perf-record perf-batch

perf-cursor: $(BUILD)/perf_cursor
	$(BUILD)/perf_cursor

perf-undo: $(BUILD)/perf_undo
	$(BUILD)/perf_undo

perf-units: $(BUILD)/perf_units
	$(BUILD)/perf_units

perf-multicursor: $(BUILD)/perf_multicursor
	$(BUILD)/perf_multicursor

perf-cmdcomp: $(BUILD)/perf_cmdcomp
	$(BUILD)/perf_cmdcomp

perf-record: $(BUILD)/perf_record
	$(BUILD)/perf_record $(if $(PERF_GATE),--gate,)

perf-batch: $(BUILD)/perf_batch $(BUILD)/sagitta
	$(BUILD)/perf_batch --sagitta $(abspath $(BUILD)/sagitta) --gate

perf-batch-selftest: $(BUILD)/perf_batch $(BUILD)/sagitta
	@if SAG_BATCH_INJECT_NS=12000000 $(BUILD)/perf_batch \
		--sagitta $(abspath $(BUILD)/sagitta) --gate; then \
		echo 'error: batch startup gate accepted injected delay' >&2; \
		exit 1; \
	else \
		echo 'perf-batch-selftest: injected delay rejected'; \
	fi

#
# Sprint 30 DoD 12: the Fletch perf smoke.  NUMBERS ONLY -- there is no
# comparison against a baseline here, because s33 owns the gates and a
# gate invented a sprint early is a gate tuned to this laptop.
#
fl-perf-smoke: $(BUILD)/fl_smoke
	$(BUILD)/fl_smoke --perf

#
# Sprint 30 DoD 7: the GC-stress lane.
#
# Runs the WHOLE unit suite with the collector firing at every
# instruction boundary, which is what turns a handle-protection
# violation from a once-a-year wrong answer into a failure on the test
# that exercises it.  ~30x slower than the plain lane, so it is never
# part of `make test`.
#
# Under the sanitizers as well, deliberately: stress makes the
# use-after-free happen and ASan is what names the line it happened on.
# One without the other finds much less.
#
fl-gc-stress: $(UNIT_RUN)
	SAG_FL_GC_STRESS=1 $(UNIT_RUN) $(UNIT_DEATH_EXCLUDES)

#
# Sprint 30 DoD 5: the differential-dispatch gate.
#
# Builds the trace driver under BOTH dispatch settings and byte-compares
# the executed-opcode streams.  The two builds cannot share $(BUILD):
# their objects differ by a -D, and mixing them produces a binary whose
# dispatch mode depends on what happened to be stale.
#
# The `# dispatch:` header is the one line allowed to differ, so it is
# stripped before the diff rather than special-cased inside the diff.
#
fl-dispatch-parity:
	@rm -rf $(BUILD)-fl-sw $(BUILD)-fl-cg
	$(MAKE) --no-print-directory BUILD=$(BUILD)-fl-sw FL_CGOTO=0 \
		CFLAGS_FL_TRACE=1 $(BUILD)-fl-sw/fl_smoke
	$(MAKE) --no-print-directory BUILD=$(BUILD)-fl-cg FL_CGOTO=1 \
		CFLAGS_FL_TRACE=1 $(BUILD)-fl-cg/fl_smoke
	$(BUILD)-fl-sw/fl_smoke --trace | grep -v '^# dispatch:' \
		> $(BUILD)-fl-sw/trace.txt
	$(BUILD)-fl-cg/fl_smoke --trace | grep -v '^# dispatch:' \
		> $(BUILD)-fl-cg/trace.txt
	@if diff -u $(BUILD)-fl-sw/trace.txt $(BUILD)-fl-cg/trace.txt; then \
		echo "fl-dispatch-parity: traces identical"; \
	else \
		echo "fl-dispatch-parity: THE TWO DISPATCHERS DISAGREE"; \
		exit 1; \
	fi
	@#
	@# Sprint 30 DoD 6, the half a single-process test cannot make:
	@# map iteration order stable ACROSS PROCESSES.  The corpus walks
	@# maps and the trace records what that produced, so running the
	@# same binary a second time and comparing catches an order that
	@# depends on an address, a hash seed or an allocation sequence --
	@# none of which repeat across a fork.
	@#
	$(BUILD)-fl-sw/fl_smoke --trace | grep -v '^# dispatch:' \
		> $(BUILD)-fl-sw/trace2.txt
	@if diff -u $(BUILD)-fl-sw/trace.txt $(BUILD)-fl-sw/trace2.txt; then \
		echo "fl-dispatch-parity: stable across processes"; \
	else \
		echo "fl-dispatch-parity: NOT DETERMINISTIC ACROSS RUNS"; \
		exit 1; \
	fi

perf-state: $(BUILD)/perf_state
	$(BUILD)/perf_state

perf-finder: $(BUILD)/perf_finder
	$(BUILD)/perf_finder

perf-mouse: $(BUILD)/perf_mouse
	$(BUILD)/perf_mouse

perf-latency: $(BUILD)/perf_latency $(BUILD)/sagitta
	$(BUILD)/perf_latency --sagitta $(abspath $(BUILD)/sagitta) \
		--baseline $(LATENCY_BASELINE)

perf-re-pathological: $(BUILD)/perf_re_pathological
	$(BUILD)/perf_re_pathological --baseline $(PERF_BASELINE)

perf-re-throughput: $(BUILD)/perf_re_throughput
	$(BUILD)/perf_re_throughput --baseline $(PERF_BASELINE)

perf-search-latency: $(BUILD)/perf_search_latency
	$(BUILD)/perf_search_latency --baseline $(PERF_BASELINE)

perf-jobstream: $(BUILD)/perf_jobstream $(BUILD)/sagitta
	$(BUILD)/perf_jobstream --sagitta $(abspath $(BUILD)/sagitta) \
		--baseline $(LATENCY_BASELINE)

# Proves the gate reacts: an injected paint delay must fail it.
perf-jobstream-selftest: $(BUILD)/perf_jobstream $(BUILD)/sagitta
	@if SAG_JOBSTREAM_KEYS=60 SAG_JOBSTREAM_INJECT_NS=6000000 \
		$(BUILD)/perf_jobstream --sagitta $(abspath $(BUILD)/sagitta) \
		--baseline $(LATENCY_BASELINE); then \
		echo 'error: jobstream gate accepted injected delay' >&2; \
		exit 1; \
	else \
		echo 'jobstream selftest: injected delay correctly rejected'; \
	fi

perf-latency-selftest: $(BUILD)/perf_latency $(BUILD)/sagitta
	@if SAG_LATENCY_KEYS=100 SAG_LATENCY_INJECT_NS=6000000 \
		$(BUILD)/perf_latency --sagitta $(abspath $(BUILD)/sagitta) \
		--baseline $(LATENCY_BASELINE); then \
		echo 'error: latency gate accepted injected paint delay' >&2; \
		exit 1; \
	else \
		echo 'perf-latency-selftest: injected delay rejected'; \
	fi

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

torture-build: $(BUILD)/sagitta $(TORTURE_CHILD) $(TORTURE_LIVE) \
               $(TORTURE_DRIVER) $(TORTURE_BATCH) $(FAULTSHIM)

torture-live-check: torture-build
	SAG_TORTURE_CLEAN_ONLY=1 \
	SAG_TORTURE_CHECKER=$(abspath $(TORTURE_CHILD)) \
	SAG_TORTURE_SAGITTA=$(abspath $(BUILD)/sagitta) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) --trace-children=yes,) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_LIVE)) \
		$(abspath $(FAULTSHIM))

torture-batch: torture-build
	$(TORTURE_BATCH) --sagitta $(abspath $(BUILD)/sagitta) \
		--checker $(abspath $(TORTURE_CHILD))

torture: torture-build
	SAG_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_CHILD)) \
		$(abspath $(FAULTSHIM))
	SAG_TORTURE_CHECKER=$(abspath $(TORTURE_CHILD)) \
	SAG_TORTURE_SAGITTA=$(abspath $(BUILD)/sagitta) \
	SAG_TORTURE_LANE=live-editor \
	SAG_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_LIVE)) \
		$(abspath $(FAULTSHIM))
	$(TORTURE_BATCH) --sagitta $(abspath $(BUILD)/sagitta) \
		--checker $(abspath $(TORTURE_CHILD))

unicode-tables: $(BUILD)/gen-unicode-tables
	$< ucd/16.0.0 > src/unicode/tables.c
	$< --word-break ucd/16.0.0 > src/unicode/tables_wb.c
	$< --case ucd/16.0.0 > src/unicode/tables_case.c
	$< --category ucd/16.0.0 > src/unicode/tables_cat.c

# Check the literal selection on every invocation, but preserve the stamp's
# mtime when it is unchanged so objects are not rebuilt spuriously.
$(BUILD)/mods.stamp: FORCE | dirs
	@if ! printf '%s\n' '$(MODULES)' | cmp -s - $@; then \
		rm -f $(OBJ) $(OBJ:.o=.d) $(UNIT_OBJ) $(UNIT_OBJ:.o=.d); \
		printf '%s\n' '$(MODULES)' > $@; \
	fi

# The one file allowed out of the C11 box, and only when the
# computed-goto dispatcher is on.  Deferred to here so it overrides the
# FINAL CFLAGS: a target-specific `:=` earlier in the file would snapshot
# the value before the sanitizer lane has added its own flags.
ifeq ($(FL_CGOTO),1)
$(BUILD)/src/fl/vm.o: CFLAGS := \
  $(subst -std=c11,-std=gnu11,$(CFLAGS)) -Wno-pedantic
endif

$(BUILD)/%.o: %.c $(BUILD)/mods.stamp $(MODULE_FORCE) | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

$(STATE_LEGACY_OBJ): src/ws/state_legacy.c src/ws/fl_parse.c \
                     src/ws/fl_emit.c $(BUILD)/mods.stamp | dirs
	$(CC) $(CFLAGS) -DSAG_STATE_LEGACY=1 -c -o $@ $<

dirs:
	mkdir -p $(BUILD_DIRS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD)/sagitta $(DESTDIR)$(PREFIX)/bin/sagitta
	ln -sf sagitta $(DESTDIR)$(PREFIX)/bin/sag
	install -d $(DESTDIR)$(PREFIX)/share/sagitta/runtime
	install -m 0644 runtime/init.fl \
		$(DESTDIR)$(PREFIX)/share/sagitta/runtime/init.fl

clean:
	rm -rf $(BUILD)

test-script: $(BUILD)/script_runner $(BUILD)/sagitta
	LC_ALL=C SAG_SCRIPT_BUDGET_MS=$(SAG_SCRIPT_BUDGET_MS) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) \
		--trace-children=yes \
		$(VALGRIND_TRACE_SKIP),) \
		$(BUILD)/script_runner --selftest \
		--sagitta $(abspath $(BUILD)/sagitta)
	LC_ALL=C SAG_SCRIPT_BUDGET_MS=$(SAG_SCRIPT_BUDGET_MS) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) \
		--trace-children=yes \
		$(VALGRIND_TRACE_SKIP),) \
		$(BUILD)/script_runner \
		--sagitta $(abspath $(BUILD)/sagitta)

test-script-determinism: $(BUILD)/script_runner $(BUILD)/sagitta
	@tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	LC_ALL=C $(BUILD)/script_runner --selftest \
		--sagitta $(abspath $(BUILD)/sagitta) >"$$tmp/run-1" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner \
		--sagitta $(abspath $(BUILD)/sagitta) >>"$$tmp/run-1" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner --selftest \
		--sagitta $(abspath $(BUILD)/sagitta) >"$$tmp/run-2" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner \
		--sagitta $(abspath $(BUILD)/sagitta) >>"$$tmp/run-2" 2>&1; \
	diff -u "$$tmp/run-1" "$$tmp/run-2"; \
	echo 'test-script-determinism: ok'

test-script-budget: $(BUILD)/perf_script_suite $(BUILD)/script_runner \
                    $(BUILD)/sagitta $(SCRIPT_SUITE_BASELINE)
	$(BUILD)/perf_script_suite --selftest
	LC_ALL=C $(BUILD)/perf_script_suite \
		--runner $(abspath $(BUILD)/script_runner) \
		--sagitta $(abspath $(BUILD)/sagitta) \
		--baseline $(SCRIPT_SUITE_BASELINE)

# The conformance suite (Sprint 33).  LC_ALL=C is set rather than
# assumed: run.c sorts with strcmp and the ledger is byte-compared.
# The RUNNER goes under valgrind; the `sag fl` children deliberately do
# not (no --trace-children).  Wrapping 37 subprocess spawns would turn a
# 0.2 s lane into minutes for a check the sanitize lane already makes
# with ASan -- and the runner is the part with the file descriptors and
# the allocation bookkeeping that --track-fds and --leak-check exist to
# police.
test-fletch: $(BUILD)/fletch_run $(BUILD)/sagitta
	LC_ALL=C $(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN),) \
		$(BUILD)/fletch_run --sagitta $(abspath $(BUILD)/sagitta)
	BUILD=$(BUILD) scripts/check-fletch-coverage.sh
	BUILD=$(BUILD) scripts/check-fletch-meta.sh
	BUILD=$(BUILD) scripts/check-fletch-gate-selftest.sh

# PERF_GATE=1 enforces; every other lane runs it ungated, because a
# bench that is never executed outside the perf runner rots.
BASELINE ?= dev
bench-fletch: $(BUILD)/perf_fletch
	$(BUILD)/perf_fletch --selftest-gate
	$(BUILD)/perf_fletch --baseline $(BASELINE) \
		$(if $(PERF_GATE),--gate,--gate-budgets)

fletch-ledger: $(BUILD)/fletch_run $(BUILD)/sagitta
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--sagitta $(abspath $(BUILD)/sagitta) >tests/fletch/ledger.txt

# DoD 11: the whole suite under both dispatchers, outputs byte-compared.
#
# Separate BUILD dirs, both from clean.  A reused build directory
# produces deterministic FALSE results here -- half the objects come
# from the other setting and the two binaries are then the same binary.
test-fletch-dispatch:
	@rm -rf $(BUILD)-flc-sw $(BUILD)-flc-cg
	$(MAKE) --no-print-directory BUILD=$(BUILD)-flc-sw FL_CGOTO=0 \
		$(BUILD)-flc-sw/sagitta $(BUILD)-flc-sw/fletch_run
	$(MAKE) --no-print-directory BUILD=$(BUILD)-flc-cg FL_CGOTO=1 \
		$(BUILD)-flc-cg/sagitta $(BUILD)-flc-cg/fletch_run
	LC_ALL=C $(BUILD)-flc-sw/fletch_run \
		--sagitta $(abspath $(BUILD)-flc-sw/sagitta) \
		>$(BUILD)-flc-sw/out.txt 2>&1
	LC_ALL=C $(BUILD)-flc-cg/fletch_run \
		--sagitta $(abspath $(BUILD)-flc-cg/sagitta) \
		>$(BUILD)-flc-cg/out.txt 2>&1
	cmp $(BUILD)-flc-sw/out.txt $(BUILD)-flc-cg/out.txt
	@echo 'test-fletch-dispatch: both dispatchers agree, byte for byte'

# DoD 11: the whole suite under the collector's stress mode, minus any
# file carrying a justified `# GC_STRESS: 0`.  The runner prints and
# asserts the opt-out count, so the escape hatch cannot become the norm.
test-fletch-gc-stress: $(BUILD)/fletch_run $(BUILD)/sagitta
	SAG_FL_GC_STRESS=1 LC_ALL=C $(BUILD)/fletch_run \
		--sagitta $(abspath $(BUILD)/sagitta)

# Determinism: the suite twice and the ledger twice, byte-compared.
test-fletch-determinism: $(BUILD)/fletch_run $(BUILD)/sagitta
	LC_ALL=C $(BUILD)/fletch_run --sagitta $(abspath $(BUILD)/sagitta) \
		>$(BUILD)/fletch-run-1.txt 2>&1
	LC_ALL=C $(BUILD)/fletch_run --sagitta $(abspath $(BUILD)/sagitta) \
		>$(BUILD)/fletch-run-2.txt 2>&1
	cmp $(BUILD)/fletch-run-1.txt $(BUILD)/fletch-run-2.txt
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--sagitta $(abspath $(BUILD)/sagitta) >$(BUILD)/fletch-led-1.txt
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--sagitta $(abspath $(BUILD)/sagitta) >$(BUILD)/fletch-led-2.txt
	cmp $(BUILD)/fletch-led-1.txt $(BUILD)/fletch-led-2.txt
	@echo 'test-fletch-determinism: two runs identical, ledger stable'

test-roundtrip: $(BUILD)/roundtrip_runner
	SAG_RT_TMP=$(BUILD)/tmp LC_ALL=C $(BUILD)/roundtrip_runner

test-roundtrip-coverage: $(BUILD)/roundtrip_runner
	SAG_RT_TMP=$(BUILD)/tmp LC_ALL=C $(BUILD)/roundtrip_runner --coverage

test-fletch-roundtrip: test-roundtrip

test-pty: $(BUILD)/pty_runner $(BUILD)/demo_paint $(BUILD)/sagitta
	$(PTY_PREP) $(PTY_RUN) --demo $(abspath $(BUILD)/demo_paint) \
		--sagitta $(abspath $(BUILD)/sagitta) $(PTY_LOG_REDIRECT)

-include $(OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(STATE_LEGACY_OBJ:.o=.d) \
         $(FUZZ_LIB_OBJ:.o=.d) \
         $(FUZZ_UTF8_OBJ:.o=.d) $(FUZZ_GRAPHEME_OBJ:.o=.d) \
         $(FUZZ_INPUT_OBJ:.o=.d) $(FUZZ_GRID_OBJ:.o=.d) \
         $(FUZZ_VT_OBJ:.o=.d) $(FUZZ_UNDO_OBJ:.o=.d) \
         $(FUZZ_TEXTBUF_OBJ:.o=.d) $(TEXT_FUZZ_SUPPORT_OBJ:.o=.d) \
         $(FUZZ_MULTICURSOR_OBJ:.o=.d) \
         $(FUZZ_FLAPI_OBJ:.o=.d) \
         $(FUZZ_CMDPARSE_OBJ:.o=.d) $(FUZZ_RECOMPILE_OBJ:.o=.d) \
         $(FUZZ_REDIFF_OBJ:.o=.d) $(RE_REF_OBJ:.o=.d) \
         $(PTY_ORACLE_OBJ:.o=.d) \
         $(PTY_HARNESS_OBJ:.o=.d) $(PTY_REGISTRY_OBJ:.o=.d) \
         $(PTY_RUNNER_OBJ:.o=.d) $(PTY_DEMO_OBJ:.o=.d) \
         $(FLETCH_RUN_OBJ:.o=.d) $(SCRIPT_RUNNER_OBJ:.o=.d) \
         $(ROUNDTRIP_OBJ:.o=.d) \
         $(PERF_UNICODE_OBJ:.o=.d) $(PERF_RENDER_OBJ:.o=.d) \
         $(PERF_PIECE_OBJ:.o=.d) $(PERF_CURSOR_OBJ:.o=.d) \
         $(PERF_UNDO_OBJ:.o=.d) $(PERF_TEXTBUF_OBJ:.o=.d) \
         $(PERF_LATENCY_OBJ:.o=.d) $(PERF_JOBSTREAM_OBJ:.o=.d) \
         $(PERF_REPATH_OBJ:.o=.d) $(PERF_RETHRU_OBJ:.o=.d) \
         $(LIVE_PTY_OBJ:.o=.d) \
         $(PERF_MULTICURSOR_OBJ:.o=.d) \
         $(PERF_CMDCOMP_OBJ:.o=.d) \
         $(PERF_STATE_OBJ:.o=.d) \
         $(PERF_FINDER_OBJ:.o=.d) $(PERF_MOUSE_OBJ:.o=.d) \
         $(PERF_FLETCH_OBJ:.o=.d) $(PERF_BATCH_OBJ:.o=.d) \
         $(PERF_SCRIPT_SUITE_OBJ:.o=.d) \
         $(GEN_BIGFILE_OBJ:.o=.d) \
         $(TORTURE_CHILD_OBJ:.o=.d) \
	 $(TORTURE_DRIVER_OBJ:.o=.d) $(TORTURE_LIVE_OBJ:.o=.d) \
	 $(TORTURE_BATCH_OBJ:.o=.d)

FORCE:
