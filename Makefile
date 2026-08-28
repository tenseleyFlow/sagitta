HOST_OS := $(shell uname -s)
HOST_ARCH := $(shell uname -m)
HOST_LIBC := $(strip $(shell \
	if [ '$(HOST_OS)' = Linux ]; then \
		if getconf GNU_LIBC_VERSION >/dev/null 2>&1; then printf '%s' gnu; \
		elif ldd --version 2>&1 | grep -qi musl; then printf '%s' musl; fi; \
	fi))
HOST_TARGET := $(strip $(if $(filter Linux,$(HOST_OS)),\
                 $(if $(filter x86_64,$(HOST_ARCH)),\
                   $(if $(HOST_LIBC),x86_64-linux-$(HOST_LIBC),),\
                   $(if $(filter aarch64 arm64,$(HOST_ARCH)),arm64-linux,)),\
                 $(if $(and $(filter Darwin,$(HOST_OS)),\
                            $(filter arm64,$(HOST_ARCH))),arm64-macos,)))
SUPPORTED_TARGETS := x86_64-linux-gnu arm64-linux x86_64-linux-musl \
                     arm64-macos
CROSS_INPUT := $(strip $(CROSS))
CROSS_TARGET := $(strip \
                  $(if $(filter aarch64-linux-gnu-,$(CROSS_INPUT)),\
                    arm64-linux,\
                    $(if $(filter x86_64-linux-musl-,$(CROSS_INPUT)),\
                      x86_64-linux-musl,)))
ifeq ($(origin TARGET),undefined)
ifneq ($(CROSS_INPUT),)
ifeq ($(CROSS_TARGET),)
$(error cannot infer TARGET from CROSS '$(CROSS_INPUT)'; set TARGET to one of: $(SUPPORTED_TARGETS))
endif
endif
endif
TARGET ?= $(if $(CROSS_TARGET),$(CROSS_TARGET),$(HOST_TARGET))
ifeq ($(strip $(TARGET)),)
$(error unsupported host $(HOST_ARCH)-$(HOST_OS); set TARGET to one of: $(SUPPORTED_TARGETS))
endif
ifneq ($(words $(filter $(TARGET),$(SUPPORTED_TARGETS))),1)
$(error unsupported TARGET '$(TARGET)' (supported: $(SUPPORTED_TARGETS)))
endif
ifneq ($(CROSS_TARGET),)
ifneq ($(TARGET),$(CROSS_TARGET))
$(error CROSS '$(CROSS_INPUT)' selects TARGET '$(CROSS_TARGET)', not '$(TARGET)')
endif
endif

# TARGET chooses the ABI/profile; CROSS keeps the compiler and every binary
# inspection tool on one prefix.  A glibc host uses the canonical musl cross
# prefix, while Alpine and native arm64 lanes keep CROSS empty.
CROSS ?= $(strip \
           $(if $(and $(filter x86_64-linux-musl,$(TARGET)),\
                      $(filter-out x86_64-linux-musl,$(HOST_TARGET))),\
             x86_64-linux-musl-,))
ifneq ($(TARGET),$(HOST_TARGET))
ifeq ($(strip $(CROSS)),)
$(error TARGET '$(TARGET)' is not native to $(HOST_TARGET); set CROSS=<tool-prefix>)
endif
endif

CC      ?= cc
HOSTCC  ?= cc
NM      ?= nm
SIZE    ?= size
STRIP   ?= strip
AR      ?= ar
READELF ?= readelf
FILE_CMD ?= file
LDD     ?= ldd
MUSL_CC ?= $(if $(strip $(CROSS)),$(CROSS)gcc,gcc)
ifneq ($(strip $(CROSS)),)
override CC := $(CROSS)gcc
override NM := $(CROSS)nm
override SIZE := $(CROSS)size
override STRIP := $(CROSS)strip
override AR := $(CROSS)ar
override READELF := $(CROSS)readelf
endif
ifeq ($(TARGET),x86_64-linux-musl)
override CC := $(MUSL_CC)
endif

TARGET_OS := $(if $(filter arm64-macos,$(TARGET)),Darwin,Linux)
BUILD   ?= build
ALLOCDBG ?= 0
EMBED_RUNTIME ?= 0
SHIPPING ?= 0
ifeq ($(TARGET),x86_64-linux-musl)
override SHIPPING := 1
endif
GC_SECTIONS ?= 1
STRIPFLAGS ?= -s -R .comment -R .note.ABI-tag
SIZE_ROOT ?= build-size
PREFIX  ?= /usr/local
MODULES ?= lsp ai fuss plugins
FUZZ_ITERS ?= 200000
FUZZ_SEED  ?= 1
FUZZ_SECONDS ?=
PLUG_FUZZ_SECONDS ?= 3600
LSP_RESP_FUZZ_ITERS ?= 50000
LSP_RESP_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                       0x9e3779b97f4a7c15 0xd1b54a32d192ed03
PORCELAIN_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                        0x9e3779b97f4a7c15 0xd1b54a32d192ed03
FUSS_FUZZ_ITERS ?= 20000
CMDPARSE_FUZZ_ITERS ?= 1000000
TEXTBUF_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                      0x9e3779b97f4a7c15 0xd1b54a32d192ed03
TEXTBUF_FUZZ_MIXES ?= typing paste undo
UNITS_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                    0x9e3779b97f4a7c15 0xd1b54a32d192ed03
MULTICURSOR_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                          0x9e3779b97f4a7c15 0xd1b54a32d192ed03
MULTICURSOR_FUZZ_OPS ?= 100000
SHADOW_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                    0x9e3779b97f4a7c15 0xd1b54a32d192ed03
SHADOW_FUZZ_ITERS ?= 50000
SYN_FUZZ_SEEDS ?= 1 0x243f6a8885a308d3 \
                  0x9e3779b97f4a7c15 0xd1b54a32d192ed03
SYN_FUZZ_OPS ?= 100000
SYN_FUZZ_SECONDS ?= 600
SYN_PACK_ROTATE ?= 0
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
PERF_BASELINE ?= $(if $(filter perf-arm64-linux,$(PERF_RUNNER_ID)),\
                    tests/perf/baselines/perf-arm64-linux.txt,\
                    tests/perf/baselines/perf-x86_64-linux-gnu.txt)
PERF_COMPONENT_LIMITS ?= tests/perf/component-limits.txt
LATENCY_BASELINE ?= tests/perf/baselines/latency-x86_64-linux-gnu.txt
SCRIPT_SUITE_BASELINE ?= tests/perf/baselines/script-x86_64-linux-gnu.txt
PERF_ADVISORY ?= 0
PERF_S56_COLLECT ?= 1
PERF_SYN_PROBE_STEM ?= markdown
# Reduced fixture size for the functional search gate.  Export or override
# this Make variable to exercise a larger smoke without creating the 3 GiB
# manifest set; `perf-search-s56` remains pinned to the manifest's 1 GiB rows.
PERF_SEARCH_SMOKE_BYTES ?= 8388608
CALIB_REFERENCE ?=
CALIB_OUTPUT ?= $(BUILD)/calib.txt
EXTRA_CFLAGS ?=
PERF_S56_WORKSPACE_READY := $(BUILD)/perf-s56-many/.ready

ifeq ($(ALLOCDBG),1)
ifeq ($(origin BUILD),file)
BUILD := build-adbg
endif
endif

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
          -DYEW_RUNTIME_DIR_DEFAULT='"$(PREFIX)/share/yew/runtime"' \
          -MMD -MP -Isrc -Itests -Itests/pty -Itests/fuzz \
          -DYEW_WITH_LSP=$(if $(filter lsp,$(MODULES)),1,0) \
          -DYEW_WITH_AI=$(if $(filter ai,$(MODULES)),1,0) \
          -DYEW_WITH_FUSS=$(if $(filter fuss,$(MODULES)),1,0) \
          -DYEW_WITH_PLUGINS=$(if $(filter plugins,$(MODULES)),1,0) \
          -DYEW_EMBED_RUNTIME=$(EMBED_RUNTIME) \
          $(EXTRA_CFLAGS)

ifeq ($(TARGET),arm64-macos)
CFLAGS += -D_DARWIN_C_SOURCE
endif

ifeq ($(ALLOCDBG),1)
CFLAGS += -DYEW_ALLOC_DEBUG=1
endif

ifeq ($(SHIPPING),1)
CFLAGS += -DNDEBUG
ifeq ($(TARGET_OS),Linux)
CFLAGS += -ffunction-sections -fdata-sections \
          -fno-asynchronous-unwind-tables -fno-unwind-tables
endif
endif
ifeq ($(TARGET),x86_64-linux-musl)
CFLAGS += -fPIE -fno-plt
endif

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
ifeq ($(SHIPPING),1)
ifeq ($(TARGET_OS),Linux)
ifeq ($(GC_SECTIONS),1)
LDFLAGS += -Wl,--gc-sections
endif
LDFLAGS += -Wl,--build-id=none
endif
endif
ifeq ($(TARGET),x86_64-linux-musl)
LDFLAGS += -static-pie -Wl,-z,relro,-z,now,-z,noexecstack
endif
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
ifeq ($(TARGET_OS),Darwin)
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
CFLAGS  += -DYEW_ASAN_RUNTIME='"$(ASAN_RT)"'
endif
endif

UNIT_HOME := $(abspath $(BUILD)/tmp/home)
UNIT_STATE_HOME := $(abspath $(BUILD)/tmp/state)
UNIT_RUNTIME_PREP := mkdir -p '$(UNIT_HOME)' '$(UNIT_STATE_HOME)' &&
UNIT_RUNTIME_ENV := HOME='$(UNIT_HOME)' \
                    TMPDIR='$(abspath $(BUILD)/tmp)' \
                    XDG_STATE_HOME='$(UNIT_STATE_HOME)' \
                    YEW_RUNTIME_DIR='$(abspath runtime)'
MUSL_UNIT_EXCLUDES :=
MUSL_UNIT_PREP :=
ifeq ($(TARGET),x86_64-linux-musl)
# A static PIE has no dynamic loader with which to interpose faultshim.so.
# The same four contracts remain mandatory in the glibc, sanitizer, and
# valgrind lanes; make the musl omission named rather than silently passing.
MUSL_UNIT_EXCLUDES := \
  --exclude multicursor_200_insert_is_one_undo_and_one_journal_sync \
  --exclude save_fault_shim_contract \
  --exclude ws_save_write_is_atomic_in_order \
  --exclude ws_save_survives_kill9_at_every_step
MUSL_UNIT_PREP := \
  printf '%s\n' \
    'SKIP multicursor_200_insert_is_one_undo_and_one_journal_sync: static PIE cannot load the LD_PRELOAD fault shim' \
    'SKIP save_fault_shim_contract: static PIE cannot load the LD_PRELOAD fault shim' \
    'SKIP ws_save_write_is_atomic_in_order: static PIE cannot load the LD_PRELOAD fault shim' \
    'SKIP ws_save_survives_kill9_at_every_step: static PIE cannot load the LD_PRELOAD fault shim' &&
