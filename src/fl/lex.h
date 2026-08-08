#ifndef SAG_FL_LEX_H
#define SAG_FL_LEX_H

/*
 * Sprint 29: the Fletch lexer.  Implements spec §1 in full, including
 * §1.6's motion token space.
 *
 * The lexer NEVER prints and NEVER exits.  A malformed token is reported
 * through DiagCtx, returned as FL_T_ERROR carrying an already-emitted
 * diagnostic, and scanning continues at the next codepoint -- so one bad
 * byte in a file does not cost the reader every later message in it.
 */

#include <stdbool.h>
#include <stddef.h>

#include "fl/diag.h"
#include "util/base.h"
#include "util/intern.h"

typedef enum FlTokKind {
    /* literals / names */
    FL_T_INT, FL_T_FLOAT, FL_T_STRING, FL_T_IDENT,
    /* punctuation */
    FL_T_LPAREN, FL_T_RPAREN, FL_T_LBRACKET, FL_T_RBRACKET,
    FL_T_LBRACE, FL_T_RBRACE, FL_T_COMMA, FL_T_COLON,
    FL_T_SEMI, FL_T_DOT,
    FL_T_ATBRACKET,               /* "@[" -- one token, opens motion mode */
    /* operators */
    FL_T_PLUS, FL_T_MINUS, FL_T_STAR, FL_T_SLASH, FL_T_PERCENT,
    FL_T_EQ, FL_T_EQEQ, FL_T_BANGEQ, FL_T_LT, FL_T_LE, FL_T_GT, FL_T_GE,
    /*
     * The 22 reserved words of spec §15.1, alphabetical.  The order is
     * load-bearing: keyword_at() binary-searches a table sorted the same
     * way, and FL_T_KW_FIRST/LAST bracket the range so "is this token a
     * keyword" stays one comparison rather than a growing switch.
     */
    FL_T_AND, FL_T_AS, FL_T_BREAK, FL_T_CATCH, FL_T_CONTINUE, FL_T_EDIT,
    FL_T_ELSE, FL_T_FALSE, FL_T_FN, FL_T_FOR, FL_T_IF, FL_T_IMPORT,
    FL_T_IN, FL_T_LET, FL_T_MACRO, FL_T_NIL, FL_T_NOT, FL_T_OR,
    FL_T_RETURN, FL_T_TRUE, FL_T_TRY, FL_T_WHILE,
    /* layout */
    FL_T_NEWLINE, FL_T_EOF,
    /*
     * Motion token space (spec §3), produced only inside `@[ ... ]`.
     * `l`/`v`/`del` are motion words there and ordinary identifiers
     * everywhere else, which is exactly why this is a lexer mode.
     */
    FL_M_COUNT,                   /* v.count */
    FL_M_UNIT,                    /* v.m.ch in {l,w,b,c} */
    FL_M_ARROW,                   /* v.m.ch in {<,>,^,v}, v.m.alt */
    FL_M_H, FL_M_LPAREN, FL_M_RPAREN,
    FL_M_INSERT,                  /* i"..." -- v.str_id */
    FL_M_DEL, FL_M_ESC,
    FL_M_WORD,                    /* CMDWORD -- v.str_id */
    FL_M_END,                     /* "]" closing the block */

    FL_T_ERROR,
    FL_T_KIND__N
} FlTokKind;

/* Macros, not an anonymous enum: -Werror=enum-compare rejects comparing
 * an FlTokKind against a differently-typed enumerator. */
#define FL_T_KW_FIRST FL_T_AND
#define FL_T_KW_LAST  FL_T_WHILE

/* Spec §15.1 pins the count, and Sprint 33 asserts it. */
enum { FL_KEYWORD_COUNT = FL_T_KW_LAST - FL_T_KW_FIRST + 1 };

/* Spec §3: a count prefix is a u32 and 0 is meaningless as a repeat. */
enum { FL_MOTION_COUNT_MAX = 65535 };

typedef struct FlTok {
    FlTokKind kind;
    FlSpan sp;
    union {
        i64 i;
        double f;
        u32 str_id;                      /* interned STRING / IDENT / WORD */
        struct { u8 ch; bool alt; } m;   /* UNIT / ARROW payload */
        u32 count;
    } v;
} FlTok;

typedef struct FlLexer {
    const char *src;
    size_t len;
    size_t at;
    u32 line;
    u32 col;
    u32 file_id;
    Arena *arena;
    DiagCtx *dc;
    Interner *in;
    /*
     * Motion nesting DEPTH, not a boolean.
     *
     * `@[` opens the mode and the matching `]` closes it, but `H( ... )`
     * nests inside without leaving it -- so a flag would have `H(2>)]`
     * drop back to ordinary tokens at the inner `)`.  0 means ordinary.
     */
    u32 motion_depth;
    bool utf8_reported;              /* one invalid-UTF-8 error per run */
} FlLexer;

void fl_lex_init(FlLexer *lx, Arena *a, DiagCtx *dc, Interner *in,
                 const char *src, size_t len, u32 file_id);
FlTok fl_lex_next(FlLexer *lx);

/* The token's source spelling, for `expected X, found Y` wording.
 * Keywords and operators render as themselves; literals render as a
 * CATEGORY (`integer`, `string`), because quoting the value would make
 * the message vary with the input it is describing. */
const char *fl_tok_spelling(FlTokKind kind);

/* True while `kind` is one of the 22 reserved words. */
bool fl_tok_is_keyword(FlTokKind kind);

#endif /* SAG_FL_LEX_H */
