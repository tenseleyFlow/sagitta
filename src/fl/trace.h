#ifndef SAG_FL_TRACE_H
#define SAG_FL_TRACE_H

/*
 * Sprint 32 §6: runtime error quality.
 *
 * Spec §9 makes a raised error a map with `kind` and `msg`; the VM adds
 * `trace` -- a list of `"name (file:line:col)"` strings -- WHEN THE
 * ERROR ESCAPES EVERY FRAME.  Errors a `catch` claims carry kind and
 * msg only, because a try/catch in a tight loop must not pay for string
 * formatting it will discard.
 *
 * THERE IS NO SEPARATE SOURCE TABLE.  §6 calls for an `FlSrcTab`
 * mapping file_id to path and bytes; DiagCtx already is one, and it
 * holds the source that was actually COMPILED rather than whatever is
 * on disk now.  That makes §6's size+mtime staleness check unnecessary
 * rather than unimplemented: the caret cannot point at a line the
 * compiler never saw, because it quotes the bytes the compiler read.
 * A caret is skipped only when the source is genuinely unavailable --
 * a file_id the context does not know.
 */

#include "fl/diag.h"
#include "fl/vm.h"
#include "util/buf.h"

enum {
    /* Past this, the middle is elided.  Infinite recursion raises
     * "limit" at 256 frames and must not print 256 lines. */
    FL_TRACE_MAX_FRAMES = 32,
    FL_TRACE_HEAD = 16,
    FL_TRACE_TAIL = 16
};

/*
 * Builds the trace from the live frame stack and stores it on
 * `vm->err` under `trace`.  Called from the unwinder; a no-op when
 * `vm->err` is not a map.
 */
void fl_trace_attach(FlVm *vm);

/*
 * The printed block: the `error: kind: msg` line, one `  at ...` line
 * per frame, and the caret for the innermost frame when its source is
 * available.  Appends to `out` and writes nothing else anywhere.
 */
void fl_trace_render(FlVm *vm, FlValue err, Bytebuf *out);

#endif /* SAG_FL_TRACE_H */
