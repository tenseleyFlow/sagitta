/* Sprint 29: the Fletch lexer.  Spec §1 in full, §1.6's motion space
 * included.  Every rule below cites the section it implements. */
#include "fl/lex.h"

#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "util/buf.h"
#include "util/log.h"

/* ---------------------------------------------------------------- */
/* Keywords (spec §15.1)                                            */
/* ---------------------------------------------------------------- */

/*
 * Sorted, and asserted to be: keyword_at binary-searches it, and a table
 * that drifted out of order would silently stop finding the words past
 * the break rather than fail to build.
 */
static const char *const fl_keywords[] = {
    "and", "as", "break", "catch", "continue", "edit",
    "else", "false", "fn", "for", "if", "import",
    "in", "let", "macro", "nil", "not", "or",
    "return", "true", "try", "while"
};

_Static_assert(sizeof(fl_keywords) / sizeof(fl_keywords[0]) ==
                   (size_t)FL_KEYWORD_COUNT,
               "spec 15.1 pins 22 reserved words");

/* Returns the keyword's token kind, or FL_T_IDENT when `s` is not one.
 * The caller has already established that `s`/`n` is a complete
 * identifier, which is what keeps `letx` an IDENT. */
static FlTokKind keyword_at(const char *s, size_t n)
{
    u32 lo = 0U;
    u32 hi = (u32)FL_KEYWORD_COUNT;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        const char *k = fl_keywords[mid];
        size_t klen = strlen(k);
        size_t cmp_len = klen < n ? klen : n;
        int c = memcmp(k, s, cmp_len);

        if (c == 0 && klen != n)
            c = klen < n ? -1 : 1;
        if (c == 0)
            return (FlTokKind)(FL_T_KW_FIRST + (int)mid);
        if (c < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return FL_T_IDENT;
}

bool fl_tok_is_keyword(FlTokKind kind)
{
    return kind >= FL_T_KW_FIRST && kind <= FL_T_KW_LAST;
}

bool fl_kw_reserved(const char *s, size_t n)
{
    return keyword_at(s, n) != FL_T_IDENT;
}

const char *fl_tok_spelling(FlTokKind kind)
{
    if (fl_tok_is_keyword(kind))
        return fl_keywords[(int)kind - FL_T_KW_FIRST];
    switch (kind) {
    /* Literals name their CATEGORY.  Quoting the value instead would
     * make `expected ')', found 42` vary with input that has nothing to
     * do with the mistake, and goldens would pin the wrong thing. */
    case FL_T_INT:      return "integer";
    case FL_T_FLOAT:    return "float";
    case FL_T_STRING:   return "string";
    case FL_T_IDENT:    return "identifier";
    case FL_T_LPAREN:   return "(";
    case FL_T_RPAREN:   return ")";
    case FL_T_LBRACKET: return "[";
    case FL_T_RBRACKET: return "]";
    case FL_T_LBRACE:   return "{";
    case FL_T_RBRACE:   return "}";
    case FL_T_COMMA:    return ",";
    case FL_T_COLON:    return ":";
    case FL_T_SEMI:     return ";";
    case FL_T_DOT:      return ".";
    case FL_T_ATBRACKET: return "@[";
    case FL_T_PLUS:     return "+";
    case FL_T_MINUS:    return "-";
    case FL_T_STAR:     return "*";
    case FL_T_SLASH:    return "/";
    case FL_T_PERCENT:  return "%";
    case FL_T_EQ:       return "=";
    case FL_T_EQEQ:     return "==";
    case FL_T_BANGEQ:   return "!=";
    case FL_T_LT:       return "<";
    case FL_T_LE:       return "<=";
    case FL_T_GT:       return ">";
    case FL_T_GE:       return ">=";
    case FL_T_NEWLINE:  return "newline";
    case FL_T_EOF:      return "end of file";
    case FL_M_COUNT:    return "count";
    case FL_M_UNIT:     return "motion unit";
    case FL_M_ARROW:    return "motion arrow";
    case FL_M_H:        return "H";
    case FL_M_LPAREN:   return "(";
    case FL_M_RPAREN:   return ")";
    case FL_M_INSERT:   return "insert";
    case FL_M_DEL:      return "del";
    case FL_M_ESC:      return "esc";
    case FL_M_WORD:     return "motion word";
    case FL_M_END:      return "]";
    case FL_T_COMMENT:  return "comment";
    default:            return "invalid token";
    }
}

/* ---------------------------------------------------------------- */
/* Cursor                                                           */
/* ---------------------------------------------------------------- */

void fl_lex_init(FlLexer *lx, Arena *a, DiagCtx *dc, Interner *in,
                 const char *src, size_t len, u32 file_id)
{
    (void)memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = src == NULL ? 0U : len;
    lx->line = 1U;
    lx->col = 1U;
    lx->file_id = file_id;
    lx->arena = a;
    lx->dc = dc;
    lx->in = in;
}

static bool at_end(const FlLexer *lx) { return lx->at >= lx->len; }

static u8 peek(const FlLexer *lx)
{
    return lx->at < lx->len ? (u8)lx->src[lx->at] : 0U;
}

static u8 peek2(const FlLexer *lx)
{
    return lx->at + 1U < lx->len ? (u8)lx->src[lx->at + 1U] : 0U;
}

/* Advances one BYTE.  Callers that consume a multibyte codepoint call
 * this once per byte, so `col` stays a byte index (see FlSpan). */
static u8 bump(FlLexer *lx)
{
    u8 c = (u8)lx->src[lx->at++];

    if (c == '\n') {
        lx->line++;
        lx->col = 1U;
    } else {
        lx->col++;
    }
    return c;
}

static FlSpan span_at(const FlLexer *lx, u32 line, u32 col, size_t from)
{
    FlSpan sp;

    sp.file_id = lx->file_id;
    sp.line = line;
    sp.col = col;
    sp.len = (u32)(lx->at - from);
    return sp;
}

static FlTok make(FlTokKind kind, FlSpan sp)
{
    FlTok t;

    (void)memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.sp = sp;
    return t;
}

/* Reports and returns the error token.  The diagnostic is emitted HERE,
 * so a caller seeing FL_T_ERROR must not emit its own -- that is what
 * would turn one bad byte into two messages. */
static FlTok fail(FlLexer *lx, FlSpan sp, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fl_diag_vemit(lx->dc, FL_DIAG_ERROR, sp, fmt, ap);
    va_end(ap);
    return make(FL_T_ERROR, sp);
}

static bool is_digit(u8 c) { return c >= '0' && c <= '9'; }
static bool is_hex(u8 c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_alpha(u8 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_cont(u8 c) { return is_alpha(c) || is_digit(c); }

static u32 hex_val(u8 c)
{
    if (is_digit(c))
        return (u32)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (u32)(c - 'a') + 10U;
    return (u32)(c - 'A') + 10U;
}

/* ---------------------------------------------------------------- */
/* Numbers (spec §1.4)                                              */
/* ---------------------------------------------------------------- */

/*
 * `_` separates digits and may not lead, trail or double.
 *
 * Checked while scanning rather than by a pass over the finished text so
 * the caret lands on the offending underscore itself.
 */
static bool scan_digits(FlLexer *lx, bool hex, u64 *acc, bool *overflow,
                        bool *any, FlSpan *bad, const char **why)
{
    bool prev_digit = false;
    FlSpan last_us = {0};

    *any = false;
    for (;;) {
        u8 c = peek(lx);

        if (c == '_') {
            u32 line = lx->line;
            u32 col = lx->col;
            size_t from = lx->at;

            (void)bump(lx);
            last_us = span_at(lx, line, col, from);
            if (!prev_digit) {
                *bad = last_us;
                *why = "'_' must separate digits";
                return false;
            }
            prev_digit = false;
            continue;
        }
        if (hex ? !is_hex(c) : !is_digit(c))
            break;
        {
            u64 digit = hex ? hex_val(c) : (u64)(c - '0');
            u64 base = hex ? 16U : 10U;

            /* i64 is the type (§1.4); a literal that does not fit is a
             * compile error and never a silent wrap. */
            if (*acc > ((u64)INT64_MAX - digit) / base)
                *overflow = true;
            else
                *acc = *acc * base + digit;
        }
        (void)bump(lx);
        prev_digit = true;
        *any = true;
    }
    if (!prev_digit && *any) {
        /* Trailing `_`.  The caret goes on the underscore itself, which
         * is why its span was kept rather than recomputed from here. */
        *bad = last_us;
        *why = "'_' must separate digits";
        return false;
    }
    return true;
}

static FlTok lex_number(FlLexer *lx)
{
    u32 line = lx->line;
    u32 col = lx->col;
    size_t from = lx->at;
    u64 acc = 0U;
    bool overflow = false;
    bool any = false;
    FlSpan bad = {0};
    const char *why = NULL;
    bool hex = false;

    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        hex = true;
        (void)bump(lx);
        (void)bump(lx);
    }
    if (!scan_digits(lx, hex, &acc, &overflow, &any, &bad, &why))
        return fail(lx, bad, "%s", why);
    if (hex && !any)
        return fail(lx, span_at(lx, line, col, from),
                    "'0x' needs at least one hexadecimal digit");
    /*
     * Only consume `.` when a DIGIT follows.
     *
     * Spec §1.4 has no leading-dot float and no trailing-dot float, so
     * `1.foo` is INT DOT IDENT -- a field access on an integer, which
     * the parser will reject on its own terms -- and `1.` is INT DOT.
     * Consuming the dot eagerly would turn every `x.field` on a numeric
     * literal into a malformed float.
     */
    if (!hex && peek(lx) == '.' && is_digit(peek2(lx))) {
        u64 ignored = 0U;
        bool f_over = false;
        bool f_any = false;

        (void)bump(lx);
        if (!scan_digits(lx, false, &ignored, &f_over, &f_any, &bad, &why))
            return fail(lx, bad, "%s", why);
        if (peek(lx) == 'e' || peek(lx) == 'E') {
            (void)bump(lx);
            if (peek(lx) == '+' || peek(lx) == '-')
                (void)bump(lx);
            ignored = 0U;
            if (!scan_digits(lx, false, &ignored, &f_over, &f_any, &bad,
                             &why))
                return fail(lx, bad, "%s", why);
            if (!f_any)
                return fail(lx, span_at(lx, line, col, from),
                            "exponent needs at least one digit");
        }
        {
            /* strtod over the token's own bytes: the digits were only
             * accumulated to detect integer overflow, and re-deriving
             * the value here avoids a second rounding rule. */
            char text[64];
            size_t n = lx->at - from;
            FlTok t;

            if (n >= sizeof(text))
                return fail(lx, span_at(lx, line, col, from),
                            "float literal is too long");
            (void)memcpy(text, lx->src + from, n);
            text[n] = '\0';
            t = make(FL_T_FLOAT, span_at(lx, line, col, from));
            t.v.f = strtod(text, NULL);
            return t;
        }
    }
    if (overflow)
        return fail(lx, span_at(lx, line, col, from),
                    "integer literal does not fit in i64");
    {
        FlTok t = make(FL_T_INT, span_at(lx, line, col, from));

        t.v.i = (i64)acc;
        return t;
    }
}

/* ---------------------------------------------------------------- */
/* Strings (spec §1.5)                                              */
/* ---------------------------------------------------------------- */

/* Appends `cp` as UTF-8.  Shared by \x and \u so the two cannot encode
 * differently. */
static void push_cp(Bytebuf *b, u32 cp)
{
    u8 enc[SAG_UTF8_MAX];
    size_t n = sag_utf8_encode(cp, enc);

    bytebuf_append(b, enc, n);
}

static FlTok lex_string(FlLexer *lx)
{
    u32 line = lx->line;
    u32 col = lx->col;
    size_t from = lx->at;
    Bytebuf out;
    FlTok t;

    bytebuf_init(&out);
    (void)bump(lx); /* opening quote */
    for (;;) {
        u32 eline;
        u32 ecol;
        size_t efrom;
        u8 c;

        if (at_end(lx)) {
            bytebuf_free(&out);
            return fail(lx, span_at(lx, line, col, from),
                        "unterminated string literal");
        }
        c = peek(lx);
        if (c == '"') {
            (void)bump(lx);
            break;
        }
        if (c == '\n') {
            /* §1.5: a newline inside a string is an error; use \n.  The
             * newline is left unconsumed so the statement it terminates
             * still terminates. */
            bytebuf_free(&out);
            return fail(lx, span_at(lx, line, col, from),
                        "newline in string literal; use '\\n'");
        }
        if (c != '\\') {
            bytebuf_push_u8(&out, bump(lx));
            continue;
        }
        eline = lx->line;
        ecol = lx->col;
        efrom = lx->at;
        (void)bump(lx); /* backslash */
        c = peek(lx);
        switch (c) {
        case 'n': (void)bump(lx); bytebuf_push_u8(&out, (u8)'\n'); break;
        case 't': (void)bump(lx); bytebuf_push_u8(&out, (u8)'\t'); break;
        case 'r': (void)bump(lx); bytebuf_push_u8(&out, (u8)'\r'); break;
        case '\\': (void)bump(lx); bytebuf_push_u8(&out, (u8)'\\'); break;
        case '"': (void)bump(lx); bytebuf_push_u8(&out, (u8)'"'); break;
        case '0': (void)bump(lx); bytebuf_push_u8(&out, 0U); break;
        case 'x': {
            u32 v = 0U;
            int i;

            (void)bump(lx);
            for (i = 0; i < 2; i++) {
                if (!is_hex(peek(lx))) {
                    bytebuf_free(&out);
                    return fail(lx, span_at(lx, eline, ecol, efrom),
                                "'\\x' needs two hexadecimal digits");
                }
                v = v * 16U + hex_val(bump(lx));
            }
            /* \xNN is a BYTE, not a codepoint: it is how a caller writes
             * a byte that is not valid UTF-8 on its own. */
            bytebuf_push_u8(&out, (u8)v);
            break;
        }
        case 'u': {
            u32 v = 0U;
            u32 ndigits = 0U;

            (void)bump(lx);
            if (peek(lx) != '{') {
                bytebuf_free(&out);
                return fail(lx, span_at(lx, eline, ecol, efrom),
                            "'\\u' needs a braced codepoint, as '\\u{1F600}'");
            }
            (void)bump(lx);
            while (is_hex(peek(lx)) && ndigits < 8U) {
                v = v * 16U + hex_val(bump(lx));
                ndigits++;
            }
            if (ndigits == 0U || peek(lx) != '}') {
                bytebuf_free(&out);
                return fail(lx, span_at(lx, eline, ecol, efrom),
                            "'\\u{...}' needs 1 to 6 hexadecimal digits");
            }
            (void)bump(lx);
            /* Surrogates are not scalar values; encoding one would put
             * bytes in the string that no decoder will accept back. */
            if (v > 0x10FFFFU || (v >= 0xD800U && v <= 0xDFFFU)) {
                bytebuf_free(&out);
                return fail(lx, span_at(lx, eline, ecol, efrom),
                            "'\\u{...}' is not a Unicode scalar value");
            }
            push_cp(&out, v);
            break;
        }
        default:
            /*
             * §1.5: an unknown escape is an error, never passed through,
             * and the caret goes ON THE ESCAPE rather than on the string
             * -- a 200-character line with one bad escape is unreadable
             * otherwise.
             */
            if (!at_end(lx))
                (void)bump(lx);
            bytebuf_free(&out);
            return fail(lx, span_at(lx, eline, ecol, efrom),
                        "unknown escape in string literal");
        }
    }
    t = make(FL_T_STRING, span_at(lx, line, col, from));
    t.v.str_id = sag_intern(lx->in, (const char *)out.data, out.len);
    bytebuf_free(&out);
    return t;
}

/* ---------------------------------------------------------------- */
/* Motion token space (spec §1.6, §3)                               */
/* ---------------------------------------------------------------- */

/* The Unicode arrow aliases of §3.  Accepted on input; the recorder
 * only ever emits the ASCII forms. */
static bool arrow_alias(u32 cp, u8 *ch)
{
    switch (cp) {
    case 0x2192U: *ch = (u8)'>'; return true;  /* -> */
    case 0x2190U: *ch = (u8)'<'; return true;  /* <- */
    case 0x2191U: *ch = (u8)'^'; return true;  /* up */
    case 0x2193U: *ch = (u8)'v'; return true;  /* down */
    default: return false;
    }
}

static bool is_unit_ch(u8 c)
{
    return c == 'l' || c == 'w' || c == 'b' || c == 'c';
}

static bool is_arrow_ch(u8 c)
{
    return c == '<' || c == '>' || c == '^' || c == 'v';
}

/*
 * One motion word.
 *
 * LONGEST MATCH WITH A TERMINATOR CHECK is the whole trick here: `l` is
 * a unit but `list` is a CMDWORD, `av` is alt-down but `avx` is the word
 * `avx`, `del` is a keyword but `delete_line` is a word.  Every
 * single-letter form therefore has to confirm that what follows cannot
 * continue an identifier before it may claim the byte.
 */
static FlTok lex_motion_word(FlLexer *lx)
{
    u32 line = lx->line;
    u32 col = lx->col;
    size_t from = lx->at;
    u8 c = peek(lx);

    /* An identifier-shaped run: decide unit/arrow/keyword/word by its
     * whole extent, never by its first byte. */
    if (is_alpha(c)) {
        size_t start = lx->at;
        size_t n;

        while (!at_end(lx) && is_ident_cont(peek(lx)))
            (void)bump(lx);
        n = lx->at - start;
        if (n == 1U && is_unit_ch((u8)lx->src[start])) {
            FlTok t = make(FL_M_UNIT, span_at(lx, line, col, from));

            t.v.m.ch = (u8)lx->src[start];
            return t;
        }
        if (n == 1U && lx->src[start] == 'v') {
            FlTok t = make(FL_M_ARROW, span_at(lx, line, col, from));

            t.v.m.ch = (u8)'v';
            return t;
        }
        if (n == 1U && lx->src[start] == 'H') {
            return make(FL_M_H, span_at(lx, line, col, from));
        }
        if (n == 1U && lx->src[start] == 'a') {
            /* `a` alone is only meaningful as an arrow prefix; the ASCII
             * arrows are punctuation so they are not part of this run. */
            u8 nxt = peek(lx);
            u8 ch = 0U;

            if (is_arrow_ch(nxt) && nxt != 'v') {
                FlTok t;

                (void)bump(lx);
                t = make(FL_M_ARROW, span_at(lx, line, col, from));
                t.v.m.ch = nxt;
                t.v.m.alt = true;
                return t;
            }
            if (!at_end(lx)) {
                u32 cp = 0U;
                size_t adv = sag_utf8_decode((const u8 *)lx->src + lx->at,
                                             lx->len - lx->at, &cp);

                if (adv != 0U && arrow_alias(cp, &ch)) {
                    FlTok t;
                    size_t k;

                    for (k = 0U; k < adv; k++)
                        (void)bump(lx);
                    t = make(FL_M_ARROW, span_at(lx, line, col, from));
                    t.v.m.ch = ch;
                    t.v.m.alt = true;
                    return t;
                }
            }
            /* Bare `a`: an ordinary command word. */
        }
        if (n == 2U && memcmp(lx->src + start, "av", 2U) == 0) {
            FlTok t = make(FL_M_ARROW, span_at(lx, line, col, from));

            t.v.m.ch = (u8)'v';
            t.v.m.alt = true;
            return t;
        }
        if (n == 3U && memcmp(lx->src + start, "del", 3U) == 0)
            return make(FL_M_DEL, span_at(lx, line, col, from));
        if (n == 3U && memcmp(lx->src + start, "esc", 3U) == 0)
            return make(FL_M_ESC, span_at(lx, line, col, from));
        if (n == 1U && lx->src[start] == 'i' && peek(lx) == '"') {
            /* i"..." -- §1.5's escape rules apply to the payload. */
            FlTok s = lex_string(lx);
            FlTok t;

            if (s.kind != FL_T_STRING)
                return s;
            t = make(FL_M_INSERT, span_at(lx, line, col, from));
            t.v.str_id = s.v.str_id;
            return t;
        }
        {
            FlTok t = make(FL_M_WORD, span_at(lx, line, col, from));

            t.v.str_id = sag_intern(lx->in, lx->src + start, n);
            return t;
        }
    }
    if (is_arrow_ch(c)) {
        FlTok t;

        (void)bump(lx);
        t = make(FL_M_ARROW, span_at(lx, line, col, from));
        t.v.m.ch = c;
        return t;
    }
    {
        u32 cp = 0U;
        size_t adv = sag_utf8_decode((const u8 *)lx->src + lx->at,
                                     lx->len - lx->at, &cp);
        u8 ch = 0U;

        if (adv != 0U && arrow_alias(cp, &ch)) {
            FlTok t;
            size_t k;

            for (k = 0U; k < adv; k++)
                (void)bump(lx);
            t = make(FL_M_ARROW, span_at(lx, line, col, from));
            t.v.m.ch = ch;
            return t;
        }
    }
    (void)bump(lx);
    return fail(lx, span_at(lx, line, col, from),
                "not a motion word");
}

static FlTok lex_motion(FlLexer *lx)
{
    /* Whitespace-insensitive: `4>` and `4 >` are the same motion.
     * Newlines inside a block are layout too -- §1.2 makes an unclosed
     * `@[` a continuation, so the block may span lines. */
    for (;;) {
        u8 c = peek(lx);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            (void)bump(lx);
            continue;
        }
        if (c == '#') {
            u32 cline = lx->line;
            u32 ccol = lx->col;
            size_t cfrom = lx->at;

            while (!at_end(lx) && peek(lx) != '\n')
                (void)bump(lx);
            /* Kept here too: a directive written inside `@[ ... ]` must
             * reach the runner rather than vanish, or a conformance file
             * silently stops asserting what it says it asserts. */
            if (lx->keep_comments)
                return make(FL_T_COMMENT,
                            span_at(lx, cline, ccol, cfrom));
            continue;
        }
        break;
    }
    if (at_end(lx)) {
        /*
         * END OF INPUT INSIDE A BLOCK IS EOF, AND SILENT.
         *
         * Returning an error token here consumed no bytes, so a caller
         * draining to EOF got the same token at the same offset
         * forever -- fuzz_fl_lex found the hang on the two-byte input
         * `@[`.  Leaving the mode and reporting EOF terminates, and
         * saying nothing is also the right answer: §1.2 makes an
         * unclosed `@[` a CONTINUATION, so the parser turns this into
         * `incomplete` and the Sprint 32 REPL asks for another line.
         * A diagnostic here would make that impossible, because an
         * error and `incomplete` are mutually exclusive.
         */
        lx->motion_depth = 0U;
        return make(FL_T_EOF, span_at(lx, lx->line, lx->col, lx->at));
    }
    {
        u32 line = lx->line;
        u32 col = lx->col;
        size_t from = lx->at;
        u8 c = peek(lx);

        if (c == ']') {
            (void)bump(lx);
            lx->motion_depth--;
            return make(FL_M_END, span_at(lx, line, col, from));
        }
        if (c == '(') {
            (void)bump(lx);
            /* H( ... ) nests WITHOUT leaving motion mode. */
            lx->motion_depth++;
            return make(FL_M_LPAREN, span_at(lx, line, col, from));
        }
        if (c == ')') {
            (void)bump(lx);
            if (lx->motion_depth > 0U)
                lx->motion_depth--;
            return make(FL_M_RPAREN, span_at(lx, line, col, from));
        }
        if (is_digit(c)) {
            u64 n = 0U;
            bool over = false;

            while (is_digit(peek(lx))) {
                n = n * 10U + (u64)(bump(lx) - '0');
                if (n > (u64)FL_MOTION_COUNT_MAX)
                    over = true;
            }
            if (over)
                return fail(lx, span_at(lx, line, col, from),
                            "motion count must be 1 to %d",
                            FL_MOTION_COUNT_MAX);
            if (n == 0U)
                return fail(lx, span_at(lx, line, col, from),
                            "motion count must not be zero");
            {
                FlTok t = make(FL_M_COUNT, span_at(lx, line, col, from));

                t.v.count = (u32)n;
                return t;
            }
        }
        return lex_motion_word(lx);
    }
}

/* ---------------------------------------------------------------- */
/* Main scanner                                                     */
/* ---------------------------------------------------------------- */

FlTok fl_lex_next(FlLexer *lx)
{
    if (lx->motion_depth > 0U)
        return lex_motion(lx);

    for (;;) {
        u8 c;

        if (at_end(lx))
            return make(FL_T_EOF, span_at(lx, lx->line, lx->col, lx->at));
        c = peek(lx);
        if (c == ' ' || c == '\t') {
            (void)bump(lx);
            continue;
        }
        if (c == '#') {
            /* §1.1: to end of line, and the newline still terminates the
             * statement -- swallowing it would join two statements. */
            u32 cline = lx->line;
            u32 ccol = lx->col;
            size_t cfrom = lx->at;

            while (!at_end(lx) && peek(lx) != '\n')
                (void)bump(lx);
            if (lx->keep_comments)
                return make(FL_T_COMMENT,
                            span_at(lx, cline, ccol, cfrom));
            continue;
        }
        break;
    }

    {
        u32 line = lx->line;
        u32 col = lx->col;
        size_t from = lx->at;
        u8 c = peek(lx);

        if (c == '\r') {
            /* CRLF is ONE newline; a bare CR is an error rather than a
             * silent terminator, because a file with mixed endings would
             * otherwise report line numbers nobody can match to a
             * cursor. */
            if (peek2(lx) == '\n') {
                (void)bump(lx);
                (void)bump(lx);
                return make(FL_T_NEWLINE, span_at(lx, line, col, from));
            }
            (void)bump(lx);
            return fail(lx, span_at(lx, line, col, from),
                        "bare carriage return; use '\\n' line endings");
        }
        if (c == '\n') {
            (void)bump(lx);
            return make(FL_T_NEWLINE, span_at(lx, line, col, from));
        }
        if (is_digit(c))
            return lex_number(lx);
        if (c == '"')
            return lex_string(lx);
        if (is_alpha(c)) {
            size_t start = lx->at;
            size_t n;
            FlTokKind kw;

            while (!at_end(lx) && is_ident_cont(peek(lx)))
                (void)bump(lx);
            n = lx->at - start;
            /* Terminator check is implicit: the run is maximal, so
             * `letx` arrives here as four bytes and cannot match `let`. */
            kw = keyword_at(lx->src + start, n);
            if (kw != FL_T_IDENT)
                return make(kw, span_at(lx, line, col, from));
            {
                FlTok t = make(FL_T_IDENT, span_at(lx, line, col, from));

                t.v.str_id = sag_intern(lx->in, lx->src + start, n);
                return t;
            }
        }

        switch (c) {
        case '(': (void)bump(lx); return make(FL_T_LPAREN, span_at(lx, line, col, from));
        case ')': (void)bump(lx); return make(FL_T_RPAREN, span_at(lx, line, col, from));
        case '[': (void)bump(lx); return make(FL_T_LBRACKET, span_at(lx, line, col, from));
        case ']': (void)bump(lx); return make(FL_T_RBRACKET, span_at(lx, line, col, from));
        case '{': (void)bump(lx); return make(FL_T_LBRACE, span_at(lx, line, col, from));
        case '}': (void)bump(lx); return make(FL_T_RBRACE, span_at(lx, line, col, from));
        case ',': (void)bump(lx); return make(FL_T_COMMA, span_at(lx, line, col, from));
        case ':': (void)bump(lx); return make(FL_T_COLON, span_at(lx, line, col, from));
        case ';': (void)bump(lx); return make(FL_T_SEMI, span_at(lx, line, col, from));
        case '.': (void)bump(lx); return make(FL_T_DOT, span_at(lx, line, col, from));
        case '+': (void)bump(lx); return make(FL_T_PLUS, span_at(lx, line, col, from));
        case '-': (void)bump(lx); return make(FL_T_MINUS, span_at(lx, line, col, from));
        case '*': (void)bump(lx); return make(FL_T_STAR, span_at(lx, line, col, from));
        case '/': (void)bump(lx); return make(FL_T_SLASH, span_at(lx, line, col, from));
        case '%': (void)bump(lx); return make(FL_T_PERCENT, span_at(lx, line, col, from));
        case '@':
            (void)bump(lx);
            if (peek(lx) == '[') {
                (void)bump(lx);
                lx->motion_depth = 1U;
                return make(FL_T_ATBRACKET, span_at(lx, line, col, from));
            }
            /* `@` exists only as `@[`; naming the pair is the whole
             * value of the message. */
            return fail(lx, span_at(lx, line, col, from),
                        "'@' must be followed by '['; a motion block is "
                        "written '@[ ... ]'");
        case '=':
            (void)bump(lx);
            if (peek(lx) == '=') {
                (void)bump(lx);
                return make(FL_T_EQEQ, span_at(lx, line, col, from));
            }
            return make(FL_T_EQ, span_at(lx, line, col, from));
        case '!':
            (void)bump(lx);
            if (peek(lx) == '=') {
                (void)bump(lx);
                return make(FL_T_BANGEQ, span_at(lx, line, col, from));
            }
            return fail(lx, span_at(lx, line, col, from),
                        "'!' is only valid as '!='; use 'not' to negate");
        case '<':
            (void)bump(lx);
            if (peek(lx) == '=') {
                (void)bump(lx);
                return make(FL_T_LE, span_at(lx, line, col, from));
            }
            return make(FL_T_LT, span_at(lx, line, col, from));
        case '>':
            (void)bump(lx);
            if (peek(lx) == '=') {
                (void)bump(lx);
                return make(FL_T_GE, span_at(lx, line, col, from));
            }
            return make(FL_T_GT, span_at(lx, line, col, from));
        default:
            break;
        }

        /*
         * Anything else.  Invalid UTF-8 is reported ONCE per run and
         * then resynchronised at the next valid boundary: a binary file
         * fed to the parser would otherwise emit one diagnostic per
         * byte, and the twenty-error cap would hide the real first
         * message behind them.
         */
        {
            u32 cp = 0U;
            size_t adv = sag_utf8_decode((const u8 *)lx->src + lx->at,
                                         lx->len - lx->at, &cp);

            /*
             * Sprint 2's decoder never fails: an invalid byte comes back
             * as a U+DC80..U+DCFF escape scalar so buffer text can round
             * trip.  Spec §1 says that policy is for BUFFER text and not
             * for program text -- "invalid bytes are a compile error" --
             * so the escape is what invalid UTF-8 looks like here, and
             * testing `adv == 0` alone would silently accept it as an
             * ordinary unexpected character.
             */
            if (adv == 0U || sag_utf8_is_escape(cp)) {
                bool first = !lx->utf8_reported;

                lx->utf8_reported = true;
                (void)bump(lx);
                while (!at_end(lx) &&
                       !sag_utf8_is_boundary((const u8 *)lx->src, lx->len,
                                             lx->at))
                    (void)bump(lx);
                if (first)
                    return fail(lx, span_at(lx, line, col, from),
                                "invalid UTF-8 in source");
                return make(FL_T_ERROR, span_at(lx, line, col, from));
            }
            {
                size_t k;

                for (k = 0U; k < adv; k++)
                    (void)bump(lx);
            }
            return fail(lx, span_at(lx, line, col, from),
                        "unexpected character in source");
        }
    }
}
