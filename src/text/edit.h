#ifndef YEW_TEXT_EDIT_H
#define YEW_TEXT_EDIT_H

#include "edit/multicursor.h"
#include "text/file.h"
#include "text/journal.h"
#include "text/mark.h"
#include "text/undo.h"

typedef struct EditCtx {
    TextBuf *tb;
    MarkSet *marks;
    CursorSet *cset;
    u32 win_id;
    Journal *jrnl;
    UndoTree *undo;
    FileMeta *meta;
    /*
     * Sprint 21 §5: the changelist is another consumer of this one op
     * stream, not a parallel notification path.  A hook rather than a
     * ChangeList* because the list lives at the edit layer and this is
     * the text layer; undo replay writes through yew_textbuf_* directly
     * Undo replay fires it with `new_change == false`, so changelist/Fletch
     * observers do not mistake replay for a new user edit.  Syntax and the
     * other byte consumers follow replay through yew_edit_notify_pre/post.
     */
    void (*on_change)(void *ctx, ByteOff at, LineNo line,
                      u64 removed_lines, u64 inserted_lines,
                      i64 now_ms, bool new_change);
    void *on_change_ctx;
    i64 now_ms;
    /* Sprint 43: direct consumers of the fixed edit notification list. */
    struct Ed *ed;
    struct Buffer *buffer;
    LineNo notify_line;
    u64 notify_old_lines;
} EditCtx;

bool yew_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len);
bool yew_edit_delete(EditCtx *ec, Span range);
YewSaveErr yew_edit_save(EditCtx *ec, const char *path);
/* Internal: undo/navigation must establish durability before replay. */
bool yew_edit_ensure_journal(EditCtx *ec);

#endif
