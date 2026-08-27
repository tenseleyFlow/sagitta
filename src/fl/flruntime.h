#ifndef YEW_FL_FLRUNTIME_H
#define YEW_FL_FLRUNTIME_H

/* Persistent editor embedding and the Sprint 34 hook dispatch bridge. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "fl/flhook.h"
#include "util/base.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct FlRuntime FlRuntime;
typedef struct FlVm FlVm;
typedef struct Win Win;

bool yew_fl_runtime_init(Ed *ed);
void yew_fl_runtime_free(Ed *ed);
FlVm *yew_fl_vm(Ed *ed);

/* Evaluate one E-mode entry in the editor's persistent VM.  Globals and
 * registered hooks survive subsequent entries; the source is arena-owned
 * so retained closures keep valid trace spans. */
CmdStatus yew_fl_eval(Ed *ed, const char *source, u32 len);

/* The most recently rendered compiler diagnostic, retained by the runtime
 * until the next compile attempt.  The returned string belongs to `rt`. */
const char *fl_runtime_last_diag(const FlRuntime *rt, FlSpan *span);

/* Compile an arena-owned script for later execution.  `label` is copied and
 * is used only in diagnostics/traces. */
FlFn *fl_compile_str(FlRuntime *rt, const u8 *source, size_t len,
                     const char *label);
/* Compile a top-level CLI script with its real path and full user authority. */
FlFn *fl_compile_script(FlRuntime *rt, const u8 *source, size_t len,
                        const char *realpath_label);
/* Batch-profiler variant: emits explicit top-level statement markers. */
FlFn *fl_compile_script_profiled(FlRuntime *rt, const u8 *source, size_t len,
                                 const char *realpath_label);

/* Execute a compiled script in the persistent editor VM.  A call made from
 * inside Fletch nests without resetting the caller's frames; a host call is
 * one ordinary outer VM transaction.  The source is visible to every editor
 * command reached by the chunk for the duration of the call. */
bool fl_call_chunk(FlRuntime *rt, FlFn *fn, CmdSource source);
/* Execute a retained closure with its own globals map.  Config files use
 * this entry so each origin can be torn down without sharing globals. */
bool fl_call_value(FlRuntime *rt, FlValue callable, CmdSource source);
/* The argument-bearing form used by host-managed plugin lifecycle calls.
 * `out` receives the callable's result on success. */
bool fl_call_value_args(FlRuntime *rt, FlValue callable,
                        const FlValue *args, u32 nargs, CmdSource source,
                        FlValue *out);

/* Sprint 35's bounded register cache.  Registers are lower-case a..z.  A hit
 * requires byte-identical source, not merely matching register metadata.
 * Cached functions are GC roots for the lifetime of the runtime. */
FlFn *fl_macro_compile_cached(FlRuntime *rt, u8 reg,
                              const u8 *source, size_t len);
void fl_macro_cache_invalidate(FlRuntime *rt, u8 reg);

/* Command source inherited by Fletch editor bindings and motion blocks. */
CmdSource fl_runtime_cmd_source(const FlVm *vm);

/* Native behind the global on(event, fn) prelude entry. */
bool fl_runtime_on(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);

void yew_fl_hook_fire(Ed *ed, FlEvent event,
                      const FlValue *args, u8 nargs);
void yew_fl_hook_buffer(Ed *ed, FlEvent event, Buffer *buffer);
void yew_fl_hook_mode(Ed *ed, FlEvent event, const char *mode);
void yew_fl_hook_window(Ed *ed, FlEvent event, Win *win);
void yew_fl_hook_workspace(Ed *ed, FlEvent event);

/* Snapshot/flush pair used around one drained input burst.  Every cursor's
 * stable identity and position participates, not only the primary cursor. */
u64 yew_fl_cursor_burst_state(const Win *win);
void yew_fl_hook_flush_cursor(Ed *ed, Win *win, u64 before);

/* Text changes enqueue here; the loop flushes once after the input burst. */
void yew_fl_hook_note_change(Ed *ed, Buffer *buffer, u64 at);
void yew_fl_hook_flush_change(Ed *ed);

#endif /* YEW_FL_FLRUNTIME_H */
