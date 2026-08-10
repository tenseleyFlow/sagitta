#ifndef YEW_FL_HANDLE_H
#define YEW_FL_HANDLE_H

/*
 * Sprint 34 deliverable 1: the five editor handle types, spec §4.
 *
 * THE WHOLE DESIGN IS ONE SENTENCE: no FlValue ever contains an editor
 * pointer.  A script can hold a value for as long as it likes, and the
 * editor can close the object underneath it at any moment; a raw
 * pointer in a script value is a time bomb with a delay measured in
 * user sessions.  DoD 2's grep asserts this file declares no `Buffer
 * *`, `Win *`, `Cursor *` or `TextBuf *` struct field.
 *
 * TWO INDEPENDENT GUARDS, BECAUSE THEY CATCH DIFFERENT BUGS:
 *
 *   - The slot GENERATION guards handle-slot reuse.  A script keeping
 *     a value after fl_h_free sees a gen mismatch even though the slot
 *     is live again for someone else.
 *   - The stored OBJECT ID guards object death.  fl_h_buf looks
 *     as.buf up in the workspace, and a closed buffer is simply
 *     absent, so a handle taken before buf.close() fails at resolution
 *     rather than resolving to whatever now occupies the memory.
 *
 * Neither subsumes the other.  Drop the id indirection and a recycled
 * Buffer* detonates; drop the generation and a recycled HANDLE slot
 * points at a live but wrong object.
 *
 * GENERATION WRAPAROUND RETIRES THE SLOT.  `gen` starts at 1 and is
 * bumped on free; if the bump would reach 0 the slot is marked
 * FL_H_NONE and never returned to the free list.  A wrapped generation
 * makes a four-billion-frees-ago handle valid again, which is strictly
 * worse than leaking one slot.
 *
 * GC CONTRACT (gc.h rule 3).  Handles are scalars with no heap
 * children, so the collector needs no knowledge of them and cannot
 * collect a live buffer.  The mirror problem -- FlValues the HOST
 * retains, like hook closures -- is what fl_gc_host_root_add exists
 * for; see gc.h.
 *
 * FL_H_RE is the only OWNING kind: it holds an Arena with the compiled
 * program, because s20's YewRe is arena-owned and immutable.  It is
 * therefore the one place a leak is possible, and the one kind with a
 * bounded-RSS test.
 *
 * RESOURCE-LIFETIME LIMIT.  Stable objects and equal live spans are
 * canonicalized, but two distinct retained span values cannot be merged
 * or recycled: handles are scalar values, so the GC cannot prove that a
 * script discarded either one.  Their marks therefore live until explicit
 * handle release or buffer close.  Reclaiming arbitrary distinct spans
 * sooner requires a future traced wrapper or a script-visible close law;
 * guessing would silently invalidate retained values.
 */

#include <stdbool.h>

#include "fl/origin.h"
#include "fl/value.h"
#include "text/coords.h"
#include "text/mark.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct Buffer Buffer;
typedef struct Win Win;
typedef struct Cursor Cursor;
typedef struct YewRe YewRe;
typedef struct FlVm FlVm;

typedef enum {
    FL_H_NONE = 0, FL_H_BUF, FL_H_CUR, FL_H_SPAN, FL_H_WIN, FL_H_RE,
    FL_H__N
} FlHandleKind;

/*
 * The FlValue payload for a handle: 64 bits, no pointer, ever.  It
 * rides in the `as` union, which value.h sized at eight bytes for
 * exactly this.
 */
typedef struct FlHandle {
    u32 slot;
    u32 gen;
} FlHandle;

_Static_assert(sizeof(FlHandle) == 8, "a handle is 64 bits (s34 DoD 2)");

