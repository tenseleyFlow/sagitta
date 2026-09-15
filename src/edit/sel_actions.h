#ifndef YEW_EDIT_SEL_ACTIONS_H
#define YEW_EDIT_SEL_ACTIONS_H

#include "edit/cmd.h"
#include "text/piece.h"
#include "util/buf.h"
#include "util/vec.h"

/*
 * The aggregate multi-cursor edit plan.
 *
 * One entry per span the command wants replaced, in ASCENDING,
 * non-overlapping offset order; `replacement` empty means a pure
 * delete.  yew_sel_apply_edits replays the plan against a single
 * EditCtx with a running signed delta, so every span is stated in
 * the offsets the command saw BEFORE any of them were applied.
 *
 * Exported rather than copied: the single-exit journal discipline in
 * yew_sel_apply_edits is a correctness rule (see its comment), and a
 * second implementation of it is a second place that has to keep
 * obeying it.
 */
typedef struct SelEdit {
    Span span;
    Bytebuf replacement;
} SelEdit;

VEC_DECL(SelEditVec, SelEdit);

SelEdit *yew_sel_edit_push(SelEditVec *edits, Span span);
void yew_sel_edits_free(SelEditVec *edits);
bool yew_sel_apply_edits(CmdCtx *cx, SelEditVec *edits, ByteOff *first);
Bytebuf yew_sel_copy_span(const TextBuf *tb, Span span);

CmdStatus yew_sel_cmd_yank(CmdCtx *cx);
CmdStatus yew_sel_cmd_clip_copy(CmdCtx *cx);
CmdStatus yew_sel_cmd_clip_cut(CmdCtx *cx);
CmdStatus yew_sel_cmd_clip_paste(CmdCtx *cx);
CmdStatus yew_sel_cmd_delete(CmdCtx *cx);
CmdStatus yew_sel_cmd_all(CmdCtx *cx);
CmdStatus yew_sel_cmd_change(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_upper(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_lower(CmdCtx *cx);
CmdStatus yew_sel_cmd_case_toggle(CmdCtx *cx);
CmdStatus yew_sel_cmd_indent(CmdCtx *cx);
CmdStatus yew_sel_cmd_dedent(CmdCtx *cx);
CmdStatus yew_sel_cmd_shift_left(CmdCtx *cx);
CmdStatus yew_sel_cmd_shift_right(CmdCtx *cx);
CmdStatus yew_sel_cmd_join(CmdCtx *cx);
CmdStatus yew_sel_cmd_replace_char(CmdCtx *cx);
CmdStatus yew_sel_cmd_rect_insert(CmdCtx *cx);
CmdStatus yew_sel_cmd_rect_append(CmdCtx *cx);

#endif
