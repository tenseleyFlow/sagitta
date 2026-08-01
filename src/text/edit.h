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
} EditCtx;

void sag_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len);
void sag_edit_delete(EditCtx *ec, Span range);
SagSaveErr sag_edit_save(EditCtx *ec, const char *path);
/* Internal: undo/navigation must establish durability before replay. */
void sag_edit_ensure_journal(EditCtx *ec);

#endif
