#ifndef YEW_EDIT_BUF_H
#define YEW_EDIT_BUF_H

/* The editor's ordinary buffer model. */

#include "edit/jumplist.h"
#include "syn/engine.h"
#include "text/edit.h"
#include "util/strmap.h"

enum {
    YEW_BUF_SCRATCH = 1U << 0,
    YEW_BUF_NOUNDO = 1U << 1,
    YEW_BUF_READONLY = 1U << 2
};

typedef struct Buffer {
    struct Ed *owner;
    u32 id;
    TextBuf *tb;
    FileMeta meta;
    char *path;
    char *name;
    const char *lang;
    u32 flags;
    u32 tabwidth;
    UndoTree *undo;
    Journal *jrn;
    /* A deferred file is probed when hydration first reads it.  Keep the
     * result until that tab is focused so background hydration cannot put
     * a recovery question over the wrong document. */
    bool recovery_pending;
    MarkSet *marks;
    ChangeList changes;
    /* Owns its retained line state/cache.  Any engine/definition bound
     * through SynBuf is borrowed and must outlive this Buffer; dispose
     * detaches syntax before releasing the TextBuf. */
    SynBuf syn;
    MarkId named[26];
    bool named_set[26];
    u64 pending_marks[26];
    bool pending_mark_set[26];
    /* Nonzero only for an editable macro-register scratch buffer. */
    u8 macro_reg;
    Strmap opt_overrides;
    /* Optional-module allocation; the LSP client owns its lifetime. */
    struct DiagStore *diag;
} Buffer;

#endif /* YEW_EDIT_BUF_H */
