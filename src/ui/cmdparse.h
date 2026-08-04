#ifndef SAG_UI_CMDPARSE_H
#define SAG_UI_CMDPARSE_H

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
} CmdParsePoint;

bool sag_cmd_parse(Ed *ed, const char *line, size_t len, Arena *a,
                   CmdParse *out);
bool sag_cmd_parse_point(Ed *ed, const char *line, size_t len,
                         size_t cursor, Arena *a, CmdParsePoint *out);

/* Converts an already validated inclusive line range to bytes. */
Span sag_range_span(const TextBuf *tb, const CmdRange *range);

#endif
