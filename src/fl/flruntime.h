#ifndef SAG_FL_FLRUNTIME_H
#define SAG_FL_FLRUNTIME_H

/* Persistent editor embedding and the Sprint 34 hook dispatch bridge. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/flhook.h"
#include "util/base.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct FlRuntime FlRuntime;
typedef struct FlVm FlVm;
typedef struct Win Win;

bool sag_fl_runtime_init(Ed *ed);
void sag_fl_runtime_free(Ed *ed);
FlVm *sag_fl_vm(Ed *ed);

/* Evaluate one E-mode entry in the editor's persistent VM.  Globals and
 * registered hooks survive subsequent entries; the source is arena-owned
 * so retained closures keep valid trace spans. */
CmdStatus sag_fl_eval(Ed *ed, const char *source, u32 len);

/* Compile an arena-owned script for later execution.  `label` is copied and
 * is used only in diagnostics/traces. */
FlFn *fl_compile_str(FlRuntime *rt, const u8 *source, size_t len,
                     const char *label);

/* Execute a compiled script in the persistent editor VM.  A call made from
 * inside Fletch nests without resetting the caller's frames; a host call is
 * one ordinary outer VM transaction.  The source is visible to every editor
 * command reached by the chunk for the duration of the call. */
bool fl_call_chunk(FlRuntime *rt, FlFn *fn, CmdSource source);

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

void sag_fl_hook_fire(Ed *ed, FlEvent event,
                      const FlValue *args, u8 nargs);
void sag_fl_hook_buffer(Ed *ed, FlEvent event, Buffer *buffer);
void sag_fl_hook_mode(Ed *ed, FlEvent event, const char *mode);
void sag_fl_hook_window(Ed *ed, FlEvent event, Win *win);
void sag_fl_hook_workspace(Ed *ed, FlEvent event);

/* Snapshot/flush pair used around one drained input burst.  Every cursor's
 * stable identity and position participates, not only the primary cursor. */
u64 sag_fl_cursor_burst_state(const Win *win);
void sag_fl_hook_flush_cursor(Ed *ed, Win *win, u64 before);

/* Text changes enqueue here; the loop flushes once after the input burst. */
void sag_fl_hook_note_change(Ed *ed, Buffer *buffer, u64 at);
void sag_fl_hook_flush_change(Ed *ed);

#endif /* SAG_FL_FLRUNTIME_H */