typedef struct FlHandleSlot {
    u8 kind;             /* FL_H_NONE = free                             */
    u32 gen;             /* bumped on free; a stale gen never resolves   */
    u32 next_free;
    union {
        u32 buf;                                    /* FL_H_BUF          */
        /* `stamp` is the CursorSet's stable identity.  `index` is only
         * a resolution hint and is refreshed when normalization moves
         * the cursor. */
        struct { u32 win; u32 index; u64 stamp; } cur; /* FL_H_CUR       */
        struct { u32 buf; MarkId lo, hi; } span;    /* FL_H_SPAN         */
        u32 win;                                    /* FL_H_WIN          */
        /*
         * The arena is held by POINTER, not by value.  The slot array
         * is reallocated as it grows, and s20 hands out YewRe pointers
         * INTO the arena's blocks -- an Arena that moves is fine for
         * the blocks but not for anything that took its address.
         */
        struct { Arena *a; YewRe *re; } re;         /* FL_H_RE: owned    */
    } as;
} FlHandleSlot;

typedef struct FlHandleTable {
    FlHandleSlot *slots;
    u32 n;
    u32 cap;
    u32 free_head;       /* index + 1; 0 = the list is empty             */
    u64 live;
} FlHandleTable;

void fl_h_table_init(FlHandleTable *t);
void fl_h_table_free(FlHandleTable *t);

/*
 * `init` supplies the payload only; kind, gen and next_free are the
 * table's business.  Returns a value tagged with the FlType matching
 * `k`.
 */
FlValue fl_h_make(FlHandleTable *t, FlHandleKind k,
                  const FlHandleSlot *init);
/* Explicit close.  False when the value is not a live handle.  The
 * editor is required because closing a span must release its marks. */
bool fl_h_free(Ed *ed, FlValue v);

/* True for the five handle type tags. */
bool fl_h_is(FlValue v);
FlHandleKind fl_h_kind_of(FlValue v);
FlHandle fl_h_decode(FlValue v);

/*
 * Resolution without raising: for `s.valid()`, `c.valid()` and the
 * places that must ask rather than fail.
 */
bool fl_h_alive(const FlHandleTable *t, FlValue v);
const FlHandleSlot *fl_h_peek(const FlHandleTable *t, FlValue v);
FlHandleSlot *fl_h_peek_mut(FlHandleTable *t, FlValue v);

/*
 * THE RESOLVERS.  NULL (or false) plus a raised spec-§9 error of kind
 * "handle" on any failure -- wrong tag, dead slot, stale generation, or
 * a live slot whose object has since been closed.
 */
Buffer *fl_h_buf(FlVm *vm, FlValue v);
Win *fl_h_win(FlVm *vm, FlValue v);
Cursor *fl_h_cur(FlVm *vm, FlValue v, Win **out_win);
bool fl_h_span(FlVm *vm, FlValue v, Buffer **out_buf, Span *out);
const YewRe *fl_h_re(FlVm *vm, FlValue v);

/* Constructors used by the bindings; they take the editor so the table
 * is found the one way.  Stable editor identities and equal live spans
 * are canonicalized: repeated queries return the same scalar handle and
 * do not grow the handle or mark tables. */
FlValue fl_h_buf_make(Ed *ed, const Buffer *b);
FlValue fl_h_win_make(Ed *ed, const Win *w);
FlValue fl_h_cur_make(Ed *ed, const Win *w, u32 index);
/* Mark-backed: `lo` biases left and `hi` biases right, so an insert at
 * either end grows the span (s09 bias rules).  A zero-length span at
 * `lo == hi` is legal and is what b.mark(off) hands back. */
FlValue fl_h_span_make(Ed *ed, Buffer *b, u64 lo, u64 hi);
FlValue fl_h_re_make(Ed *ed, Arena *own, YewRe *re);

/*
 * Releases every handle naming `buf_id` and drops its marks.  Called
 * from the buffer-close path BEFORE the MarkSet goes away: a span whose
 * marks outlive their set is the one shape neither guard catches.
 */
void fl_h_drop_buffer(Ed *ed, u32 buf_id);
void fl_h_drop_window(Ed *ed, u32 win_id);

#endif /* YEW_FL_HANDLE_H */
