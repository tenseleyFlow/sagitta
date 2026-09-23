#ifndef YEW_EDIT_READLINE_CMDS_H
#define YEW_EDIT_READLINE_CMDS_H

#include "edit/cmd.h"

/*
 * The readline/Emacs editing keys Insert mode and the prompt bind.
 *
 * They act on whatever Win the dispatcher hands them: the document in
 * Insert mode, the `:` prompt's one-line Win in E mode.  They replaced the
 * prompt's own `ed.del.*` (Sprint 57.28), which reached `cs.primary`
 * alone and fed nothing a yank could read.  Multi-cursor is first class
 * in yew, so a kill reaches every live cursor and survives a macro round
 * trip.
 *
 * Every one of these is an AGGREGATE command (YEW_CMD_MULTI_AGGREGATE):
 * it plans one span per cursor in ascending order and replays the plan
 * through yew_sel_apply_edits, so the whole fan-out is one EditCtx, one
 * undo transaction, and one yank-stack entry.  Fanning out through
 * yew_mc_run instead would run the command once per cursor and push one
 * entry per cursor, leaving the last cursor's text as "what was killed".
 *
 * Word and grapheme boundaries come from yew_unit_word and the grapheme
 * layer, so there is exactly one definition of a word in the editor and
 * CJK, emoji and combining marks are already right here.
 *
 * THE YANK STACK (Sprint 57.28).  The kills push onto Ed.yank, readline's
 * kill ring, and C-y / A-y read it -- NEVER a register: a kill leaves the
 * unnamed register, the register ring and the system clipboard exactly as
 * they were, and `p` never pastes a kill (text/yankstack.h).  Consecutive
 * kills in one Win join the way readline's do, decided by the
 * dispatcher's sequence number rather than a flag other commands clear.
 * The same commands run on the `:` prompt's Win: the prompt reuses them
 * rather than keeping twins.
 */

CmdStatus yew_rl_cmd_kill_word_prev(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_word_next(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_to_home(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_to_end(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_ws_word_prev(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_yank(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_yank_pop(CmdCtx *cx);
CmdStatus yew_rl_cmd_transpose_chars(CmdCtx *cx);
CmdStatus yew_rl_cmd_transpose_words(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_upper_word(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_lower_word(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_cap_word(CmdCtx *cx);

#endif
