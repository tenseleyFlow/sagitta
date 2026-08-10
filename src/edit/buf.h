#ifndef SAG_EDIT_BUF_H
#define SAG_EDIT_BUF_H

/* The editor's ordinary buffer model. */

#include "edit/jumplist.h"
#include "text/edit.h"
#include "util/strmap.h"

enum {
    SAG_BUF_SCRATCH = 1U << 0,
    SAG_BUF_NOUNDO = 1U << 1,
    SAG_BUF_READONLY = 1U << 2
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
    MarkSet *marks;
    ChangeList changes;
    MarkId named[26];
    bool named_set[26];
    u64 pending_marks[26];
    bool pending_mark_set[26];
    /* Nonzero only for an editable macro-register scratch buffer. */
    u8 macro_reg;
    Strmap opt_overrides;
} Buffer;

#endif /* SAG_EDIT_BUF_H */
