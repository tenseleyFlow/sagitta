#ifndef SAG_TEXT_EDIT_H
#define SAG_TEXT_EDIT_H

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
     * the text layer; undo replay writes through sag_textbuf_* directly
     * and so never fires it, which is exactly right — replaying an undo
     * is not a new change.
     */
    void (*on_change)(void *ctx, ByteOff at, i64 now_ms);
    void *on_change_ctx;
    i64 now_ms;
} EditCtx;

bool sag_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len);
bool sag_edit_delete(EditCtx *ec, Span range);
SagSaveErr sag_edit_save(EditCtx *ec, const char *path);
/* Internal: undo/navigation must establish durability before replay. */
bool sag_edit_ensure_journal(EditCtx *ec);

#endif
