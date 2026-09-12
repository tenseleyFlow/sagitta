#ifndef YEW_UI_CMDPARSE_H
#define YEW_UI_CMDPARSE_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "util/arena.h"

typedef struct TextBuf TextBuf;

typedef struct CmdErr {
    u32 tok_lo;
    u32 tok_hi;
    char msg[128];
} CmdErr;

typedef struct CmdParse {
    CmdRange range;
    Span name_tok;
    CmdArgv argv;
    Span *arg_tok;
    CmdId command;
    bool bang;
    CmdErr err;
} CmdParse;

/* Completion uses the same lexical rules as execution.  `stem` is decoded
 * token text, never a borrowed slice of the prompt. */
typedef struct CmdParsePoint {
    Span token;
    char *stem;
    u32 token_index;
    CmdId command;
    bool command_known;
    /*
     * Sprint 57.17 §1: where the command NAME sits and what it decoded
     * to, so a caller whose caret is on an ARGUMENT can still ask about
     * the name.  `stem`/`token` describe the token under the cursor,
     * which stops being the name the moment arguments are typed --
     * `:fwq somefile` puts the caret on `somefile`, and §1 still has to
     * resolve `fwq`.  Both come from the same loose_name scan, so there
     * is no second lexical rule to drift.
     */
    Span name_tok;
    char *name;
    /* What the leading range resolved to, for Sprint 18.5 §9's hint.
     * `given` is false when the user typed none. */
    CmdRange range;
} CmdParsePoint;

bool yew_cmd_parse(Ed *ed, const char *line, size_t len, Arena *a,
                   CmdParse *out);
bool yew_cmd_parse_point(Ed *ed, const char *line, size_t len,
                         size_t cursor, Arena *a, CmdParsePoint *out);

/* Converts an already validated inclusive line range to bytes. */
Span yew_range_span(const TextBuf *tb, const CmdRange *range);

#endif