endif
UNIT_RUN := $(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
            $(BUILD)/unit_tests $(MUSL_UNIT_EXCLUDES)
ifeq ($(TARGET),x86_64-linux-musl)
TORTURE_LIVE_GATE = @printf '%s\n' \
  'SKIP torture-live-check: static PIE cannot load the LD_PRELOAD fault shim'
else
TORTURE_LIVE_GATE = $(MAKE) --no-print-directory torture-live-check \
  BUILD=$(BUILD) CC=$(CC) SAN=$(SAN) VALGRIND=$(VALGRIND)
endif
# Deferred (=, not :=) so the PTY budgets resolve at USE time, after the
# VALGRIND/else branches below have chosen their values.  Without these
# prefixes the plain lanes silently inherit runner.c's fallbacks.
PTY_RUN   = YEW_PTY_BUDGET_MS=$(YEW_PTY_BUDGET_MS) \
            YEW_PTY_CASE_BUDGET_MS=$(YEW_PTY_CASE_BUDGET_MS) \
            $(BUILD)/pty_runner
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
  --exclude syn_deferred_surfaces_fail_loudly \
  --exclude syn_registry_allocation_overflow_is_a_bug \
  --exclude json_writer_structure_bugs \
  --exclude ctxmenu_a_row_handler_reading_a_payload_is_a_bug \
  --exclude shadow_menu_ghost_conflict_is_a_bug \
  --exclude undo_filter_reason_names_sprint19 \
  --exclude undo_replace_reason_names_sprint21 \
  --exclude undo_macro_reason_names_sprint34 \
  --exclude undo_lsp_reason_names_sprint47 \
  --exclude undo_save_rejects_open_transaction \
  --exclude cmd_registry_rejects_invalid_descriptors \
  --exclude cmd_registry_enforces_cmdwords \
  --exclude tty_poisoned_access_is_bug \
  --exclude render_invalid_cells_are_bugs
ifeq ($(VALGRIND),1)
# MEASURED, not guessed: the expanded suite exceeded the old 3600 s ceiling
# on a hosted runner while cases were still completing normally.  Keep the
# strict per-case hang bound below, but give the complete leak-check sweep
# enough aggregate time to finish on CI.  This is a wall-clock ceiling, not a
# latency budget: nothing measures against it.
YEW_PTY_BUDGET_MS ?= 7200000
YEW_PTY_CASE_BUDGET_MS ?= 60000
YEW_SCRIPT_BUDGET_MS ?= 600000
# A settle infers "done" from silence, and valgrind makes the editor
# silent for far longer than it is idle.  See quiet_scale() in
# tests/pty/harness.c.
YEW_PTY_QUIET_SCALE ?= 8
VALGRIND_RUN := valgrind --quiet --error-exitcode=99 --leak-check=full \
                 --errors-for-leak-kinds=definite --track-fds=yes \
                 --child-silent-after-fork=yes
VALGRIND_TRACE_SKIP := \
    --trace-children-skip='/bin/*,/usr/bin/*,/usr/lib/*,/sbin/*'
UNIT_RUN := $(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
            YEW_TEST_INSTRUMENTED=1 $(VALGRIND_RUN) \
            $(BUILD)/unit_tests $(MUSL_UNIT_EXCLUDES) \
            $(UNIT_DEATH_EXCLUDES) && \
            $(UNIT_RUNTIME_ENV) YEW_TORTURE_CLEAN_ONLY=1 $(VALGRIND_RUN) \
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
PTY_RUN  := YEW_PTY_BUDGET_MS=$(YEW_PTY_BUDGET_MS) \
            YEW_PTY_CASE_BUDGET_MS=$(YEW_PTY_CASE_BUDGET_MS) \
            YEW_PTY_QUIET_SCALE=$(YEW_PTY_QUIET_SCALE) \
            YEW_PTY_EXCLUDE=notepad_restore_segv,s32_bug_restores_the_terminal \
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
# Sprint 53 grew this to 421 cases.  Two independent hosted lanes reached
# the old 600 s aggregate ceiling while cases were still passing normally;
# the per-case deadline remained healthy.  Give the complete sweep 50%
# headroom for shared-runner variance.  This is a wall-clock ceiling on a
# HANG, not a latency budget — nothing measures against it, and the per-case
# budget is what bounds a single stuck case.
YEW_PTY_BUDGET_MS ?= 900000
# Git-backed editor cases execute real subprocesses.  The stale-blame case
# completed correctly at 4.7-4.9 s under CPU contention, which leaves no
# honest margin under the runner's 5 s fallback.  This remains a hang
# ceiling; PTY latency is asserted by semantic barriers and dedicated gates.
YEW_PTY_CASE_BUDGET_MS ?= 10000
ifeq ($(SAN),1)
# The 10,000-replacement migration case is deliberately CPU-heavy under
# per-instruction VM checks plus ASan/UBSan; this is a hang ceiling only.
YEW_SCRIPT_BUDGET_MS ?= 120000
else ifeq ($(TARGET),x86_64-linux-musl)
# The same 10,000-replacement case takes about 13 seconds in Alpine's static
# PIE profile on the pinned runner.  Keep this a hang ceiling with better than
# 2x measured headroom; the performance lanes own latency assertions.
YEW_SCRIPT_BUDGET_MS ?= 30000
else
YEW_SCRIPT_BUDGET_MS ?= 10000
endif
endif

ifeq ($(SAN),1)
UNIT_RUN := $(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
            YEW_TORTURE_CLEAN_ONLY=1 YEW_TEST_INSTRUMENTED=1 \
            $(BUILD)/unit_tests $(MUSL_UNIT_EXCLUDES) \
            $(UNIT_DEATH_EXCLUDES)
endif

# Keep source and link order deterministic across filesystems.
CORE_SRC := $(filter-out src/ws/fl_emit.c src/ws/fl_parse.c \
                         src/ws/state_legacy.c, \
              $(shell find src -path 'src/mod/*' -prune -o -name '*.c' \
                -print | sort))
JSON_SRC := src/mod/lsp/json.c
MOD_SRC  := src/mod/mods.c \
  $(if $(filter lsp ai,$(MODULES)),$(JSON_SRC)) \
  $(foreach m,$(filter $(KNOWN_MODS),$(MODULES)), \
    $(filter-out %/shim.c $(JSON_SRC), \
      $(sort $(wildcard src/mod/$(MODDIR_$(m))/*.c)))) \
  $(foreach m,$(filter-out $(MODULES),$(KNOWN_MODS)), \
    $(sort $(wildcard src/mod/$(MODDIR_$(m))/shim.c)))
SRC      := $(CORE_SRC) $(MOD_SRC)
OBJ      := $(SRC:%.c=$(BUILD)/%.o)
RUNTIME_BLOB_GEN := $(BUILD)/host/gen-runtime-blob
RUNTIME_BLOB_C := $(BUILD)/gen/runtime_blob.c
RUNTIME_BLOB_OBJ := $(BUILD)/gen/runtime_blob.o
RUNTIME_BLOB_INPUTS := $(shell find runtime -type f -print | LC_ALL=C sort)
RUNTIME_BLOB_DIRS := $(shell find runtime -type d -print | LC_ALL=C sort)
ifeq ($(EMBED_RUNTIME),1)
OBJ += $(RUNTIME_BLOB_OBJ)
endif

UNIT_SRC := $(filter-out tests/unit/fakeclip.c, \
              $(sort $(wildcard tests/unit/*.c)))
UNIT_JSON_SRC := tests/unit/test_json.c tests/unit/test_json_num.c \
                 tests/unit/test_jsonw.c
UNIT_LSP_SRC := tests/unit/test_jsonrpc_frame.c \
                tests/unit/test_jsonrpc.c tests/unit/test_lsp_transport.c \
                tests/unit/test_lsp_caps.c tests/unit/test_lsp_diag.c \
                tests/unit/test_lsp_completion.c \
                tests/unit/test_lsp_config.c tests/unit/test_lsp_life.c \
                tests/unit/test_lsp_gate.c \
                tests/unit/test_lsp_highlight.c \
                tests/unit/test_lsp_nav.c \
                tests/unit/test_lsp_rename.c \
                tests/unit/test_lsp_snippet.c \
                tests/unit/test_lsp_symbols.c \
                tests/unit/test_lsp_sync.c \
                tests/unit/test_lsp_uri.c
UNIT_AI_SRC := tests/unit/test_ai_backend.c tests/unit/test_ai_curl.c \
               tests/unit/test_ai_config.c tests/unit/test_ai_key.c \
               tests/unit/test_ai_registry.c tests/unit/test_ai_runtime.c \
               tests/unit/test_ai_stream.c tests/unit/test_ai_context.c \
               tests/unit/test_ai_prompt.c tests/unit/test_ai_trim.c \
               tests/unit/test_ai_frame.c tests/unit/test_ai_cancel.c \
               tests/unit/test_ai_stats.c tests/unit/test_ai_shadow_live.c \
               tests/unit/test_ai_shadow_policy.c \
               tests/unit/test_ai_badge.c \
               tests/unit/test_ai_debug.c \
               tests/unit/test_ai_optin.c tests/unit/test_ai_presets.c \
               tests/unit/test_ai_privacy_gates.c \
               tests/unit/test_ai_privacy_wire.c \
               tests/unit/test_ai_policy.c \
               tests/unit/test_ai_paths.c tests/unit/test_ai_redact.c \
               tests/unit/test_http_chunk.c tests/unit/test_http_req.c \
               tests/unit/test_http_rx.c tests/unit/test_http_url.c \
               tests/unit/test_http_live.c
UNIT_FUSS_SRC := tests/unit/test_porcelain.c tests/unit/test_gitcache.c \
                 tests/unit/test_fusstree.c tests/unit/test_fussnav.c \
                 tests/unit/test_fusscollapse.c tests/unit/test_fussjump.c \
                 tests/unit/test_fussdrawer.c tests/unit/test_fusscommit.c \
                 tests/unit/test_diff.c \
                 tests/unit/test_hunk.c tests/unit/test_blamecache.c \
                 tests/unit/test_diffview.c tests/unit/test_git_editor.c
UNIT_PLUG_SRC := $(sort $(wildcard tests/unit/test_plug_*.c))
ifeq ($(filter lsp ai,$(MODULES)),)
UNIT_SRC := $(filter-out $(UNIT_JSON_SRC),$(UNIT_SRC))
endif
ifeq ($(filter lsp,$(MODULES)),)
UNIT_SRC := $(filter-out $(UNIT_LSP_SRC),$(UNIT_SRC))
endif
ifeq ($(filter ai,$(MODULES)),)
UNIT_SRC := $(filter-out $(UNIT_AI_SRC),$(UNIT_SRC))
endif
ifeq ($(filter fuss,$(MODULES)),)
UNIT_SRC := $(filter-out $(UNIT_FUSS_SRC),$(UNIT_SRC))
endif
ifeq ($(filter plugins,$(MODULES)),)
UNIT_SRC := $(filter-out $(UNIT_PLUG_SRC),$(UNIT_SRC))
endif
UNIT_OBJ := $(UNIT_SRC:%.c=$(BUILD)/%.o)
SYN_ENGINE_UNIT_OBJ := $(BUILD)/tests/unit/syn_engine.o
STATE_LEGACY_OBJ := $(BUILD)/tests/unit/state_legacy.o

# Sprint 36: activate the independent Fletch arm in the frozen-corpus
# differential.  The hand-written parser remains visible only to tests.
$(BUILD)/tests/unit/test_state_differential.o: CFLAGS += \
  -DYEW_HAVE_FLETCH_STATE=1
$(BUILD)/tests/unit/test_syn_embed_runtime.o: CFLAGS += -DYEW_SYN_TEST=1
FAKECLIP := $(BUILD)/fakeclip
FAKELSP := $(BUILD)/tests/helpers/fakelsp
FAKECURL := $(BUILD)/tests/helpers/fakecurl
FAKEHTTP := $(BUILD)/tests/helpers/fakehttp
MOCKAI := $(BUILD)/tests/helpers/mockai
MOCKCURL := $(BUILD)/tests/helpers/mockcurl
AI_TEST_HELPERS := $(if $(filter ai,$(MODULES)),$(MOCKAI) $(MOCKCURL))
$(BUILD)/tests/unit/test_ai_curl.o: CFLAGS += \
  -DYEW_TEST_FAKECURL='"$(abspath $(FAKECURL))"'
$(BUILD)/tests/unit/test_ai_curl.o: $(FAKECURL)
$(BUILD)/tests/unit/test_http_live.o: CFLAGS += \
  -DYEW_TEST_FAKEHTTP='"$(abspath $(FAKEHTTP))"'
$(BUILD)/tests/unit/test_http_live.o: $(FAKEHTTP)
$(BUILD)/tests/unit/test_ai_commands.o: CFLAGS += \
  -DYEW_TEST_FAKEHTTP='"$(abspath $(FAKEHTTP))"' \
  -DYEW_TEST_FAKECURL='"$(abspath $(FAKECURL))"'
$(BUILD)/tests/unit/test_ai_commands.o: $(FAKEHTTP) $(FAKECURL)
$(BUILD)/tests/unit/test_ai_cancel.o: CFLAGS += \
  -DYEW_TEST_FAKEHTTP='"$(abspath $(FAKEHTTP))"'
$(BUILD)/tests/unit/test_ai_cancel.o: $(FAKEHTTP)
$(BUILD)/tests/unit/test_ai_debug.o: CFLAGS += \
  -DYEW_TEST_MOCKAI='"$(abspath $(MOCKAI))"'
$(BUILD)/tests/unit/test_ai_debug.o: $(MOCKAI)
$(BUILD)/tests/unit/test_ai_shadow_live.o: CFLAGS += \
  -DYEW_TEST_MOCKAI='"$(abspath $(MOCKAI))"' \
  -DYEW_TEST_MOCKCURL='"$(abspath $(MOCKCURL))"'
$(BUILD)/tests/unit/test_ai_shadow_live.o: $(MOCKAI) $(MOCKCURL)
ifeq ($(filter lsp,$(MODULES)),)
ifeq ($(filter ai,$(MODULES)),)
SCRIPT_RUNNER_ARGS := --exclude lsp_,ai_
else
SCRIPT_RUNNER_ARGS := --exclude lsp_
endif
else
ifeq ($(filter ai,$(MODULES)),)
SCRIPT_RUNNER_ARGS := --exclude ai_
else
SCRIPT_RUNNER_ARGS :=
endif
endif
ifeq ($(filter plugins,$(MODULES)),)
ifeq ($(filter lsp,$(MODULES)),)
ifeq ($(filter ai,$(MODULES)),)
SCRIPT_RUNNER_ARGS := --exclude lsp_,ai_,plug_examples_
else
SCRIPT_RUNNER_ARGS := --exclude lsp_,plug_examples_
endif
else
ifeq ($(filter ai,$(MODULES)),)
SCRIPT_RUNNER_ARGS := --exclude ai_,plug_examples_
else
SCRIPT_RUNNER_ARGS := --exclude plug_examples_
endif
endif
endif
PTY_VT_OBJ := $(BUILD)/tests/pty/vt.o
PTY_SNAPSHOT_OBJ := $(BUILD)/tests/pty/snapshot.o
PTY_ORACLE_OBJ := $(PTY_VT_OBJ) $(PTY_SNAPSHOT_OBJ)
PTY_HARNESS_OBJ := $(BUILD)/tests/pty/harness.o
PTY_REGISTRY_OBJ := $(BUILD)/tests/pty/registry.o
PTY_RUNNER_OBJ := $(BUILD)/tests/pty/runner.o
PTY_DEMO_OBJ := $(BUILD)/tests/pty/demo_paint.o
ifneq ($(filter ai,$(MODULES)),)
$(PTY_REGISTRY_OBJ): CFLAGS += \
  -DYEW_TEST_MOCKAI='"$(abspath $(MOCKAI))"'
$(PTY_REGISTRY_OBJ): $(MOCKAI)
endif
PTY_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) \
                $(PTY_ORACLE_OBJ) $(PTY_HARNESS_OBJ) $(PTY_REGISTRY_OBJ) \
                $(PTY_RUNNER_OBJ)
PTY_DEMO_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) $(PTY_DEMO_OBJ)
TEXT_FUZZ_SUPPORT_OBJ := $(BUILD)/tests/fuzz/oracle.o \
                         $(BUILD)/tests/fuzz/shrink.o
UNIT_LINK_OBJ := $(filter-out $(BUILD)/src/main.o \
                 $(BUILD)/src/syn/engine.o,$(OBJ)) \
                 $(SYN_ENGINE_UNIT_OBJ) $(UNIT_OBJ) \
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
FUZZ_SHADOW_OBJ := $(BUILD)/tests/fuzz/fuzz_shadow.o
FUZZ_GROUPS_OBJ := $(BUILD)/tests/fuzz/fuzz_groups.o
FUZZ_REDIFF_OBJ := $(BUILD)/tests/fuzz/fuzz_re_diff.o
FUZZ_FUZZY_OBJ := $(BUILD)/tests/fuzz/fuzz_fuzzy.o
FUZZ_STATE_OBJ := $(BUILD)/tests/fuzz/fuzz_state.o
FUZZ_GITIGNORE_OBJ := $(BUILD)/tests/fuzz/fuzz_gitignore.o
FUZZ_PORCELAIN_OBJ := $(BUILD)/tests/fuzz/fuzz_porcelain.o
FUZZ_FUSS_OBJ := $(BUILD)/tests/fuzz/fuzz_fuss.o
FUZZ_GIT_DIFF_OBJ := $(BUILD)/tests/fuzz/fuzz_git_diff.o
FUZZ_MOUSE_OBJ := $(BUILD)/tests/fuzz/fuzz_mouse.o
FUZZ_FLLEX_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_lex.o
FUZZ_FLPARSE_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_parse.o
FUZZ_FLSTD_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_std.o
FUZZ_FLVM_OBJ := $(BUILD)/tests/fuzz/fuzz_fl_vm.o
FUZZ_FLAPI_OBJ := $(BUILD)/tests/fuzz/fuzz_flapi.o
FUZZ_RECORD_OBJ := $(BUILD)/tests/fuzz/fuzz_record.o
FUZZ_SYN_OBJ := $(BUILD)/tests/fuzz/fuzz_syn.o
FUZZ_SYN_DEF_OBJ := $(BUILD)/tests/fuzz/fuzz_syn_def.o
FUZZ_SYMIDX_OBJ := $(BUILD)/tests/fuzz/fuzz_symidx.o
FUZZ_JSON_OBJ := $(BUILD)/tests/fuzz/fuzz_json.o
FUZZ_JSONRPC_OBJ := $(BUILD)/tests/fuzz/fuzz_jsonrpc.o
FUZZ_LSP_MSG_OBJ := $(BUILD)/tests/fuzz/fuzz_lsp_msg.o
FUZZ_LSP_RESP_OBJ := $(BUILD)/tests/fuzz/fuzz_lsp_resp.o
FUZZ_HTTP_OBJ := $(BUILD)/tests/fuzz/fuzz_http.o
FUZZ_AI_STREAM_OBJ := $(BUILD)/tests/fuzz/fuzz_ai_stream.o
FUZZ_AI_SHADOW_OBJ := $(BUILD)/tests/fuzz/fuzz_ai_shadow.o
FUZZ_AI_REDACT_OBJ := $(BUILD)/tests/fuzz/fuzz_ai_redact.o
FUZZ_PKG_TREE_OBJ := $(BUILD)/tests/fuzz/fuzz_pkg_tree.o
LSP_LIVE_OBJ := $(BUILD)/tests/lsp/test_clangd_live.o
LSP_LIVE_BIN := $(BUILD)/tests/lsp/test_clangd_live
RE_REF_OBJ := $(BUILD)/tests/fuzz/re_ref.o
FUZZ_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
FUZZ_LINK_OBJ := $(FUZZ_CORE_OBJ) $(FUZZ_LIB_OBJ)
FUSS_TREE_TEST_OBJ := $(BUILD)/src/mod/git/fusstree.o \
                      $(BUILD)/src/mod/git/porcelain.o \
                      $(BUILD)/src/util/arena.o $(BUILD)/src/util/base.o \
                      $(BUILD)/src/util/log.o $(BUILD)/src/util/sort.o
AI_FUZZ_TARGET := $(if $(filter ai,$(MODULES)),fuzz-ai,)

PERF_UNICODE_OBJ := $(BUILD)/tests/perf/perf_unicode.o
PERF_RENDER_OBJ := $(BUILD)/tests/perf/perf_render.o
PERF_SHADOW_OBJ := $(BUILD)/tests/perf/perf_shadow.o
PERF_SCROLL_OBJ := $(BUILD)/tests/perf/scroll.o
PERF_PIECE_OBJ := $(BUILD)/tests/perf/perf_piece.o
PERF_CURSOR_OBJ := $(BUILD)/tests/perf/perf_cursor.o
PERF_UNDO_OBJ := $(BUILD)/tests/perf/perf_undo.o
PERF_TEXTBUF_OBJ := $(BUILD)/tests/perf/perf_textbuf.o
PERF_LATENCY_OBJ := $(BUILD)/tests/perf/latency.o
PERF_LATENCY_S56_OBJ := $(BUILD)/tests/perf/perf_latency.o
PERF_ECHO_CHILD_OBJ := $(BUILD)/tests/perf/echo_child.o
PERF_STARTUP_OBJ := $(BUILD)/tests/perf/perf_startup.o
PERF_NULLEXEC_OBJ := $(BUILD)/tests/perf/nullexec.o
PERF_OPEN_OBJ := $(BUILD)/tests/perf/perf_open.o
PERF_MEM_OBJ := $(BUILD)/tests/perf/perf_mem.o
PERF_ALLOC_OBJ := $(BUILD)/tests/perf/perf_alloc.o
PERF_S56_GATE_POLICY_OBJ := $(BUILD)/tests/perf/s56_gate_policy_selftest.o
PERF_S56_PROF_CROSSCHECK_OBJ := $(BUILD)/tests/perf/perf_prof_crosscheck.o
PERF_JOBSTREAM_OBJ := $(BUILD)/tests/perf/jobstream.o
PERF_REPATH_OBJ := $(BUILD)/tests/perf/re_pathological.o
PERF_RETHRU_OBJ := $(BUILD)/tests/perf/re_throughput.o
PERF_SEARCHLAT_OBJ := $(BUILD)/tests/perf/search_latency.o
PERF_SEARCH_S56_OBJ := $(BUILD)/tests/perf/perf_search.o
PERF_UNITS_OBJ := $(BUILD)/tests/perf/perf_units.o
PERF_MULTICURSOR_OBJ := $(BUILD)/tests/perf/multicursor.o
PERF_CMDCOMP_OBJ := $(BUILD)/tests/perf/perf_cmdcomp.o
FL_SMOKE_OBJ := $(BUILD)/tests/perf/fl_smoke.o
PERF_STATE_OBJ := $(BUILD)/tests/perf/perf_state.o
PERF_FINDER_OBJ := $(BUILD)/tests/perf/finder.o
PERF_MOUSE_OBJ := $(BUILD)/tests/perf/mouse.o
PERF_GIT_STATUS_OBJ := $(BUILD)/tests/perf/git_status.o
PERF_FUSS_OBJ := $(BUILD)/tests/perf/fuss.o
PERF_GIT_GUTTER_OBJ := $(BUILD)/tests/perf/git_gutter.o
ifeq ($(HOST_OS),Linux)
PERF_GIT_ALLOC_WRAP := -Wl,--wrap=malloc -Wl,--wrap=calloc \
                       -Wl,--wrap=realloc -Wl,--wrap=free \
                       -Wl,--wrap=arena_alloc
PERF_SHADOW_ALLOC_WRAP := -Wl,--wrap=malloc -Wl,--wrap=calloc \
                          -Wl,--wrap=realloc -Wl,--wrap=free
endif
LIVE_PTY_OBJ := $(BUILD)/tests/support/live_pty.o
PERF_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
PERF_FLETCH_OBJ := $(BUILD)/tests/perf/perf_fletch.o
PERF_RECORD_OBJ := $(BUILD)/tests/perf/perf_record.o
PERF_SYN_OBJ := $(BUILD)/tests/perf/perf_syn.o
PERF_BATCH_OBJ := $(BUILD)/tests/perf/batch.o
PERF_SCRIPT_SUITE_OBJ := $(BUILD)/tests/perf/script_suite.o
PERF_SYMIDX_OBJ := $(BUILD)/tests/perf/perf_symidx.o
PERF_LSP_OBJ := $(BUILD)/tests/perf/perf_lsp.o
PERF_AI_HTTP_OBJ := $(BUILD)/tests/perf/perf_ai_http.o
PERF_AI_SHADOW_OBJ := $(BUILD)/tests/perf/perf_ai_shadow.o
PERF_AI_PRIVACY_OBJ := $(BUILD)/tests/perf/perf_ai_privacy.o
PERF_PLUG_OBJ := $(BUILD)/tests/perf/perf_plug.o
PERF_PKG_OBJ := $(BUILD)/tests/perf/perf_pkg.o
PERF_CLOUD_OBJ := $(BUILD)/tests/perf/perf_cloud.o
CALIB_OBJ := $(BUILD)/tests/perf/calib.o
ifneq ($(filter lsp,$(MODULES)),)
LSP_FUZZ_TARGET := fuzz-lsp-msg fuzz-lsp-resp
LSP_PERF_TARGET := perf-lsp
endif
ifneq ($(filter ai,$(MODULES)),)
AI_PERF_TARGET := perf-ai-http perf-ai-shadow perf-ai-privacy
endif
ifneq ($(filter plugins,$(MODULES)),)
PLUG_PERF_TARGET := perf-plug perf-pkg perf-cloud
PKG_FUZZ_TARGET := fuzz-pkg-tree
PKG_TEST_TARGET := test-pkg
endif
ifneq ($(filter fuss,$(MODULES)),)
FUSS_FUZZ_TARGET := fuzz-porcelain fuzz-fuss fuzz-git-diff
FUSS_PERF_TARGET := perf-git-status perf-fuss perf-git-gutter
FUSS_SCRIPT_TARGET := test-git-script test-fuss-commands test-git-hunks \
                      test-group-from-dir
FUSS_TORTURE_TARGET := torture-git-hunk
endif
FLETCH_RUN_OBJ := $(BUILD)/tests/fletch/run.o
SCRIPT_RUNNER_OBJ := $(BUILD)/tests/script/runner.o
GIT_SCRIPT_OBJ := $(BUILD)/tests/script/git_layer.o
FUSS_COMMANDS_OBJ := $(BUILD)/tests/script/fuss_commands.o
GIT_HUNKS_OBJ := $(BUILD)/tests/script/git_hunks.o
GROUP_FROM_DIR_OBJ := $(BUILD)/tests/script/group_from_dir.o
FLETCH_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
ROUNDTRIP_OBJ := $(BUILD)/tests/roundtrip/gen.o \
                 $(BUILD)/tests/roundtrip/runner.o
ROUNDTRIP_LINK_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ)) \
                      $(ROUNDTRIP_OBJ)
GEN_BIGFILE_OBJ := $(BUILD)/scripts/gen-bigfile.o

TORTURE_CHILD_OBJ := $(BUILD)/tests/torture/yew-torture.o
TORTURE_DRIVER_OBJ := $(BUILD)/tests/torture/kill9.o
TORTURE_LIVE_OBJ := $(BUILD)/tests/torture/yew-live-torture.o
TORTURE_BATCH_OBJ := $(BUILD)/tests/torture/batch_kill9.o
TORTURE_GIT_HUNK_OBJ := $(BUILD)/tests/torture/git_hunk_kill9.o
TORTURE_CORE_OBJ := $(filter-out $(BUILD)/src/main.o,$(OBJ))
TORTURE_CHILD := $(BUILD)/yew-torture
TORTURE_DRIVER := $(BUILD)/kill9
TORTURE_LIVE := $(BUILD)/yew-live-torture
TORTURE_BATCH := $(BUILD)/batch-kill9
TORTURE_GIT_HUNK := $(BUILD)/git-hunk-kill9
FAULTSHIM := $(BUILD)/tests/torture/faultshim.so

BUILD_DIRS := $(sort $(dir $(OBJ) $(UNIT_OBJ) $(SYN_ENGINE_UNIT_OBJ) \
                $(RUNTIME_BLOB_GEN) $(RUNTIME_BLOB_C) \
                $(FUZZ_LIB_OBJ) \
                $(FUZZ_UTF8_OBJ) $(FUZZ_GRAPHEME_OBJ) $(FUZZ_INPUT_OBJ) \
                $(FUZZ_GRID_OBJ) $(FUZZ_VT_OBJ) $(FUZZ_UNDO_OBJ) \
                $(FUZZ_TEXTBUF_OBJ) $(TEXT_FUZZ_SUPPORT_OBJ) \
                $(FUZZ_UNITS_OBJ) \
                $(FUZZ_MULTICURSOR_OBJ) \
                $(FUZZ_SHADOW_OBJ) \
                $(FUZZ_CMDPARSE_OBJ) $(FUZZ_RECOMPILE_OBJ) \
                $(FUZZ_REDIFF_OBJ) $(RE_REF_OBJ) \
                $(PTY_ORACLE_OBJ) \
                $(PTY_HARNESS_OBJ) $(PTY_REGISTRY_OBJ) $(PTY_RUNNER_OBJ) \
                $(PTY_DEMO_OBJ) $(PERF_UNICODE_OBJ) $(PERF_RENDER_OBJ) \
                $(PERF_SHADOW_OBJ) \
                $(PERF_PIECE_OBJ) $(PERF_CURSOR_OBJ) $(PERF_UNDO_OBJ) \
                $(PERF_TEXTBUF_OBJ) $(PERF_LATENCY_OBJ) \
                $(PERF_LATENCY_S56_OBJ) $(PERF_ECHO_CHILD_OBJ) \
                $(PERF_STARTUP_OBJ) $(PERF_NULLEXEC_OBJ) \
                $(PERF_OPEN_OBJ) $(PERF_MEM_OBJ) \
                $(PERF_ALLOC_OBJ) \
                $(PERF_S56_GATE_POLICY_OBJ) \
                $(PERF_S56_PROF_CROSSCHECK_OBJ) \
                $(PERF_JOBSTREAM_OBJ) $(PERF_REPATH_OBJ) \
                $(PERF_RETHRU_OBJ) $(PERF_SEARCH_S56_OBJ) \
                $(LIVE_PTY_OBJ) \
                $(PERF_UNITS_OBJ) \
                $(PERF_MULTICURSOR_OBJ) \
                $(PERF_CMDCOMP_OBJ) \
                $(PERF_STATE_OBJ) \
                $(PERF_FINDER_OBJ) $(PERF_MOUSE_OBJ) \
                $(GEN_BIGFILE_OBJ) $(FLETCH_RUN_OBJ) $(SCRIPT_RUNNER_OBJ) \
                $(GIT_SCRIPT_OBJ) $(FUSS_COMMANDS_OBJ) \
                $(ROUNDTRIP_OBJ) \
                $(PERF_FLETCH_OBJ) $(PERF_RECORD_OBJ) $(PERF_BATCH_OBJ) \
                $(PERF_SCRIPT_SUITE_OBJ) \
                $(FUZZ_RECORD_OBJ) $(FUZZ_SYN_OBJ) $(FUZZ_SYN_DEF_OBJ) \
                $(FUZZ_SYMIDX_OBJ) $(FUZZ_JSON_OBJ) $(FUZZ_JSONRPC_OBJ) \
                $(FUZZ_FUSS_OBJ) $(FUZZ_GIT_DIFF_OBJ) \
                $(FUZZ_LSP_MSG_OBJ) $(FUZZ_LSP_RESP_OBJ) $(LSP_LIVE_OBJ) \
                $(FUZZ_HTTP_OBJ) $(FUZZ_AI_STREAM_OBJ) \
                $(FUZZ_AI_SHADOW_OBJ) $(FUZZ_AI_REDACT_OBJ) \
                $(PERF_SYN_OBJ) $(PERF_SYMIDX_OBJ) $(PERF_LSP_OBJ) \
                $(PERF_AI_HTTP_OBJ) $(PERF_AI_SHADOW_OBJ) \
                $(PERF_AI_PRIVACY_OBJ) $(PERF_FUSS_OBJ) $(CALIB_OBJ) \
                $(PERF_GIT_GUTTER_OBJ) \
                $(TORTURE_CHILD_OBJ) \
                $(TORTURE_DRIVER_OBJ) $(TORTURE_LIVE_OBJ) \
                $(TORTURE_BATCH_OBJ) $(TORTURE_GIT_HUNK_OBJ) \
                $(GIT_HUNKS_OBJ) $(GROUP_FROM_DIR_OBJ) \
                $(FAULTSHIM) $(FAKELSP)))

# A content mismatch makes FORCE a normal prerequisite of every object built
# by this invocation.  The stamp recipe also removes objects not reachable
# from the requested target (notably main.o during `make test`), so a later
# target cannot reuse macros from the previous module selection.
# GNU Make 3.81 (the system make on macOS) predates $(file ...).  Reading
# through the POSIX shell keeps profile reuse correct on every locked target.
STAMP_MODULES := $(strip $(shell if test -f '$(BUILD)/mods.stamp'; then \
	cat '$(BUILD)/mods.stamp'; fi))
ifneq ($(STAMP_MODULES),$(MODULES))
MODULE_FORCE := FORCE
endif
BUILD_PROFILE_KEY := target=$(TARGET);cc=$(CC);shipping=$(SHIPPING);gc=$(GC_SECTIONS);allocdbg=$(ALLOCDBG);embed_runtime=$(EMBED_RUNTIME);san=$(SAN);valgrind=$(VALGRIND);fl_cgoto=$(FL_CGOTO);fl_checks=$(FL_CHECKS);fl_trace=$(CFLAGS_FL_TRACE);prefix=$(PREFIX);extra=$(EXTRA_CFLAGS)
STAMP_PROFILE := $(strip $(shell if test -f '$(BUILD)/profile.stamp'; then \
	cat '$(BUILD)/profile.stamp'; fi))
ifneq ($(STAMP_PROFILE),$(BUILD_PROFILE_KEY))
PROFILE_FORCE := FORCE
endif

.DEFAULT_GOAL := all
.PHONY: all check test test-alloc-debug alloc perf-alloc clean install dirs FORCE \
        target-info target-tools-selftest static-pie-tools-selftest \
        runtime-blob-selftest runtime-embedded-e2e test-runtime-embedded \
        runtime-embedded-budget \
        musl-verify test-musl-hosts \
        test-script test-git-script \
        test-fuss-commands test-git-hunks test-group-from-dir \
        test-script-determinism test-script-budget test-pkg test-pty fuzz \
        fuzz-textbuf fuzz-units fuzz-multicursor fuzz-cmdparse fuzz-long \
        fuzz-plug-manifest fuzz-pkg-tree fuzz-pkg-tree-long \
        fuzz-mouse fuzz-groups fuzz-shadow fuzz-record fuzz-syn fuzz-syn-def \
        fuzz-symidx fuzz-json fuzz-jsonrpc fuzz-fuss fuzz-lsp-msg fuzz-lsp-resp \
        fuzz-porcelain fuzz-git-diff \
        fuzz-ai \
        test-lsp-live \
        fuzz-syn-long \
        fuzz-syn-line-long fuzz-syn-edit-long \
        test-record-corpus \
        test-syn-corpus test-syn-def-corpus test-syn-assets syn-goldens \
        syn-fuzz-seeds \
        fixtures fixtures-quick fixtures-verify \
        fixtures-verify-quick \
        unicode-tables calib perf perf-components perf-symbols size \
        size-tools-selftest size-ledger-full size-ledger-minimal \
        perf-unicode perf-render perf-piece perf-cursor \
        perf-shadow perf-symidx perf-lsp perf-ai-http perf-ai-http-valgrind \
        perf-git-status perf-fuss perf-git-gutter \
        perf-ai-shadow perf-ai-privacy perf-plug perf-pkg perf-cloud \
        perf-units perf-multicursor perf-cmdcomp perf-state perf-finder \
        perf-mouse perf-record perf-syn perf-syn-budgets perf-syn-quiet \
        perf-syn-scroll-s56 \
        perf-syn-gate-selftest perf-syn-line-probe \
        perf-syn-resident-line-probe perf-syn-edit-probe perf-syn-size \
        perf-batch perf-batch-selftest \
        perf-undo perf-textbuf perf-huge perf-huge-components \
        perf-update perf-baseline-guard \
        perf-baseline-selftest \
        perf-gate-selftest perf-latency perf-latency-selftest \
        perf-s56-functional perf-s56-observation \
        perf-s56-huge-observation perf-s56-checks \
        perf-latency-s56-check perf-latency-s56-smoke \
        perf-latency-s56-matrix perf-latency-s56-typing-huge \
        perf-latency-s56-syntax perf-latency-s56-multicursor \
        perf-latency-s56-search-huge \
        perf-search-s56 perf-search-s56-smoke \
        perf-latency-s56-many perf-latency-s56-assist \
        perf-startup-s56 perf-open-s56 perf-mem-s56 \
        perf-s56-gate-selftest perf-prof-crosscheck-s56 \
        torture torture-build torture-live-check torture-batch \
        torture-git-hunk \
        fl-perf-smoke fl-dispatch-parity fl-gc-stress \
        test-fletch test-roundtrip test-roundtrip-coverage \
        test-fletch-roundtrip fletch-ledger \
        bench-fletch \
        test-fletch-dispatch test-fletch-gc-stress test-fletch-determinism

all: $(BUILD)/yew

target-info:
	@printf '%s\n' \
		'target $(TARGET)' \
		'host $(HOST_TARGET)' \
		'cross $(if $(strip $(CROSS)),$(CROSS),-)' \
		'cc $(CC)' \
		'nm $(NM)' \
		'size $(SIZE)' \
		'strip $(STRIP)' \
		'ar $(AR)' \
		'readelf $(READELF)' \
		'shipping $(SHIPPING)' \
		'embed_runtime $(EMBED_RUNTIME)' \
		'static_pie $(if $(filter x86_64-linux-musl,$(TARGET)),1,0)' \
		'profile $(BUILD_PROFILE_KEY)'

target-tools-selftest:
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) scripts/tests/target-tools.test.sh

static-pie-tools-selftest:
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) scripts/tests/static-pie-tools.test.sh

ifeq ($(TARGET),x86_64-linux-musl)
musl-verify: static-pie-tools-selftest $(BUILD)/yew
	FILE='$(FILE_CMD)' READELF='$(READELF)' NM='$(NM)' LDD='$(LDD)' \
		scripts/verify-static-pie.sh --binary '$(BUILD)/yew'

test-musl-hosts: $(BUILD)/unit_tests $(FAKEHTTP)
	UNIT_TESTS='$(BUILD)/unit_tests' FAKEHTTP='$(FAKEHTTP)' \
		scripts/tests/musl-hosts.test.sh
else
musl-verify: static-pie-tools-selftest
	@echo "musl-verify requires TARGET=x86_64-linux-musl" >&2; exit 2

test-musl-hosts:
	@echo "test-musl-hosts requires TARGET=x86_64-linux-musl" >&2; exit 2
endif

test-alloc-debug: $(BUILD)/unit_tests
	env -u XDG_STATE_HOME $(BUILD)/unit_tests --filter alloc_

compile_commands.json: FORCE $(BUILD)/gen-compdb
	$(BUILD)/gen-compdb "$(abspath .)" $(CC) $(CFLAGS) -- $(SRC) > $@

$(BUILD)/yew: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/unit_tests: $(UNIT_LINK_OBJ) $(FAKECLIP) $(FAKELSP) $(TORTURE_CHILD) \
                     $(TORTURE_DRIVER) $(FAULTSHIM)
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

$(BUILD)/fuzz_pkg_tree: $(FUZZ_LINK_OBJ) $(FUZZ_PKG_TREE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_PKG_TREE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_record: $(FUZZ_LINK_OBJ) $(FUZZ_RECORD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_RECORD_OBJ) $(LDLIBS)

$(BUILD)/fuzz_syn: $(FUZZ_LINK_OBJ) $(FUZZ_SYN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_SYN_OBJ) $(LDLIBS)

$(BUILD)/fuzz_syn_def: $(FUZZ_LINK_OBJ) $(FUZZ_SYN_DEF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_SYN_DEF_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fl_std: $(FUZZ_LINK_OBJ) $(FUZZ_FLSTD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FLSTD_OBJ) $(LDLIBS)

$(BUILD)/fuzz_tabs: $(FUZZ_LINK_OBJ) $(FUZZ_TABS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_TABS_OBJ) $(LDLIBS)

$(BUILD)/fuzz_shadow: $(FUZZ_LINK_OBJ) $(FUZZ_SHADOW_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_SHADOW_OBJ) $(LDLIBS)

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

$(BUILD)/fuzz_porcelain: $(FUZZ_LINK_OBJ) $(FUZZ_PORCELAIN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_PORCELAIN_OBJ) $(LDLIBS)

$(BUILD)/fuzz_fuss: $(FUZZ_LINK_OBJ) $(FUZZ_FUSS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_FUSS_OBJ) $(LDLIBS)

$(BUILD)/fuzz_git_diff: $(FUZZ_LINK_OBJ) $(FUZZ_GIT_DIFF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_GIT_DIFF_OBJ) $(LDLIBS)

$(BUILD)/fuzz_mouse: $(FUZZ_LINK_OBJ) $(FUZZ_MOUSE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_MOUSE_OBJ) $(LDLIBS)

$(BUILD)/fuzz_symidx: $(FUZZ_LINK_OBJ) $(FUZZ_SYMIDX_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_SYMIDX_OBJ) $(LDLIBS)

$(BUILD)/fuzz_json: $(FUZZ_LINK_OBJ) $(FUZZ_JSON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_JSON_OBJ) $(LDLIBS)

$(BUILD)/fuzz_jsonrpc: $(FUZZ_LINK_OBJ) $(FUZZ_JSONRPC_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_JSONRPC_OBJ) $(LDLIBS)

$(BUILD)/fuzz_lsp_msg: $(FUZZ_LINK_OBJ) $(FUZZ_LSP_MSG_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_LSP_MSG_OBJ) $(LDLIBS)

$(BUILD)/fuzz_lsp_resp: $(FUZZ_LINK_OBJ) $(FUZZ_LSP_RESP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_LSP_RESP_OBJ) $(LDLIBS)

$(BUILD)/fuzz_http: $(FUZZ_LINK_OBJ) $(FUZZ_HTTP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_HTTP_OBJ) $(LDLIBS)

$(BUILD)/fuzz_ai_stream: $(FUZZ_LINK_OBJ) $(FUZZ_AI_STREAM_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_AI_STREAM_OBJ) $(LDLIBS)

$(BUILD)/fuzz_ai_shadow: $(FUZZ_LINK_OBJ) $(FUZZ_AI_SHADOW_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_AI_SHADOW_OBJ) $(LDLIBS)

$(BUILD)/fuzz_ai_redact: $(FUZZ_LINK_OBJ) $(FUZZ_AI_REDACT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_LINK_OBJ) \
		$(FUZZ_AI_REDACT_OBJ) $(LDLIBS)

$(LSP_LIVE_BIN): $(FUZZ_CORE_OBJ) $(LSP_LIVE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FUZZ_CORE_OBJ) \
		$(LSP_LIVE_OBJ) $(LDLIBS)

$(BUILD)/gen-bigfile: $(GEN_BIGFILE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(GEN_BIGFILE_OBJ) $(LDLIBS)

$(BUILD)/perf_fletch: $(PERF_CORE_OBJ) $(PERF_FLETCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_FLETCH_OBJ) $(LDLIBS)

$(BUILD)/perf_record: $(PERF_CORE_OBJ) $(PERF_RECORD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_RECORD_OBJ) $(LDLIBS)

$(BUILD)/perf_syn: $(PERF_CORE_OBJ) $(PERF_SYN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_SYN_OBJ) $(LDLIBS)

$(BUILD)/perf_alloc: $(PERF_CORE_OBJ) $(PERF_ALLOC_OBJ) \
                     $(BUILD)/tests/unit/syn_toy.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_ALLOC_OBJ) $(BUILD)/tests/unit/syn_toy.o $(LDLIBS)

$(BUILD)/perf_symidx: $(PERF_CORE_OBJ) $(PERF_SYMIDX_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_SYMIDX_OBJ) $(LDLIBS)

$(BUILD)/perf_git_status: $(PERF_CORE_OBJ) $(PERF_GIT_STATUS_OBJ) \
		tests/fixtures/git/mkrepo.sh
	$(CC) $(CFLAGS) $(LDFLAGS) $(PERF_GIT_ALLOC_WRAP) \
		-o $@ $(PERF_CORE_OBJ) \
		$(PERF_GIT_STATUS_OBJ) $(LDLIBS)

$(BUILD)/perf_fuss: $(PERF_CORE_OBJ) $(PERF_FUSS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_FUSS_OBJ) $(LDLIBS)

$(BUILD)/perf_git_gutter: $(PERF_CORE_OBJ) $(PERF_GIT_GUTTER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_GIT_GUTTER_OBJ) $(LDLIBS)

$(BUILD)/perf_lsp: $(PERF_CORE_OBJ) $(PERF_LSP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_LSP_OBJ) $(LDLIBS)

$(BUILD)/perf_ai_http: $(PERF_CORE_OBJ) $(PERF_AI_HTTP_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_AI_HTTP_OBJ) $(LDLIBS)

$(BUILD)/tests/perf/perf_ai_shadow.o: CFLAGS += \
  -DYEW_TEST_MOCKAI='"$(abspath $(MOCKAI))"' \
  -DYEW_TEST_MOCKCURL='"$(abspath $(MOCKCURL))"'
$(BUILD)/tests/perf/perf_ai_shadow.o: $(MOCKAI) $(MOCKCURL)

$(BUILD)/perf_ai_shadow: $(PERF_CORE_OBJ) $(PERF_AI_SHADOW_OBJ) \
                         $(MOCKAI) $(MOCKCURL)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_AI_SHADOW_OBJ) $(LDLIBS)

$(BUILD)/perf_ai_privacy: $(PERF_CORE_OBJ) $(PERF_AI_PRIVACY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_AI_PRIVACY_OBJ) $(LDLIBS)

$(BUILD)/perf_plug: $(PERF_CORE_OBJ) $(PERF_PLUG_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_PLUG_OBJ) $(LDLIBS)

$(BUILD)/perf_pkg: $(PERF_CORE_OBJ) $(PERF_PKG_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_PKG_OBJ) $(LDLIBS)

$(BUILD)/perf_cloud: $(PERF_CORE_OBJ) $(PERF_CLOUD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_CLOUD_OBJ) $(LDLIBS)

$(BUILD)/calib_runner: $(CALIB_OBJ) $(BUILD)/src/util/calib.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CALIB_OBJ) \
		$(BUILD)/src/util/calib.o $(LDLIBS)

$(BUILD)/perf_batch: $(PERF_BATCH_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_BATCH_OBJ) $(LDLIBS)

$(BUILD)/perf_script_suite: $(PERF_SCRIPT_SUITE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_SCRIPT_SUITE_OBJ) $(LDLIBS)

$(BUILD)/fletch_run: $(FLETCH_CORE_OBJ) $(FLETCH_RUN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FLETCH_CORE_OBJ) \
		$(FLETCH_RUN_OBJ) $(LDLIBS)

$(BUILD)/script_runner: $(SCRIPT_RUNNER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCRIPT_RUNNER_OBJ) $(LDLIBS)

$(BUILD)/git_script: $(PERF_CORE_OBJ) $(GIT_SCRIPT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(GIT_SCRIPT_OBJ) $(LDLIBS)

$(BUILD)/fuss_commands: $(PERF_CORE_OBJ) $(FUSS_COMMANDS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(FUSS_COMMANDS_OBJ) $(LDLIBS)

$(BUILD)/git_hunks: $(PERF_CORE_OBJ) $(GIT_HUNKS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(GIT_HUNKS_OBJ) $(LDLIBS)

$(BUILD)/group_from_dir: $(PERF_CORE_OBJ) $(GROUP_FROM_DIR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(GROUP_FROM_DIR_OBJ) $(LDLIBS)

$(BUILD)/roundtrip_runner: $(ROUNDTRIP_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ROUNDTRIP_LINK_OBJ) $(LDLIBS)

$(BUILD)/perf_textbuf: $(PERF_CORE_OBJ) $(PERF_TEXTBUF_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_TEXTBUF_OBJ) $(LDLIBS)

$(BUILD)/perf_unicode: $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_UNICODE_OBJ) $(LDLIBS)

$(BUILD)/perf_render: $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) $(PERF_RENDER_OBJ) $(LDLIBS)

$(BUILD)/perf_shadow: $(PERF_CORE_OBJ) $(PERF_SHADOW_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(PERF_SHADOW_ALLOC_WRAP) -o $@ $(PERF_CORE_OBJ) \
		$(PERF_SHADOW_OBJ) $(LDLIBS)

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

$(BUILD)/perf_latency_s56: $(PERF_LATENCY_S56_OBJ) $(LIVE_PTY_OBJ) \
                          $(PTY_VT_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_LATENCY_S56_OBJ) \
		$(LIVE_PTY_OBJ) $(PTY_VT_OBJ) $(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_echo_child: $(PERF_ECHO_CHILD_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_ECHO_CHILD_OBJ) $(LDLIBS)

$(BUILD)/perf_startup_s56: $(PERF_STARTUP_OBJ) $(LIVE_PTY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_STARTUP_OBJ) \
		$(LIVE_PTY_OBJ) $(LDLIBS)

$(BUILD)/perf_nullexec: $(PERF_NULLEXEC_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_NULLEXEC_OBJ) $(LDLIBS)

$(BUILD)/perf_open_s56: $(PERF_OPEN_OBJ) $(LIVE_PTY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_OPEN_OBJ) \
		$(LIVE_PTY_OBJ) $(LDLIBS)

$(BUILD)/perf_mem_s56: $(PERF_MEM_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_MEM_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/s56_gate_policy_selftest: $(PERF_S56_GATE_POLICY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ \
		$(PERF_S56_GATE_POLICY_OBJ) $(LDLIBS)

$(BUILD)/perf_prof_crosscheck: $(PERF_S56_PROF_CROSSCHECK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ \
		$(PERF_S56_PROF_CROSSCHECK_OBJ) $(LDLIBS)

$(BUILD)/perf_re_throughput: $(PERF_RETHRU_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_RETHRU_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_search_latency: $(PERF_SEARCHLAT_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_SEARCHLAT_OBJ) \
		$(PERF_CORE_OBJ) $(LDLIBS)

$(BUILD)/perf_search_s56: $(PERF_SEARCH_S56_OBJ) $(PERF_CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_SEARCH_S56_OBJ) \
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

$(TORTURE_GIT_HUNK): $(PERF_CORE_OBJ) $(TORTURE_GIT_HUNK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PERF_CORE_OBJ) \
		$(TORTURE_GIT_HUNK_OBJ) $(LDLIBS)

$(FAULTSHIM): tests/torture/faultshim.c $(BUILD)/mods.stamp \
              $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -fPIC $(SHARED_FLAG) -o $@ $< \
		$(DL_LIBS) $(LDLIBS)

$(BUILD)/gen-unicode-tables: scripts/gen-unicode-tables.c \
                             $(BUILD)/profile.stamp $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD)/gen-compdb: scripts/gen-compdb.c $(BUILD)/profile.stamp \
                     $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(RUNTIME_BLOB_GEN): scripts/gen-runtime-blob.c | dirs
	$(HOSTCC) -std=c11 -pedantic -Wall -Wextra -Werror -Wvla -O2 \
		-o $@ $<

$(RUNTIME_BLOB_C): $(RUNTIME_BLOB_GEN) $(RUNTIME_BLOB_INPUTS) \
                   $(RUNTIME_BLOB_DIRS) | dirs
	@set -eu; \
	tmp='$@.tmp'; \
	trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
	$(RUNTIME_BLOB_GEN) runtime "$$tmp"; \
	if ! cmp -s "$$tmp" '$@'; then mv "$$tmp" '$@'; fi; \
	rm -f "$$tmp"; \
	trap - EXIT HUP INT TERM

$(RUNTIME_BLOB_OBJ): $(RUNTIME_BLOB_C) $(BUILD)/mods.stamp \
                     $(BUILD)/profile.stamp $(MODULE_FORCE) \
                     $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

runtime-blob-selftest:
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) HOSTCC='$(HOSTCC)' \
		scripts/tests/runtime-blob.test.sh

ifeq ($(EMBED_RUNTIME),1)
runtime-embedded-budget: $(RUNTIME_BLOB_OBJ)

ifeq ($(TARGET),arm64-macos)
	@set -eu; \
	out='$(BUILD)/tmp/runtime-embedded-size.out'; \
	if ! $(SIZE) '$(RUNTIME_BLOB_OBJ)' >"$$out"; then \
		echo 'cannot record the macOS embedded-runtime object size' >&2; \
		exit 1; \
	fi; \
	bytes=$$(awk 'NR == 2 && $$(NF - 1) ~ /^[0-9]+$$/ { print $$(NF - 1) }' "$$out"); \
	rm -f "$$out"; \
	if [ -z "$$bytes" ]; then \
		echo 'cannot parse the macOS embedded-runtime object size' >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "runtime.embedded $$bytes bytes (macOS record; not gated)"
else
	@set -eu; \
	out='$(BUILD)/tmp/runtime-embedded-size.out'; \
	if ! $(SIZE) -A -d '$(RUNTIME_BLOB_OBJ)' >"$$out"; then \
		echo 'cannot measure the embedded-runtime object' >&2; \
		exit 1; \
	fi; \
	bytes=$$(awk 'NR > 2 && $$2 ~ /^[0-9]+$$/ && \
		     $$1 ~ /^\.(text|rodata|data|bss)/ { n += $$2; seen = 1 } \
		     END { if (seen) print n }' "$$out"); \
	rm -f "$$out"; \
	if [ -z "$$bytes" ]; then \
		echo 'cannot parse the embedded-runtime object size' >&2; \
		exit 1; \
	fi; \
	printf '%s\n' "runtime.embedded $$bytes bytes (limit 225280)"; \
	if [ "$$bytes" -gt 225280 ]; then \
		echo 'runtime.embedded exceeds the 220 KiB budget' >&2; \
		exit 1; \
	fi
endif

runtime-embedded-e2e: $(BUILD)/yew scripts/tests/runtime-embedded-e2e.sh \
                      tests/script/57_embedded_runtime.fl
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) \
		YEW_BIN=$(abspath $(BUILD)/yew) \
		scripts/tests/runtime-embedded-e2e.sh

test-runtime-embedded: $(BUILD)/unit_tests runtime-embedded-budget \
                       runtime-embedded-e2e
	$(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
		$(BUILD)/unit_tests --filter runtime_asset
	$(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
		$(BUILD)/unit_tests --filter runtime_consumer
else
runtime-embedded-budget runtime-embedded-e2e test-runtime-embedded:
	@echo '$@ requires EMBED_RUNTIME=1' >&2; exit 2
endif

$(FAKECLIP): tests/unit/fakeclip.c $(BUILD)/mods.stamp \
             $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(FAKELSP): tests/helpers/fakelsp.c $(BUILD)/mods.stamp \
            $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(FAKECURL): tests/helpers/fakecurl.c $(BUILD)/mods.stamp \
             $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(FAKEHTTP): tests/helpers/fakehttp.c $(BUILD)/mods.stamp \
             $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(MOCKAI): tests/helpers/mockai.c $(BUILD)/mods.stamp \
           $(BUILD)/profile.stamp $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(MOCKCURL): tests/helpers/mockcurl.c tests/helpers/mockai.c \
             $(BUILD)/mods.stamp $(BUILD)/profile.stamp \
             $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
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
check: $(BUILD)/unit_tests $(BUILD)/yew $(AI_TEST_HELPERS) test-fletch test-script \
       test-syn-assets size-tools-selftest target-tools-selftest \
       static-pie-tools-selftest runtime-blob-selftest \
       $(PKG_TEST_TARGET)
	$(UNIT_RUN)
	scripts/bans.sh
	scripts/check-cmd-dispatch.sh
	scripts/check-fl-choke.sh
	scripts/check-input.sh
	scripts/check-render.sh
	scripts/check-sigsafe.sh
	scripts/check-plugin-docs.sh
	SMOKE_MODULES="$(MODULES)" scripts/smoke.sh $(BUILD)/yew
	@echo "check: ok (fast tier -- pty, torture, sanitizers and valgrind NOT run)"

test: $(BUILD)/unit_tests $(BUILD)/yew $(AI_TEST_HELPERS) test-pty test-fletch test-script \
      test-roundtrip test-record-corpus test-syn-corpus \
      test-syn-def-corpus test-syn-assets target-tools-selftest \
      static-pie-tools-selftest runtime-blob-selftest torture-build \
      $(PKG_TEST_TARGET)
	$(UNIT_RUN)
	scripts/bans.sh
	scripts/check-cmd-dispatch.sh
	scripts/check-fl-choke.sh
	scripts/check-input.sh
	scripts/check-render.sh
	scripts/check-sigsafe.sh
	scripts/check-plugin-docs.sh
	SMOKE_MODULES="$(MODULES)" scripts/smoke.sh $(BUILD)/yew
	$(TORTURE_LIVE_GATE)

test-pkg: $(BUILD)/yew
	tests/pkg/run.sh $(BUILD)/yew

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
      fuzz-mouse fuzz-groups fuzz-shadow fuzz-record fuzz-syn fuzz-syn-def \
      fuzz-symidx fuzz-json fuzz-jsonrpc $(FUSS_FUZZ_TARGET) \
      $(LSP_FUZZ_TARGET) $(AI_FUZZ_TARGET) $(PKG_FUZZ_TARGET)
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
		$(BUILD)/fuzz_fl_parse --seconds=$(FUZZ_SECONDS) --seed=$(FUZZ_SEED); \
	fi

fuzz-plug-manifest: $(BUILD)/fuzz_fl_parse
	$(BUILD)/fuzz_fl_parse --seconds=$(PLUG_FUZZ_SECONDS) --seed=$(FUZZ_SEED)

fuzz-pkg-tree: $(BUILD)/fuzz_pkg_tree
	$(BUILD)/fuzz_pkg_tree --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-pkg-tree-long: $(BUILD)/fuzz_pkg_tree
	$(BUILD)/fuzz_pkg_tree --seconds=$(PLUG_FUZZ_SECONDS) --seed=$(FUZZ_SEED)

fuzz-ai: $(BUILD)/fuzz_http $(BUILD)/fuzz_ai_stream \
         $(BUILD)/fuzz_ai_shadow $(BUILD)/fuzz_ai_redact
	$(BUILD)/fuzz_http --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_ai_stream --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_ai_shadow --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	$(BUILD)/fuzz_ai_redact --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-groups: $(BUILD)/fuzz_groups
	@set -eu; \
	for seed in $(GROUPS_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_groups --iters=$(GROUPS_FUZZ_SESSIONS) \
			--seed=$$seed; \
	done

fuzz-shadow: $(BUILD)/fuzz_shadow
	@set -eu; \
	for seed in $(SHADOW_FUZZ_SEEDS); do \
		YEW_SHADOW_TEST=0 $(BUILD)/fuzz_shadow \
			--iters=$(SHADOW_FUZZ_ITERS) --seed=$$seed; \
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

fuzz-json: $(BUILD)/fuzz_json
	$(BUILD)/fuzz_json --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-jsonrpc: $(BUILD)/fuzz_jsonrpc
	$(BUILD)/fuzz_jsonrpc --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-lsp-msg: $(BUILD)/fuzz_lsp_msg
	$(BUILD)/fuzz_lsp_msg --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-lsp-resp: $(BUILD)/fuzz_lsp_resp
	@set -eu; \
	for seed in $(LSP_RESP_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_lsp_resp --iters=$(LSP_RESP_FUZZ_ITERS) \
			--seed=$$seed; \
	done

test-lsp-live: $(LSP_LIVE_BIN)
	YEW_LSP_CI_REQUIRED=$(YEW_LSP_CI_REQUIRED) \
		tests/lsp/clangd_ci.sh $(abspath $(LSP_LIVE_BIN))

test-record-corpus: $(BUILD)/fuzz_record
	$(BUILD)/fuzz_record --corpus-only

fuzz-syn: $(BUILD)/fuzz_syn
	$(BUILD)/fuzz_syn --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)
	@set -eu; \
	for seed in $(SYN_FUZZ_SEEDS); do \
		YEW_SYN_FUZZ_EDIT_OPS=$(SYN_FUZZ_OPS) \
			$(BUILD)/fuzz_syn --iters=1 --seed=$$seed; \
	done

fuzz-syn-long: fuzz-syn-line-long fuzz-syn-edit-long

fuzz-syn-line-long: $(BUILD)/fuzz_syn
	$(BUILD)/fuzz_syn --seconds=$(SYN_FUZZ_SECONDS) --seed=$(FUZZ_SEED)

fuzz-syn-edit-long: $(BUILD)/fuzz_syn
	YEW_SYN_FUZZ_EDIT_OPS=$(SYN_FUZZ_OPS) \
		$(BUILD)/fuzz_syn --seconds=$(SYN_FUZZ_SECONDS) --seed=$(FUZZ_SEED)

test-syn-corpus: $(BUILD)/fuzz_syn
	$(BUILD)/fuzz_syn --corpus-only

fuzz-syn-def: $(BUILD)/fuzz_syn_def
	$(BUILD)/fuzz_syn_def --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-symidx: $(BUILD)/fuzz_symidx
	@set -eu; \
	for seed in 0x44 0x4401 0x4402 0x4403; do \
		$(BUILD)/fuzz_symidx --iters=$(FUZZ_ITERS) --seed=$$seed; \
	done

fuzz-porcelain: $(BUILD)/fuzz_porcelain
	@set -eu; \
	for seed in $(PORCELAIN_FUZZ_SEEDS); do \
		$(BUILD)/fuzz_porcelain --iters=$(FUZZ_ITERS) --seed=$$seed; \
	done

fuzz-fuss: $(BUILD)/fuzz_fuss
	$(BUILD)/fuzz_fuss --iters=$(FUSS_FUZZ_ITERS) --seed=$(FUZZ_SEED)

fuzz-git-diff: $(BUILD)/fuzz_git_diff
	$(BUILD)/fuzz_git_diff --iters=$(FUZZ_ITERS) --seed=$(FUZZ_SEED)

test-syn-def-corpus: $(BUILD)/fuzz_syn_def
	$(BUILD)/fuzz_syn_def --corpus-only

test-syn-pack-long: $(BUILD)/unit_tests
	YEW_SYN_PACK_LONG=1 YEW_SYN_PACK_ROTATE=$(SYN_PACK_ROTATE) \
		$(BUILD)/unit_tests --filter syn_all_new_pack_long_sanitizer_lane

test-syn-assets: $(BUILD)/yew
	YEW_RUNTIME_DIR='$(abspath runtime)' \
		scripts/check-syn-assets.sh $(BUILD)/yew

syn-goldens: $(BUILD)/yew
	YEW_RUNTIME_DIR='$(abspath runtime)' \
		scripts/gen-syn-goldens.sh $(BUILD)/yew

syn-fuzz-seeds:
	scripts/gen-syn-fuzz-corpus.sh

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

# Manual deep-dive build.  The recursive invocation keeps profiling objects
# disjoint from every ordinary/sanitized tree and preserves the release -O2
# shape while retaining usable frame-pointer call stacks.
perf-symbols:
	$(MAKE) --no-print-directory BUILD=build-prof \
		EXTRA_CFLAGS=-fno-omit-frame-pointer build-prof/yew

SIZE_FULL_BUILD := $(SIZE_ROOT)/full
SIZE_MINIMAL_BUILD := $(SIZE_ROOT)/minimal
SIZE_LSP_BUILD := $(SIZE_ROOT)/lsp-only
SIZE_AI_BUILD := $(SIZE_ROOT)/ai-only
SIZE_FUSS_BUILD := $(SIZE_ROOT)/fuss-only
SIZE_PLUGINS_BUILD := $(SIZE_ROOT)/plugins-only
SIZE_MEASURE := $(SIZE_ROOT)/measure
SIZE_FULL_BIN := $(SIZE_MEASURE)/full/yew
SIZE_MINIMAL_BIN := $(SIZE_MEASURE)/minimal/yew
SIZE_LSP_BIN := $(SIZE_MEASURE)/lsp-only/yew
SIZE_AI_BIN := $(SIZE_MEASURE)/ai-only/yew
SIZE_FUSS_BIN := $(SIZE_MEASURE)/fuss-only/yew
SIZE_PLUGINS_BIN := $(SIZE_MEASURE)/plugins-only/yew

size-tools-selftest:
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) scripts/tests/size-tools.test.sh

define size_measure_rule
$(1): FORCE
	$$(MAKE) --no-print-directory BUILD='$(2)' MODULES='$(3)' \
		SHIPPING=1 '$(2)/yew'
	mkdir -p '$$(dir $(1))'
	cp '$(2)/yew' '$(1)'
	$$(STRIP) $$(STRIPFLAGS) '$(1)'
endef

$(eval $(call size_measure_rule,$(SIZE_FULL_BIN),$(SIZE_FULL_BUILD),lsp ai fuss plugins))
$(eval $(call size_measure_rule,$(SIZE_MINIMAL_BIN),$(SIZE_MINIMAL_BUILD),))
$(eval $(call size_measure_rule,$(SIZE_LSP_BIN),$(SIZE_LSP_BUILD),lsp))
$(eval $(call size_measure_rule,$(SIZE_AI_BIN),$(SIZE_AI_BUILD),ai))
$(eval $(call size_measure_rule,$(SIZE_FUSS_BIN),$(SIZE_FUSS_BUILD),fuss))
$(eval $(call size_measure_rule,$(SIZE_PLUGINS_BIN),$(SIZE_PLUGINS_BUILD),plugins))

size: size-tools-selftest $(SIZE_FULL_BIN) $(SIZE_MINIMAL_BIN) \
      $(SIZE_LSP_BIN) $(SIZE_AI_BIN) $(SIZE_FUSS_BIN) $(SIZE_PLUGINS_BIN)
	scripts/size.sh --budgets tests/size/budgets.txt \
		full=$(SIZE_FULL_BIN) minimal=$(SIZE_MINIMAL_BIN) \
		lsp-only=$(SIZE_LSP_BIN) ai-only=$(SIZE_AI_BIN) \
		fuss-only=$(SIZE_FUSS_BIN) plugins-only=$(SIZE_PLUGINS_BIN)

size-ledger-full: $(SIZE_FULL_BIN)
	NM='$(NM)' SIZE='$(SIZE)' CC='$(CC)' \
		scripts/size-ledger.sh --build '$(SIZE_FULL_BUILD)' \
		--binary '$(SIZE_FULL_BIN)'

size-ledger-minimal: $(SIZE_MINIMAL_BIN)
	NM='$(NM)' SIZE='$(SIZE)' CC='$(CC)' \
		scripts/size-ledger.sh --build '$(SIZE_MINIMAL_BUILD)' \
		--binary '$(SIZE_MINIMAL_BIN)'

calib: $(BUILD)/calib_runner
	@set -eu; \
	args=''; \
	if test -n '$(CALIB_REFERENCE)'; then \
		args="--reference $(CALIB_REFERENCE)"; \
	fi; \
	mkdir -p $$(dirname '$(CALIB_OUTPUT)'); \
	$(BUILD)/calib_runner --runner-id '$(PERF_RUNNER_ID)' $$args \
		> '$(CALIB_OUTPUT).tmp'; \
	key=$$(awk '$$1 == "cache_key" { print $$2 }' \
		'$(CALIB_OUTPUT).tmp'); \
	test -n "$$key"; \
	mkdir -p $(BUILD)/calib-cache; \
	cp '$(CALIB_OUTPUT).tmp' $(BUILD)/calib-cache/$$key.txt; \
	mv '$(CALIB_OUTPUT).tmp' '$(CALIB_OUTPUT)'; \
	cat '$(CALIB_OUTPUT)'

perf-render: $(BUILD)/perf_render
	$(BUILD)/perf_render

alloc:
	$(MAKE) --no-print-directory BUILD=build-adbg ALLOCDBG=1 \
		test-alloc-debug perf-alloc

perf-alloc: $(BUILD)/perf_alloc
	@if test '$(ALLOCDBG)' != 1; then \
		echo "perf-alloc requires ALLOCDBG=1" >&2; exit 2; \
	fi
	mkdir -p $(BUILD)/tmp
	TMPDIR=$(abspath $(BUILD)/tmp) $(BUILD)/perf_alloc

perf-shadow: $(BUILD)/perf_shadow
	YEW_SHADOW_TEST=0 $(BUILD)/perf_shadow

perf-scroll: $(BUILD)/perf_scroll
	$(BUILD)/perf_scroll

perf-piece: $(BUILD)/perf_piece
	$(BUILD)/perf_piece

perf:
	PERF_GATE='$(PERF_GATE)' BUILD='$(BUILD)' \
		PERF_RUNNER_ID='$(PERF_RUNNER_ID)' \
		CALIB_REFERENCE='$(CALIB_REFERENCE)' \
		scripts/run-perf-suite.sh '$(MAKE)'

perf-components: perf-unicode perf-render perf-shadow perf-scroll perf-piece perf-cursor perf-undo perf-textbuf \
      perf-latency perf-jobstream perf-re-pathological \
      perf-re-throughput perf-search-latency \
      perf-units perf-multicursor perf-cmdcomp perf-state perf-finder \
      perf-mouse perf-record perf-syn perf-symidx perf-batch \
      $(if $(filter 1,$(PERF_S56_COLLECT)),perf-s56-functional) \
      $(FUSS_PERF_TARGET) \
      $(LSP_PERF_TARGET) $(AI_PERF_TARGET) $(PLUG_PERF_TARGET)

perf-git-status: $(BUILD)/perf_git_status
	$(BUILD)/perf_git_status

perf-fuss: $(BUILD)/perf_fuss
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_fuss

perf-git-gutter: $(BUILD)/perf_git_gutter
	$(BUILD)/perf_git_gutter --gate

perf-lsp: $(BUILD)/perf_lsp
	$(BUILD)/perf_lsp

perf-ai-http: $(BUILD)/perf_ai_http
	$(BUILD)/perf_ai_http

perf-ai-shadow: $(BUILD)/perf_ai_shadow $(MOCKAI) $(MOCKCURL)
	$(BUILD)/perf_ai_shadow --selftest-policy
	YEW_AI_MOCK=1 $(BUILD)/perf_ai_shadow

perf-ai-privacy: $(BUILD)/perf_ai_privacy
	$(BUILD)/perf_ai_privacy

perf-plug: $(BUILD)/perf_plug
	$(BUILD)/perf_plug

perf-pkg: $(BUILD)/perf_pkg
	$(BUILD)/perf_pkg

perf-cloud: $(BUILD)/perf_cloud
	$(BUILD)/perf_cloud

perf-ai-http-valgrind: $(BUILD)/perf_ai_http
	valgrind --quiet --error-exitcode=99 --leak-check=full \
		--errors-for-leak-kinds=definite --track-fds=yes \
		--child-silent-after-fork=yes $(BUILD)/perf_ai_http --cycles-only

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
	$(BUILD)/perf_record $(if $(filter 1,$(PERF_GATE)),--gate,)

perf-syn: $(BUILD)/perf_syn $(BUILD)/yew
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_syn --gate

perf-syn-scroll-s56: $(BUILD)/perf_syn $(BUILD)/yew
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) \
		$(BUILD)/perf_syn --gate-scroll-s56

perf-symidx: $(BUILD)/perf_symidx
	$(BUILD)/perf_symidx

perf-syn-budgets: $(BUILD)/perf_syn $(BUILD)/yew
	$(BUILD)/perf_syn --gate-budgets

perf-syn-gate-selftest: $(BUILD)/perf_syn
	$(BUILD)/perf_syn --selftest-gate

perf-syn-line-probe: $(BUILD)/perf_syn
	$(BUILD)/perf_syn --probe-legacy-line='$(PERF_SYN_PROBE_STEM)'

perf-syn-resident-line-probe: $(BUILD)/perf_syn
	$(BUILD)/perf_syn --probe-resident-line='$(PERF_SYN_PROBE_STEM)'

perf-syn-edit-probe: $(BUILD)/perf_syn
	$(BUILD)/perf_syn --probe-legacy-edit='$(PERF_SYN_PROBE_STEM)'

perf-syn-quiet: $(BUILD)/perf_syn $(BUILD)/yew
	YEW_PERF_SYN_COMMAND='$(abspath $(BUILD)/perf_syn) --gate' \
		scripts/run-perf-syn-quiet.sh

perf-syn-size: $(BUILD)/yew
	CC=$(CC) MODULES='$(MODULES)' \
		scripts/check-s42_5-binary-growth.sh $(abspath $(BUILD)/yew)

perf-batch: $(BUILD)/perf_batch $(BUILD)/yew
	$(BUILD)/perf_batch --yew $(abspath $(BUILD)/yew) --gate

perf-batch-selftest: $(BUILD)/perf_batch $(BUILD)/yew
	@if YEW_BATCH_INJECT_NS=12000000 $(BUILD)/perf_batch \
		--yew $(abspath $(BUILD)/yew) --gate; then \
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
fl-gc-stress: $(BUILD)/unit_tests
	$(UNIT_RUNTIME_PREP) $(MUSL_UNIT_PREP) $(UNIT_RUNTIME_ENV) \
		YEW_FL_GC_STRESS=1 \
		$(if $(filter 1,$(SAN)),YEW_TORTURE_CLEAN_ONLY=1 YEW_TEST_INSTRUMENTED=1,) \
		$(if $(filter 1,$(VALGRIND)),YEW_TEST_INSTRUMENTED=1 $(VALGRIND_RUN),) \
		$(BUILD)/unit_tests $(MUSL_UNIT_EXCLUDES) $(UNIT_DEATH_EXCLUDES)

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

perf-latency: $(BUILD)/perf_latency $(BUILD)/yew
	$(BUILD)/perf_latency --yew $(abspath $(BUILD)/yew) \
		--baseline $(LATENCY_BASELINE)

perf-re-pathological: $(BUILD)/perf_re_pathological
	$(BUILD)/perf_re_pathological --baseline $(PERF_COMPONENT_LIMITS)

perf-re-throughput: $(BUILD)/perf_re_throughput
	$(BUILD)/perf_re_throughput --baseline $(PERF_COMPONENT_LIMITS)

perf-search-latency: $(BUILD)/perf_search_latency
	$(BUILD)/perf_search_latency --baseline $(PERF_COMPONENT_LIMITS)

perf-jobstream: $(BUILD)/perf_jobstream $(BUILD)/yew
	$(BUILD)/perf_jobstream --yew $(abspath $(BUILD)/yew) \
		--baseline $(LATENCY_BASELINE)

# Proves the gate reacts: an injected paint delay must fail it.
perf-jobstream-selftest: $(BUILD)/perf_jobstream $(BUILD)/yew
	@if YEW_JOBSTREAM_KEYS=60 YEW_JOBSTREAM_INJECT_NS=6000000 \
		$(BUILD)/perf_jobstream --yew $(abspath $(BUILD)/yew) \
		--baseline $(LATENCY_BASELINE); then \
		echo 'error: jobstream gate accepted injected delay' >&2; \
		exit 1; \
	else \
		echo 'jobstream selftest: injected delay correctly rejected'; \
	fi

perf-latency-selftest: $(BUILD)/perf_latency $(BUILD)/yew
	$(BUILD)/perf_latency --selftest-exit-drain
	$(BUILD)/perf_latency --selftest-quiet-drain
	@if YEW_LATENCY_KEYS=100 YEW_LATENCY_INJECT_NS=6000000 \
		$(BUILD)/perf_latency --yew $(abspath $(BUILD)/yew) \
		--baseline $(LATENCY_BASELINE); then \
		echo 'error: latency gate accepted injected paint delay' >&2; \
		exit 1; \
	else \
		echo 'perf-latency-selftest: injected delay rejected'; \
	fi

# Sprint 56's end-to-end harnesses deliberately keep their names separate
# from the earlier per-editor latency gate.  Unknown and hosted runners are
# advisory; PERF_GATE=1 remains meaningful only under run-perf-suite.sh's
# calibrated designated-runner preflight.
perf-latency-s56-check: $(BUILD)/perf_latency_s56 $(BUILD)/perf_echo_child
	$(BUILD)/perf_latency_s56 --check-scripts tests/perf/sessions
	$(BUILD)/perf_latency_s56 --check-assist-vt
	$(BUILD)/perf_latency_s56 --check-frame-tags
	$(BUILD)/perf_latency_s56 --floor --echo \
		$(abspath $(BUILD)/perf_echo_child)

perf-latency-s56-smoke: perf-latency-s56-check $(BUILD)/yew
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture small \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)

perf-latency-s56-typing-huge: perf-latency-s56-check $(BUILD)/yew \
                                fixtures-quick
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture huge \
		--path $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--state $(abspath $(BUILD)/perf-s56-state)

perf-latency-s56-syntax: perf-latency-s56-check $(BUILD)/yew
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/edit.keys --fixture syntax \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/edit.keys --fixture syntax \
		--path tests/perf/fixtures/syn/c_comment_bomb.c \
		--state $(abspath $(BUILD)/perf-s56-state)

perf-latency-s56-multicursor: perf-latency-s56-check $(BUILD)/yew
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/multicursor.keys --fixture small \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)

perf-latency-s56-search-huge: perf-latency-s56-check $(BUILD)/yew \
                                fixtures-quick
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/search.keys --fixture huge \
		--path $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--state $(abspath $(BUILD)/perf-s56-state)

perf-latency-s56-many: perf-latency-s56-check $(BUILD)/yew
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/navigate.keys \
		--fixture many-buffers \
		--path $(abspath $(BUILD)/perf-s56-many/buf-00.c) \
		--many-dir $(abspath $(BUILD)/perf-s56-many) \
		--state $(abspath $(BUILD)/perf-s56-state)

$(PERF_S56_WORKSPACE_READY): tests/perf/fixtures/syn/c_kitchen.c
	@mkdir -p $(BUILD)/perf-s56-many
	@set -eu; n=0; while test $$n -lt 50; do \
		name=$$(printf '%02d' $$n); \
		cp tests/perf/fixtures/syn/c_kitchen.c \
			$(BUILD)/perf-s56-many/buf-$$name.c; \
		n=$$((n + 1)); \
	done
	@: > $@

perf-latency-s56-many: $(PERF_S56_WORKSPACE_READY)

perf-latency-s56-assist: perf-latency-s56-check $(BUILD)/yew \
                         $(FAKELSP) $(MOCKAI)
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture assist \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--fakelsp $(abspath $(FAKELSP)) \
		--mockai $(abspath $(MOCKAI)) \
		--ai-script $(abspath tests/fixtures/ai/ollama.script)

perf-latency-s56-matrix: perf-latency-s56-smoke \
                         perf-latency-s56-typing-huge \
                         perf-latency-s56-syntax \
                         perf-latency-s56-multicursor \
                         perf-latency-s56-many \
                         perf-latency-s56-search-huge \
                         perf-latency-s56-assist

perf-startup-s56: $(BUILD)/perf_startup_s56 $(BUILD)/perf_nullexec \
                  $(BUILD)/yew $(PERF_S56_WORKSPACE_READY)
	@mkdir -p $(BUILD)/perf-s56-state $(BUILD)/perf-s56-fixtures
	@: > $(BUILD)/perf-s56-fixtures/empty.c
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_startup_s56 --yew $(abspath $(BUILD)/yew) \
		--nullexec $(abspath $(BUILD)/perf_nullexec) \
		--fixture $(abspath $(BUILD)/perf-s56-fixtures/empty.c) \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--budgets tests/perf/budgets.txt \
		--workspace $(abspath $(BUILD)/perf-s56-many)

perf-open-s56: $(BUILD)/perf_open_s56 $(BUILD)/yew fixtures-quick
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_open_s56 --yew $(abspath $(BUILD)/yew) \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--budgets tests/perf/budgets.txt \
		--fixture-code $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--fixture-utf8 $(abspath $(FIXTURE_DIR)/100m-utf8.bin) \
		--fixture-allnl $(abspath $(FIXTURE_DIR)/100m-allnl.bin)

perf-mem-s56: $(BUILD)/perf_mem_s56 $(BUILD)/perf_startup_s56 \
              $(BUILD)/perf_nullexec $(BUILD)/perf_open_s56 \
              $(BUILD)/perf_latency_s56 $(BUILD)/yew fixtures-quick \
              $(PERF_S56_WORKSPACE_READY) $(FAKELSP) $(MOCKAI) \
              tests/fixtures/ai/ollama.script
	@mkdir -p $(BUILD)/perf-s56-state $(BUILD)/perf-s56-fixtures \
		$(BUILD)/perf-s56-logs
	@: > $(BUILD)/perf-s56-fixtures/empty.c
	@rm -f $(BUILD)/perf-s56-logs/default.log \
		$(BUILD)/perf-s56-logs/clean.log \
		$(BUILD)/perf-s56-logs/code.log \
		$(BUILD)/perf-s56-logs/utf8.log \
		$(BUILD)/perf-s56-logs/allnl.log \
		$(BUILD)/perf-s56-logs/workspace.log \
		$(BUILD)/perf-s56-logs/typing.log \
		$(BUILD)/perf-s56-logs/assist.log
	@YEW_PROF=1 YEW_PERF_SMOKE=1 \
		YEW_PERF_ADVISORY=1 PERF_GATE=0 \
		YEW_PERF_LOG_DEFAULT=$(abspath $(BUILD)/perf-s56-logs/default.log) \
		YEW_PERF_LOG_CLEAN=$(abspath $(BUILD)/perf-s56-logs/clean.log) \
		YEW_PERF_LOG_WORKSPACE=$(abspath $(BUILD)/perf-s56-logs/workspace.log) \
		$(BUILD)/perf_startup_s56 --yew $(abspath $(BUILD)/yew) \
		--nullexec $(abspath $(BUILD)/perf_nullexec) \
		--fixture $(abspath $(BUILD)/perf-s56-fixtures/empty.c) \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--budgets tests/perf/budgets.txt \
		--workspace $(abspath $(BUILD)/perf-s56-many) >/dev/null
	@YEW_PROF=1 YEW_PERF_SMOKE=1 \
		YEW_PERF_ADVISORY=1 PERF_GATE=0 \
		YEW_PERF_LOG_CODE=$(abspath $(BUILD)/perf-s56-logs/code.log) \
		YEW_PERF_LOG_UTF8=$(abspath $(BUILD)/perf-s56-logs/utf8.log) \
		YEW_PERF_LOG_ALLNL=$(abspath $(BUILD)/perf-s56-logs/allnl.log) \
		$(BUILD)/perf_open_s56 --yew $(abspath $(BUILD)/yew) \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--budgets tests/perf/budgets.txt \
		--fixture-code $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--fixture-utf8 $(abspath $(FIXTURE_DIR)/100m-utf8.bin) \
		--fixture-allnl $(abspath $(FIXTURE_DIR)/100m-allnl.bin) >/dev/null
	@YEW_PROF=1 YEW_PERF_ADVISORY=1 PERF_GATE=0 \
		YEW_PERF_LOG=$(abspath $(BUILD)/perf-s56-logs/typing.log) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture small \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state) >/dev/null
	@YEW_PROF=1 YEW_PERF_ADVISORY=1 PERF_GATE=0 \
		YEW_PERF_LOG=$(abspath $(BUILD)/perf-s56-logs/assist.log) \
		$(BUILD)/perf_latency_s56 --yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture assist \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--fakelsp $(abspath $(FAKELSP)) \
		--mockai $(abspath $(MOCKAI)) \
		--ai-script $(abspath tests/fixtures/ai/ollama.script) >/dev/null
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_mem_s56 --budgets tests/perf/budgets.txt \
		--yew $(abspath $(BUILD)/yew) \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--log-default $(abspath $(BUILD)/perf-s56-logs/default.log) \
		--log-clean $(abspath $(BUILD)/perf-s56-logs/clean.log) \
		--log-code $(abspath $(BUILD)/perf-s56-logs/code.log) \
		--log-utf8 $(abspath $(BUILD)/perf-s56-logs/utf8.log) \
		--log-allnl $(abspath $(BUILD)/perf-s56-logs/allnl.log) \
		--log-workspace $(abspath $(BUILD)/perf-s56-logs/workspace.log) \
		--log-typing $(abspath $(BUILD)/perf-s56-logs/typing.log) \
		--log-assist $(abspath $(BUILD)/perf-s56-logs/assist.log) \
		--fixture-code $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--fixture-utf8 $(abspath $(FIXTURE_DIR)/100m-utf8.bin) \
		--fixture-allnl $(abspath $(FIXTURE_DIR)/100m-allnl.bin)

perf-s56-gate-selftest: $(BUILD)/s56_gate_policy_selftest \
                        $(BUILD)/perf_prof_crosscheck \
                        perf-baseline-selftest
	$(BUILD)/s56_gate_policy_selftest
	$(BUILD)/perf_prof_crosscheck --selftest-policy
	scripts/tests/s56-perf-gate.test.sh
	scripts/tests/run-perf-suite.test.sh
	scripts/tests/update-perf-suite.test.sh
	scripts/tests/s56-baseline-guard.test.sh

perf-prof-crosscheck-s56: $(BUILD)/perf_prof_crosscheck \
                          $(BUILD)/perf_latency_s56 $(BUILD)/yew \
                          fixtures-quick $(PERF_S56_WORKSPACE_READY) \
                          $(FAKELSP) $(MOCKAI) \
                          tests/fixtures/ai/ollama.script
	@mkdir -p $(BUILD)/perf-s56-state
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture small \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture huge \
		--path $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/edit.keys --fixture syntax \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/edit.keys --fixture syntax \
		--path tests/perf/fixtures/syn/c_comment_bomb.c \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/multicursor.keys --fixture small \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/navigate.keys \
		--fixture many-buffers \
		--path $(abspath $(BUILD)/perf-s56-many/buf-00.c) \
		--many-dir $(abspath $(BUILD)/perf-s56-many) \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/search.keys --fixture huge \
		--path $(abspath $(FIXTURE_DIR)/100m-code.bin) \
		--state $(abspath $(BUILD)/perf-s56-state)
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_prof_crosscheck \
		--runner $(abspath $(BUILD)/perf_latency_s56) \
		--yew $(abspath $(BUILD)/yew) \
		--session tests/perf/sessions/typing.keys --fixture assist \
		--path tests/perf/fixtures/syn/c_kitchen.c \
		--state $(abspath $(BUILD)/perf-s56-state) \
		--fakelsp $(abspath $(FAKELSP)) \
		--mockai $(abspath $(MOCKAI)) \
		--ai-script $(abspath tests/fixtures/ai/ollama.script)

perf-s56-observation: perf-latency-s56-matrix perf-syn-scroll-s56 \
                      perf-search-s56-smoke \
                      perf-startup-s56 perf-open-s56 perf-mem-s56 \
                      perf-prof-crosscheck-s56

perf-s56-huge-observation: perf-search-s56

perf-s56-checks: perf-s56-gate-selftest

perf-s56-functional: perf-s56-observation perf-s56-checks

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
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_textbuf \
		--fixtures $(FIXTURE_DIR) --baseline $(PERF_BASELINE) \
		--runner-id $(PERF_RUNNER_ID)

perf-baseline-selftest: $(BUILD)/perf_textbuf
	$(BUILD)/perf_textbuf --baseline-selftest

perf-search-s56: $(BUILD)/perf_search_s56 fixtures
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) PERF_GATE=$(PERF_GATE) \
		$(BUILD)/perf_search_s56 --budgets tests/perf/budgets.txt \
		--fixture-code $(abspath $(FIXTURE_DIR)/1g-code.bin) \
		--fixture-noline $(abspath $(FIXTURE_DIR)/1g-noline.bin)

perf-search-s56-smoke: $(BUILD)/perf_search_s56 $(BUILD)/gen-bigfile
	@mkdir -p $(BUILD)/perf-search-smoke
	$(BUILD)/gen-bigfile --profile 1g-code --size $(PERF_SEARCH_SMOKE_BYTES) \
		--seed 0x9e3779b97f4a7c15 \
		--output $(BUILD)/perf-search-smoke/1g-code.bin
	$(BUILD)/gen-bigfile --profile 1g-noline --size $(PERF_SEARCH_SMOKE_BYTES) \
		--seed 0x9e3779b97f4a7c15 \
		--output $(BUILD)/perf-search-smoke/1g-noline.bin
	YEW_PERF_SMOKE=1 YEW_PERF_ADVISORY=1 PERF_GATE=0 \
		$(BUILD)/perf_search_s56 --budgets tests/perf/budgets.txt \
		--fixture-code $(abspath $(BUILD)/perf-search-smoke/1g-code.bin) \
		--fixture-noline $(abspath $(BUILD)/perf-search-smoke/1g-noline.bin)

perf-huge:
	PERF_GATE='$(PERF_GATE)' BUILD='$(BUILD)' \
		PERF_RUNNER_ID='$(PERF_RUNNER_ID)' \
		CALIB_REFERENCE='$(CALIB_REFERENCE)' \
		scripts/run-perf-suite.sh '$(MAKE)' huge

perf-huge-components: $(BUILD)/perf_textbuf fixtures
	YEW_PERF_ADVISORY=$(PERF_ADVISORY) $(BUILD)/perf_textbuf \
		--fixtures $(FIXTURE_DIR) --baseline $(PERF_BASELINE) \
		--runner-id $(PERF_RUNNER_ID) --huge

perf-update:
	BUILD='$(BUILD)' FIXTURE_DIR='$(FIXTURE_DIR)' \
		PERF_RUNNER_ID='$(PERF_RUNNER_ID)' \
		PERF_BASELINE='$(PERF_BASELINE)' \
		CALIB_REFERENCE='$(CALIB_REFERENCE)' \
		scripts/update-perf-suite.sh '$(MAKE)'

perf-baseline-guard:
	scripts/perf-baseline-guard.sh

perf-gate-selftest: $(BUILD)/perf_textbuf fixtures-quick
	@if YEW_PERF_INJECT_OPEN_DELAY=1 YEW_PERF_ADVISORY=0 \
		$(BUILD)/perf_textbuf --fixtures $(FIXTURE_DIR) \
		--baseline $(PERF_BASELINE) \
		--runner-id perf-x86_64-linux-gnu; then \
		echo 'error: performance gate accepted injected delay' >&2; \
		exit 1; \
	else \
		echo 'perf-gate-selftest: injected delay rejected'; \
	fi

torture-build: $(BUILD)/yew $(TORTURE_CHILD) $(TORTURE_LIVE) \
               $(TORTURE_DRIVER) $(TORTURE_BATCH) $(FAULTSHIM) \
               $(if $(filter fuss,$(MODULES)),$(TORTURE_GIT_HUNK),)

torture-live-check: torture-build
	YEW_TORTURE_CLEAN_ONLY=1 \
	YEW_TORTURE_CHECKER=$(abspath $(TORTURE_CHILD)) \
	YEW_TORTURE_YEW=$(abspath $(BUILD)/yew) \
	YEW_RUNTIME_DIR=$(abspath runtime) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) --trace-children=yes,) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_LIVE)) \
		$(abspath $(FAULTSHIM))

torture-batch: torture-build
	$(TORTURE_BATCH) --yew $(abspath $(BUILD)/yew) \
		--checker $(abspath $(TORTURE_CHILD))

torture: torture-build $(FUSS_TORTURE_TARGET)
	YEW_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_CHILD)) \
		$(abspath $(FAULTSHIM))
	YEW_TORTURE_CHECKER=$(abspath $(TORTURE_CHILD)) \
	YEW_TORTURE_YEW=$(abspath $(BUILD)/yew) \
	YEW_RUNTIME_DIR=$(abspath runtime) \
	YEW_TORTURE_LANE=live-editor \
	YEW_TORTURE_SIGKILL_ITERS=$(TORTURE_SIGKILL_ITERS) \
		$(TORTURE_DRIVER) $(abspath $(TORTURE_LIVE)) \
		$(abspath $(FAULTSHIM))
	$(TORTURE_BATCH) --yew $(abspath $(BUILD)/yew) \
		--checker $(abspath $(TORTURE_CHILD))

torture-git-hunk: $(TORTURE_GIT_HUNK)
	@if ! command -v git >/dev/null 2>&1; then \
		echo 'HARNESS_SKIP git_hunk_kill9: git not found'; \
	else \
		set -e; \
		tmp=$$(mktemp -d); \
		trap 'find "$$tmp" -depth -delete' EXIT HUP INT TERM; \
		LC_ALL=C $(TORTURE_GIT_HUNK) "$$tmp"; \
	fi

unicode-tables: $(BUILD)/gen-unicode-tables
	$< ucd/16.0.0 > src/unicode/tables.c
	$< --word-break ucd/16.0.0 > src/unicode/tables_wb.c
	$< --case ucd/16.0.0 > src/unicode/tables_case.c
	$< --category ucd/16.0.0 > src/unicode/tables_cat.c

# Read the literal selection on every invocation, but schedule the stamp only
# when its content changed so objects are not rebuilt spuriously.
$(BUILD)/mods.stamp: $(MODULE_FORCE) | dirs
	@if ! printf '%s\n' '$(MODULES)' | cmp -s - $@; then \
		rm -f $(OBJ) $(OBJ:.o=.d) $(UNIT_OBJ) $(UNIT_OBJ:.o=.d) \
		      $(SYN_ENGINE_UNIT_OBJ) $(SYN_ENGINE_UNIT_OBJ:.o=.d); \
		printf '%s\n' '$(MODULES)' > $@; \
	fi

# A BUILD tree cannot safely retain objects across target/compiler/profile
# changes.  Unlike the module stamp, no source set changes here: forcing every
# selected object to rebuild is enough, and avoids a second concurrent cleanup
# recipe racing the module stamp's removal of unreachable objects.
$(BUILD)/profile.stamp: $(PROFILE_FORCE) | dirs
	@if ! printf '%s\n' '$(BUILD_PROFILE_KEY)' | cmp -s - $@; then \
		printf '%s\n' '$(BUILD_PROFILE_KEY)' > $@; \
	fi

# The one file allowed out of the C11 box, and only when the
# computed-goto dispatcher is on.  Deferred to here so it overrides the
# FINAL CFLAGS: a target-specific `:=` earlier in the file would snapshot
# the value before the sanitizer lane has added its own flags.
ifeq ($(FL_CGOTO),1)
$(BUILD)/src/fl/vm.o: CFLAGS := \
  $(subst -std=c11,-std=gnu11,$(CFLAGS)) -Wno-pedantic
endif

$(BUILD)/%.o: %.c $(BUILD)/mods.stamp $(BUILD)/profile.stamp \
              $(MODULE_FORCE) $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

$(SYN_ENGINE_UNIT_OBJ): src/syn/engine.c $(BUILD)/mods.stamp \
                        $(BUILD)/profile.stamp $(MODULE_FORCE) \
                        $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) -DYEW_SYN_TEST=1 -c -o $@ $<

$(STATE_LEGACY_OBJ): src/ws/state_legacy.c src/ws/fl_parse.c \
                     src/ws/fl_emit.c $(BUILD)/mods.stamp \
                     $(BUILD)/profile.stamp $(MODULE_FORCE) \
                     $(PROFILE_FORCE) | dirs
	$(CC) $(CFLAGS) -DYEW_STATE_LEGACY=1 -c -o $@ $<

dirs:
	mkdir -p $(BUILD_DIRS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD)/yew $(DESTDIR)$(PREFIX)/bin/yew
	install -d $(DESTDIR)$(PREFIX)/share/yew/runtime
	install -m 0644 runtime/init.fl \
		$(DESTDIR)$(PREFIX)/share/yew/runtime/init.fl
	install -m 0644 runtime/ai-deny.fl runtime/preset.ai-local.fl \
		runtime/preset.ai-cloud.fl runtime/preset.cloud.fl \
		$(DESTDIR)$(PREFIX)/share/yew/runtime/
	install -d $(DESTDIR)$(PREFIX)/share/yew/docs
	install -m 0644 docs/ai-privacy.md \
		$(DESTDIR)$(PREFIX)/share/yew/docs/ai-privacy.md
	install -d $(DESTDIR)$(PREFIX)/share/yew/runtime/syntax
	install -m 0644 runtime/syntax/*.fl \
		$(DESTDIR)$(PREFIX)/share/yew/runtime/syntax/
	install -d $(DESTDIR)$(PREFIX)/share/yew/runtime/themes
	install -m 0644 runtime/themes/*.fl \
		$(DESTDIR)$(PREFIX)/share/yew/runtime/themes/

clean:
	rm -rf $(BUILD)

test-script: $(BUILD)/script_runner $(BUILD)/yew $(FAKELSP) \
             $(FUSS_SCRIPT_TARGET)
	LC_ALL=C YEW_SCRIPT_BUDGET_MS=$(YEW_SCRIPT_BUDGET_MS) \
		$(if $(filter 1,$(VALGRIND)),YEW_TEST_INSTRUMENTED=1,) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) \
		--trace-children=yes \
		$(VALGRIND_TRACE_SKIP),) \
		$(BUILD)/script_runner --selftest \
		--yew $(abspath $(BUILD)/yew)
	LC_ALL=C YEW_SCRIPT_BUDGET_MS=$(YEW_SCRIPT_BUDGET_MS) \
		$(if $(filter 1,$(VALGRIND)),YEW_TEST_INSTRUMENTED=1,) \
		$(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN) \
		--trace-children=yes \
		$(VALGRIND_TRACE_SKIP),) \
		$(BUILD)/script_runner \
		$(SCRIPT_RUNNER_ARGS) \
		--yew $(abspath $(BUILD)/yew) \
		--fakelsp $(abspath $(FAKELSP))

test-git-script: $(BUILD)/git_script tests/fixtures/git/mkrepo.sh \
                 tests/fixtures/git/hashes.txt
	@if ! command -v git >/dev/null 2>&1; then \
		echo 'HARNESS_SKIP git_layer: git not found'; \
	else \
		set -e; \
		tmp=$$(mktemp -d); \
		trap 'find "$$tmp" -depth -delete' EXIT HUP INT TERM; \
		tests/fixtures/git/mkrepo.sh "$$tmp/repo"; \
		LC_ALL=C $(BUILD)/git_script "$$tmp/repo"; \
	fi

test-fuss-commands: $(BUILD)/fuss_commands
	@if ! command -v git >/dev/null 2>&1; then \
		echo 'HARNESS_SKIP fuss_commands: git not found'; \
	else \
		set -e; \
		tmp=$$(mktemp -d); \
		trap 'find "$$tmp" -depth -delete' EXIT HUP INT TERM; \
		LC_ALL=C $(BUILD)/fuss_commands "$$tmp"; \
	fi

test-git-hunks: $(BUILD)/git_hunks
	@if ! command -v git >/dev/null 2>&1; then \
		echo 'HARNESS_SKIP git_hunks: git not found'; \
	else \
		set -e; \
		tmp=$$(mktemp -d); \
		trap 'find "$$tmp" -depth -delete' EXIT HUP INT TERM; \
		LC_ALL=C $(BUILD)/git_hunks "$$tmp"; \
	fi

test-group-from-dir: $(BUILD)/group_from_dir
	@if ! command -v git >/dev/null 2>&1; then \
		echo 'HARNESS_SKIP group_from_dir: git not found'; \
	else \
		set -e; \
		tmp=$$(mktemp -d); \
		trap 'find "$$tmp" -depth -delete' EXIT HUP INT TERM; \
		LC_ALL=C $(BUILD)/group_from_dir "$$tmp"; \
	fi

test-script-determinism: $(BUILD)/script_runner $(BUILD)/yew $(FAKELSP)
	@set -e; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	LC_ALL=C $(BUILD)/script_runner --selftest \
		--yew $(abspath $(BUILD)/yew) >"$$tmp/run-1" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner \
		$(SCRIPT_RUNNER_ARGS) \
		--yew $(abspath $(BUILD)/yew) \
		--fakelsp $(abspath $(FAKELSP)) >>"$$tmp/run-1" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner --selftest \
		--yew $(abspath $(BUILD)/yew) >"$$tmp/run-2" 2>&1; \
	LC_ALL=C $(BUILD)/script_runner \
		$(SCRIPT_RUNNER_ARGS) \
		--yew $(abspath $(BUILD)/yew) \
		--fakelsp $(abspath $(FAKELSP)) >>"$$tmp/run-2" 2>&1; \
	diff -u "$$tmp/run-1" "$$tmp/run-2"; \
	echo 'test-script-determinism: ok'

test-script-budget: $(BUILD)/perf_script_suite $(BUILD)/script_runner \
                    $(BUILD)/yew $(FAKELSP) $(SCRIPT_SUITE_BASELINE)
	$(BUILD)/perf_script_suite --selftest
	LC_ALL=C $(BUILD)/perf_script_suite \
		--runner $(abspath $(BUILD)/script_runner) \
		--yew $(abspath $(BUILD)/yew) \
		--baseline $(SCRIPT_SUITE_BASELINE)

# The conformance suite (Sprint 33).  LC_ALL=C is set rather than
# assumed: run.c sorts with strcmp and the ledger is byte-compared.
# The RUNNER goes under valgrind; the `yew fl` children deliberately do
# not (no --trace-children).  Wrapping 37 subprocess spawns would turn a
# 0.2 s lane into minutes for a check the sanitize lane already makes
# with ASan -- and the runner is the part with the file descriptors and
# the allocation bookkeeping that --track-fds and --leak-check exist to
# police.
test-fletch: $(BUILD)/fletch_run $(BUILD)/yew
	LC_ALL=C $(if $(filter 1,$(VALGRIND)),$(VALGRIND_RUN),) \
		$(BUILD)/fletch_run --yew $(abspath $(BUILD)/yew)
	BUILD=$(BUILD) scripts/check-fletch-coverage.sh
	BUILD=$(BUILD) scripts/check-fletch-meta.sh
	BUILD=$(BUILD) scripts/check-fletch-gate-selftest.sh

# PERF_GATE=1 enforces; every other lane runs it ungated, because a
# bench that is never executed outside the perf runner rots.
BASELINE ?= dev
bench-fletch: $(BUILD)/perf_fletch
	$(BUILD)/perf_fletch --selftest-gate
	$(BUILD)/perf_fletch --baseline $(BASELINE) \
		$(if $(filter 1,$(PERF_GATE)),--gate,--gate-budgets)

fletch-ledger: $(BUILD)/fletch_run $(BUILD)/yew
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--yew $(abspath $(BUILD)/yew) >tests/fletch/ledger.txt

# DoD 11: the whole suite under both dispatchers, outputs byte-compared.
#
# Separate BUILD dirs, both from clean.  A reused build directory
# produces deterministic FALSE results here -- half the objects come
# from the other setting and the two binaries are then the same binary.
test-fletch-dispatch:
	@rm -rf $(BUILD)-flc-sw $(BUILD)-flc-cg
	$(MAKE) --no-print-directory BUILD=$(BUILD)-flc-sw FL_CGOTO=0 \
		$(BUILD)-flc-sw/yew $(BUILD)-flc-sw/fletch_run
	$(MAKE) --no-print-directory BUILD=$(BUILD)-flc-cg FL_CGOTO=1 \
		$(BUILD)-flc-cg/yew $(BUILD)-flc-cg/fletch_run
	LC_ALL=C $(BUILD)-flc-sw/fletch_run \
		--yew $(abspath $(BUILD)-flc-sw/yew) \
		>$(BUILD)-flc-sw/out.txt 2>&1
	LC_ALL=C $(BUILD)-flc-cg/fletch_run \
		--yew $(abspath $(BUILD)-flc-cg/yew) \
		>$(BUILD)-flc-cg/out.txt 2>&1
	cmp $(BUILD)-flc-sw/out.txt $(BUILD)-flc-cg/out.txt
	@echo 'test-fletch-dispatch: both dispatchers agree, byte for byte'

# DoD 11: the whole suite under the collector's stress mode, minus any
# file carrying a justified `# GC_STRESS: 0`.  The runner prints and
# asserts the opt-out count, so the escape hatch cannot become the norm.
test-fletch-gc-stress: $(BUILD)/fletch_run $(BUILD)/yew
	YEW_FL_GC_STRESS=1 LC_ALL=C $(BUILD)/fletch_run \
		--yew $(abspath $(BUILD)/yew)

# Determinism: the suite twice and the ledger twice, byte-compared.
test-fletch-determinism: $(BUILD)/fletch_run $(BUILD)/yew
	LC_ALL=C $(BUILD)/fletch_run --yew $(abspath $(BUILD)/yew) \
		>$(BUILD)/fletch-run-1.txt 2>&1
	LC_ALL=C $(BUILD)/fletch_run --yew $(abspath $(BUILD)/yew) \
		>$(BUILD)/fletch-run-2.txt 2>&1
	cmp $(BUILD)/fletch-run-1.txt $(BUILD)/fletch-run-2.txt
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--yew $(abspath $(BUILD)/yew) >$(BUILD)/fletch-led-1.txt
	LC_ALL=C $(BUILD)/fletch_run --ledger \
		--yew $(abspath $(BUILD)/yew) >$(BUILD)/fletch-led-2.txt
	cmp $(BUILD)/fletch-led-1.txt $(BUILD)/fletch-led-2.txt
	@echo 'test-fletch-determinism: two runs identical, ledger stable'

test-roundtrip: $(BUILD)/roundtrip_runner
	YEW_RT_TMP=$(BUILD)/tmp LC_ALL=C $(BUILD)/roundtrip_runner

test-roundtrip-coverage: $(BUILD)/roundtrip_runner
	YEW_RT_TMP=$(BUILD)/tmp LC_ALL=C $(BUILD)/roundtrip_runner --coverage

test-fletch-roundtrip: test-roundtrip

test-pty: $(BUILD)/pty_runner $(BUILD)/demo_paint $(BUILD)/yew $(FAKELSP) \
          $(AI_TEST_HELPERS)
	$(PTY_PREP) $(PTY_RUN) --demo $(abspath $(BUILD)/demo_paint) \
		--yew $(abspath $(BUILD)/yew) $(PTY_LOG_REDIRECT)

-include $(OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(SYN_ENGINE_UNIT_OBJ:.o=.d) \
         $(STATE_LEGACY_OBJ:.o=.d) \
         $(FUZZ_LIB_OBJ:.o=.d) \
         $(FUZZ_UTF8_OBJ:.o=.d) $(FUZZ_GRAPHEME_OBJ:.o=.d) \
         $(FUZZ_INPUT_OBJ:.o=.d) $(FUZZ_GRID_OBJ:.o=.d) \
         $(FUZZ_VT_OBJ:.o=.d) $(FUZZ_UNDO_OBJ:.o=.d) \
         $(FUZZ_TEXTBUF_OBJ:.o=.d) $(TEXT_FUZZ_SUPPORT_OBJ:.o=.d) \
         $(FUZZ_MULTICURSOR_OBJ:.o=.d) \
         $(FUZZ_SHADOW_OBJ:.o=.d) \
         $(FUZZ_FLAPI_OBJ:.o=.d) $(FUZZ_RECORD_OBJ:.o=.d) \
         $(FUZZ_SYN_OBJ:.o=.d) \
         $(FUZZ_SYN_DEF_OBJ:.o=.d) \
         $(FUZZ_SYMIDX_OBJ:.o=.d) \
         $(FUZZ_JSON_OBJ:.o=.d) $(FUZZ_JSONRPC_OBJ:.o=.d) \
         $(FUZZ_FUSS_OBJ:.o=.d) \
         $(FUZZ_LSP_MSG_OBJ:.o=.d) $(FUZZ_LSP_RESP_OBJ:.o=.d) \
         $(FUZZ_HTTP_OBJ:.o=.d) $(FUZZ_AI_STREAM_OBJ:.o=.d) \
         $(FUZZ_AI_SHADOW_OBJ:.o=.d) $(FUZZ_AI_REDACT_OBJ:.o=.d) \
         $(FUZZ_PKG_TREE_OBJ:.o=.d) \
         $(LSP_LIVE_OBJ:.o=.d) \
         $(FUZZ_CMDPARSE_OBJ:.o=.d) $(FUZZ_RECOMPILE_OBJ:.o=.d) \
         $(FUZZ_REDIFF_OBJ:.o=.d) $(RE_REF_OBJ:.o=.d) \
         $(PTY_ORACLE_OBJ:.o=.d) \
         $(PTY_HARNESS_OBJ:.o=.d) $(PTY_REGISTRY_OBJ:.o=.d) \
         $(PTY_RUNNER_OBJ:.o=.d) $(PTY_DEMO_OBJ:.o=.d) \
         $(FLETCH_RUN_OBJ:.o=.d) $(SCRIPT_RUNNER_OBJ:.o=.d) \
         $(GIT_SCRIPT_OBJ:.o=.d) $(GIT_HUNKS_OBJ:.o=.d) \
         $(FUSS_COMMANDS_OBJ:.o=.d) \
         $(GROUP_FROM_DIR_OBJ:.o=.d) \
         $(ROUNDTRIP_OBJ:.o=.d) \
         $(PERF_UNICODE_OBJ:.o=.d) $(PERF_RENDER_OBJ:.o=.d) \
         $(PERF_SHADOW_OBJ:.o=.d) \
         $(PERF_PIECE_OBJ:.o=.d) $(PERF_CURSOR_OBJ:.o=.d) \
         $(PERF_UNDO_OBJ:.o=.d) $(PERF_TEXTBUF_OBJ:.o=.d) \
         $(PERF_PKG_OBJ:.o=.d) $(PERF_CLOUD_OBJ:.o=.d) \
         $(PERF_LATENCY_OBJ:.o=.d) $(PERF_LATENCY_S56_OBJ:.o=.d) \
         $(PERF_ECHO_CHILD_OBJ:.o=.d) $(PERF_STARTUP_OBJ:.o=.d) \
         $(PERF_NULLEXEC_OBJ:.o=.d) $(PERF_OPEN_OBJ:.o=.d) \
         $(PERF_MEM_OBJ:.o=.d) $(PERF_ALLOC_OBJ:.o=.d) \
         $(PERF_S56_GATE_POLICY_OBJ:.o=.d) \
         $(PERF_S56_PROF_CROSSCHECK_OBJ:.o=.d) \
         $(PERF_JOBSTREAM_OBJ:.o=.d) \
         $(PERF_REPATH_OBJ:.o=.d) $(PERF_RETHRU_OBJ:.o=.d) \
         $(PERF_SEARCH_S56_OBJ:.o=.d) \
         $(LIVE_PTY_OBJ:.o=.d) \
         $(PERF_MULTICURSOR_OBJ:.o=.d) \
         $(PERF_CMDCOMP_OBJ:.o=.d) \
         $(PERF_STATE_OBJ:.o=.d) \
         $(PERF_FINDER_OBJ:.o=.d) $(PERF_MOUSE_OBJ:.o=.d) \
         $(PERF_FLETCH_OBJ:.o=.d) $(PERF_RECORD_OBJ:.o=.d) \
         $(PERF_BATCH_OBJ:.o=.d) \
         $(PERF_SYN_OBJ:.o=.d) \
         $(PERF_SYMIDX_OBJ:.o=.d) \
         $(PERF_LSP_OBJ:.o=.d) \
         $(PERF_AI_HTTP_OBJ:.o=.d) $(PERF_AI_SHADOW_OBJ:.o=.d) \
         $(PERF_AI_PRIVACY_OBJ:.o=.d) $(PERF_FUSS_OBJ:.o=.d) \
         $(PERF_SCRIPT_SUITE_OBJ:.o=.d) \
         $(GEN_BIGFILE_OBJ:.o=.d) \
         $(TORTURE_CHILD_OBJ:.o=.d) \
	 $(TORTURE_DRIVER_OBJ:.o=.d) $(TORTURE_LIVE_OBJ:.o=.d) \
	 $(TORTURE_BATCH_OBJ:.o=.d)

FORCE:
