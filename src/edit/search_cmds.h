#ifndef SAG_EDIT_SEARCH_CMDS_H
#define SAG_EDIT_SEARCH_CMDS_H

#include "edit/cmd.h"
#include "search/replace.h"

typedef struct Ed Ed;

CmdStatus sag_search_cmd_replace(CmdCtx *cx);
CmdStatus sag_search_cmd_global(CmdCtx *cx);
CmdStatus sag_search_cmd_open(CmdCtx *cx);
CmdStatus sag_search_cmd_open_back(CmdCtx *cx);
CmdStatus sag_search_cmd_next(CmdCtx *cx);
CmdStatus sag_search_cmd_prev(CmdCtx *cx);
CmdStatus sag_search_cmd_word_next(CmdCtx *cx);
CmdStatus sag_search_cmd_word_prev(CmdCtx *cx);
CmdStatus sag_search_cmd_clear_highlight(CmdCtx *cx);
CmdStatus sag_mark_cmd_set(CmdCtx *cx);
CmdStatus sag_mark_cmd_jump(CmdCtx *cx);

/*
 * Confirm-mode state.  The plan is MOVED here from the command, so the
 * compile arena can be released immediately: once matches are planned
 * the regex is no longer needed, only the spans and the already-expanded
 * replacement bytes.
 */
typedef struct SearchConfirm {
    bool active;
    SagReplPlan plan;
    SagReplConfirm walk;
    Bytebuf shown; /* the replacement text, for the prompt */
} SearchConfirm;

void sag_search_confirm_start(Ed *ed, SagReplPlan *plan, const char *rep,
                              size_t replen);
/* Feeds one key to a running confirm run.  Returns true if the key was
 * consumed, so the dispatcher knows not to treat it as a command. */
bool sag_search_confirm_key(Ed *ed, u8 key);
void sag_search_confirm_cancel(Ed *ed);
/* Re-issues the confirm question.  The command line clears the message
 * line when it closes, which happens AFTER the `:s` that started the
 * run — so the question has to be restated once the prompt is gone. */
void sag_search_confirm_reprompt(Ed *ed);

#endif
