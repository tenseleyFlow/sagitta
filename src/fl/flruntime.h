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
