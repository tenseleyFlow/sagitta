#ifndef SAG_EDIT_MULTICURSOR_H
#define SAG_EDIT_MULTICURSOR_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/motion.h"
#include "text/cursor.h"
#include "util/vec.h"

#define SAG_MC_MAX 10000U
#define SAG_MC_ACTIVE_NONE UINT32_MAX

VEC_DECL(SagCursorVec, Cursor);
VEC_DECL(SagCursorStampVec, u64);
VEC_DECL(SagSelStackVec, SelStack);

typedef struct CursorSet {
    SagCursorVec curs;
    SagCursorStampVec stamps;
    SagSelStackVec selstacks;
    u32 primary;
    u32 active;
    u64 next_stamp;
    i64 batch_delta;
    u32 batch_next;
    bool batching;
} CursorSet;

void sag_cset_init(CursorSet *cs, Cursor primary);
void sag_cset_free(CursorSet *cs);
bool sag_cset_add(CursorSet *cs, Cursor c);
bool sag_cset_add_many(CursorSet *cs, const Cursor *cursors, u32 count);
bool sag_cset_drop_latest(CursorSet *cs);
void sag_cset_remove_all_but_primary(CursorSet *cs);
void sag_cset_normalize(const TextBuf *tb, CursorSet *cs);
void sag_cset_adjust(CursorSet *cs, u8 op, ByteOff at, u64 len);
void sag_cset_reseed(CursorSet *cs);

/* Internal-invariant and feature-boundary checks. */
void sag_cset_check(const CursorSet *cs);
void sag_cset_check_text(const TextBuf *tb, const CursorSet *cs);
void sag_cset_require_single_edit(const CursorSet *cs);
void sag_mc_require_literal_lift(bool regex);
void sag_mc_require_single_completion(const CursorSet *cs);
void sag_mc_require_single_lsp_edit(const CursorSet *cs);

CmdStatus sag_mc_run(Win *w, CmdId cmd, CmdCtx *cx);

#endif
