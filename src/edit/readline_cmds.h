#ifndef YEW_EDIT_READLINE_CMDS_H
#define YEW_EDIT_READLINE_CMDS_H

#include "edit/cmd.h"

/*
 * The readline/Emacs editing keys Insert mode binds.
 *
 * These are DOCUMENT commands, not the command line's.  `ed.del.*` in
 * ui/cmdline.c already deletes a word or a line fragment, but it acts on
 * `cs.primary` alone and is INTERNAL rather than RECORDABLE.  Multi-cursor
 * is first class in yew, so an Insert-mode kill has to reach every live
 * cursor and has to survive a macro round trip; that is a different
 * command, not a different caller of the same one.  The command line keeps
 * `ed.del.*` and its E-mode bindings unchanged.
 *
 * Every one of these is an AGGREGATE command (YEW_CMD_MULTI_AGGREGATE):
 * it plans one span per cursor in ascending order and replays the plan
 * through yew_sel_apply_edits, so the whole fan-out is one EditCtx, one
 * undo transaction, and one register value.  Fanning out through
 * yew_mc_run instead would run the command once per cursor and each run
 * would overwrite the kill register, leaving the last cursor's text as
 * "what was killed".
 *
 * Word and grapheme boundaries come from yew_unit_word and the grapheme
 * layer, so there is exactly one definition of a word in the editor and
 * CJK, emoji and combining marks are already right here.
 *
 * THE KILL RING.  yew has one: yew_reg_delete writes the unnamed register
 * through set_unnamed, which pushes onto Registers.ring (see
 * text/register.c).  These kills therefore feed the same ring that
 * `ed.sel.delete` and `d d` do, and `ed.edit.kill.yank` pastes the unnamed
 * register, which is the most recent kill.  What yew does NOT do is
 * readline's CONSECUTIVE-kill accumulation: two C-w presses in a row leave
 * two separate ring entries rather than one entry holding both words, so a
 * following C-y restores only the second word.  That is deliberate --
 * "successive kills append" needs a last-command-was-a-kill flag that
 * every other command has to clear, and yew's ring already gives the user
 * the earlier kill as its own entry.
 */

CmdStatus yew_rl_cmd_kill_word_prev(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_word_next(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_to_home(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_to_end(CmdCtx *cx);
CmdStatus yew_rl_cmd_kill_yank(CmdCtx *cx);
CmdStatus yew_rl_cmd_transpose_chars(CmdCtx *cx);
CmdStatus yew_rl_cmd_transpose_words(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_upper_word(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_lower_word(CmdCtx *cx);
CmdStatus yew_rl_cmd_case_cap_word(CmdCtx *cx);

#endif
