#ifndef YEW_EDIT_MULTICURSOR_H
#define YEW_EDIT_MULTICURSOR_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/motion.h"
#include "text/cursor.h"
#include "util/vec.h"

#define YEW_MC_MAX 10000U
#define YEW_MC_ACTIVE_NONE UINT32_MAX

VEC_DECL(YewCursorVec, Cursor);
VEC_DECL(YewCursorStampVec, u64);
VEC_DECL(YewSelStackVec, SelStack);

typedef struct CursorSet {
    YewCursorVec curs;
    YewCursorStampVec stamps;
    YewSelStackVec selstacks;
    u32 primary;
    u32 active;
    u64 next_stamp;
    i64 batch_delta;
    u32 batch_next;
    bool batching;
} CursorSet;

void yew_cset_init(CursorSet *cs, Cursor primary);
void yew_cset_free(CursorSet *cs);
bool yew_cset_add(CursorSet *cs, Cursor c);
bool yew_cset_add_many(CursorSet *cs, const Cursor *cursors, u32 count);
bool yew_cset_drop_latest(CursorSet *cs);
void yew_cset_remove_all_but_primary(CursorSet *cs);
void yew_cset_normalize(const TextBuf *tb, CursorSet *cs);
void yew_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len);
void yew_cset_reseed(CursorSet *cs);

/* Internal-invariant and feature-boundary checks. */
void yew_cset_check(const CursorSet *cs);
void yew_cset_check_text(const TextBuf *tb, const CursorSet *cs);
void yew_cset_require_single_edit(const CursorSet *cs);
void yew_mc_require_literal_lift(bool regex);
void yew_mc_require_single_completion(const CursorSet *cs);
void yew_mc_require_single_lsp_edit(const CursorSet *cs);

CmdStatus yew_mc_run(Win *w, CmdId cmd, CmdCtx *cx);

#endif
