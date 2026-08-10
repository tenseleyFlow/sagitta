#ifndef YEW_EDIT_JUMPLIST_H
#define YEW_EDIT_JUMPLIST_H

/*
 * Sprint 21 §5: navigation history.
 *
 * Two rings with the same entry shape and opposite ownership:
 *
 *   jumplist   per WINDOW  — where this *view* has been.  Two panes on
 *                            one file have independent histories; that
 *                            is the entire point of two panes.
 *   changelist per BUFFER  — where this *text* was edited.  A change is
 *                            a property of the content, so it is
 *                            reachable from any view of it.
 *
 * Positions are marks, never offsets.  An offset recorded before an
 * edit above it points at the wrong line afterwards, and the jumplist
 * is precisely the structure that gets read long after the edits that
 * invalidated it.  A mark rides the Sprint 9 choke point for free.
 *
 * Sprint 25 persists both; this sprint lands the serializer and calls
 * it from nobody.  Nothing here opens a file (DoD 11).
 */

#include "edit/cmd.h"
#include "text/coords.h"
#include "text/mark.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;
typedef struct Win Win;
typedef struct Buffer Buffer;

typedef struct JumpEntry {
    u32 buf_id;
    MarkId mark;
    /* Display and recovery only.  When the buffer has been closed the
     * mark is gone but buf_id + line can still reopen the file there. */
    LineNo line_hint;
    u64 stamp_ms;
} JumpEntry;

#define YEW_JUMPLIST_MAX 100
#define YEW_CHANGELIST_MAX 100
/* Consecutive edits within this window collapse into one entry. */
#define YEW_CHANGE_COALESCE_MS 500

/*
 * Ring, browser-history model.  `head` is one past the newest entry;
 * `len` is how many are live.  `cur` is a LOGICAL index into the
 * oldest-to-newest sequence, and `cur == len` means "standing at now,
 * not walking" — the state every walk starts and ends in.
 */
typedef struct JumpList {
    JumpEntry e[YEW_JUMPLIST_MAX];
    u32 head, len;
    u32 cur;
} JumpList;

typedef struct ChangeList {
    JumpEntry e[YEW_CHANGELIST_MAX];
    u32 head, len, cur;
    /* Coalescing needs the previous record's time and line, which are
     * not recoverable from the entry once its mark has moved. */
    i64 last_ms;
    LineNo last_line;
    bool has_last;
} ChangeList;

void yew_jumplist_init(JumpList *jl);
void yew_changelist_init(ChangeList *cl);

/*
 * Records `from` as a place worth coming back to, before a jump moves
 * away from it.  Takes the clock rather than reading one: every other
 * timestamped entry point in this program is handed now_ms by the loop,
 * which is what keeps replay deterministic (invariant 5) and what lets
 * the coalescing tests drive the boundaries exactly.
 */
void yew_jump_push(Win *w, ByteOff from, i64 now_ms);
bool yew_jump_back(Ed *ed, Win *w, u32 count);
bool yew_jump_fwd(Ed *ed, Win *w, u32 count);

/* Fed from the Sprint 9 op stream, not from a parallel notification
 * path: one op stream, N consumers. */
void yew_change_record(Buffer *b, ByteOff at, i64 now_ms);
bool yew_change_older(Ed *ed, Win *w, u32 count);
bool yew_change_newer(Ed *ed, Win *w, u32 count);

/* Entry count and logical (oldest-first) indexing, for ed.jump.list and
 * the tests. */
u32 yew_jumplist_len(const JumpList *jl);
const JumpEntry *yew_jumplist_at(const JumpList *jl, u32 index);

/*
 * Sprint 25's schema shape, landed now and called by nobody.  Marks are
 * resolved to line/col at serialize time because a mark handle means
 * nothing in another process.
 */
void yew_jumplist_serialize(const JumpList *jl, const Ed *ed, Bytebuf *out);
bool yew_jumplist_deserialize(JumpList *jl, const u8 *bytes, size_t len);

CmdStatus yew_jump_cmd_back(CmdCtx *cx);
CmdStatus yew_jump_cmd_fwd(CmdCtx *cx);
CmdStatus yew_jump_cmd_list(CmdCtx *cx);
CmdStatus yew_change_cmd_older(CmdCtx *cx);
CmdStatus yew_change_cmd_newer(CmdCtx *cx);

#endif
