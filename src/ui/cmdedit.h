#ifndef YEW_UI_CMDEDIT_H
#define YEW_UI_CMDEDIT_H

/*
 * Sprint 57.31 §2: A-e -- the prompt's line in a `*command-line*` buffer.
 *
 * A-e closes the prompt and opens its text (a bang line's BODY only) in
 * an unsaved scratch buffer in a new tab, in Insert mode.  When that
 * buffer goes away the prompt comes back with the buffer's text, made
 * one line.  `:q!` (and ed.buf.close!) discards: the prompt comes back
 * with the ORIGINAL text.
 *
 * THE RETURN HANGS OFF THE BUFFER'S RELEASE, not off a close command.
 * yew_ws_scratch_drop calls yew_cmdedit_released for every buffer it
 * frees; a buffer that is simply no longer shown anywhere (its tab
 * closed by a click, `:tabonly`, another file opened over it) is
 * released by yew_cmdedit_settle at the next event boundary.  The
 * prompt itself is reopened only at that boundary: a close is usually a
 * command typed into ANOTHER prompt, which is still open while it runs.
 *
 * NEVER LOSES TEXT.  `:q`, `:wq`, ed.buf.close and ed.tab.close refuse
 * up front when the text cannot become one line, naming the line, and
 * the buffer stays open.  A route that did not ask first (a click on the
 * tab's close box) gets the same answer after the fact: the text goes
 * back into a new `*command-line*` buffer with the same message.
 */

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;

enum {
    /* Past this, a buffer is not a command line: refused, not lexed
     * once per newline. */
    YEW_CMDEDIT_BYTES_MAX = 64 * 1024,
    YEW_CMDEDIT_LINES_MAX = 1024
};

typedef struct YewCmdEdit {
    u32 buf_id;          /* the open *command-line* buffer; 0: none      */
    u32 origin_tab_id;   /* the tab A-e was pressed in                   */
    u32 origin_buf_id;   /* what the focused window showed then          */
    u8 kind;             /* YewPromptKind to reopen                      */
    u8 return_mode;      /* the prompt's own return mode                 */
    bool discard;        /* the close under way discards (`:q!`)         */
    bool released;       /* the buffer is gone; `text` holds its bytes   */
    bool settling;
    char *prefix;        /* the bang prefix (`!`, `r !`, `%!`) or ""     */
    char *original;      /* the prompt's text when A-e was pressed       */
    size_t original_caret;
    char *text;          /* the buffer's bytes when it was released      */
    size_t text_len;
} YewCmdEdit;

/*
 * The one-line rule (§2.4).  `shell`: a bang body, where each newline is
 * decided by the lexer (yew_shctx_at) -- after a complete command it
 * becomes `; `; after an operator, a keyword that expects a command, or
 * an empty line, a blank; a `\` continuation vanishes with its newline,
 * the next line's indent folding to one blank.  A newline inside quotes,
 * a comment or a here-document is refused.  Otherwise any newline is
 * refused.  Trailing newlines are dropped either way.
 *
 * Appends to `out` and returns true, or returns false with `*line` the
 * 1-based line the refused newline ends (0 when no single line is to
 * blame) and `why` the rest of the message.
 */
bool yew_cmdedit_oneline(const char *s, size_t n, bool shell, Bytebuf *out,
                         u32 *line, const char **why);

/* Is `b` the open *command-line* buffer? */
bool yew_cmdedit_owns(const Ed *ed, const Buffer *b);
/*
 * Close the *command-line* buffer: its tab (or its views) and the buffer
 * itself.  `discard` returns the original line; otherwise the buffer's
 * text must make one line, or this refuses with the message and changes
 * nothing.  What the close commands call for that buffer.
 */
CmdStatus yew_cmdedit_close(Ed *ed, bool discard);
/* yew_ws_scratch_drop's hook: `b` is about to be freed. */
void yew_cmdedit_released(Ed *ed, Buffer *b);
/* The event boundary: release an orphaned buffer, reopen the prompt. */
void yew_cmdedit_settle(Ed *ed);
void yew_cmdedit_free(Ed *ed);

/* A-e / A-v, ed.cmdline.edit_in_buffer. */
CmdStatus yew_cmdedit_cmd_edit_in_buffer(CmdCtx *cx);

#endif
