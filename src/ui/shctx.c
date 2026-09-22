#include "ui/shctx.h"

#include <string.h>

#include "edit/job.h"
#include "util/buf.h"

/*
 * Sprint 57.23 §1: the context lexer.
 *
 * ONE forward pass over [0, caret), bytes not code points: every operator
 * and quote is ASCII, so a multibyte sequence can never be mistaken for
 * one, and invalid bytes pass through into the stem untouched
 * (invariant 2).
 *
 * The state is a stack.  Command-list contexts -- the top level, `(`,
 * `$(`, a backtick, `<(`/`>(`, `{` -- each own a simple command in
 * progress (its words so far and the word being typed).  A double quote
 * is a lighter frame on the same stack: it owns no words, it only
 * changes how bytes join the enclosing command's current word, and it is
 * what lets `"…$(ls sr` nest a command inside a quoted word.  Single
 * quotes and `$'…'` cannot nest anything, so they are a mode of the
 * current word rather than a frame.
 *
 * Constructs the completer never completes INSIDE (comments, arithmetic,
 * `${…}` bodies, here-document bodies) are skipped eagerly by a sub-scan;
 * a caret that lands in one is NONE.
 */

enum {
    FR_TOP,
    FR_PAREN,
    FR_CMDSUB,
    FR_BACKTICK,
    FR_PROCSUB,
    FR_BRACE,
    FR_DQ
};

enum {
    Q_NONE,
    Q_SINGLE,
    Q_DOLLAR
};

/* Word flags. */
enum {
    W_QUOTED = 1U << 0,  /* some byte was quoted or escaped            */
    W_EXPANDS = 1U << 1, /* holds an active $/`/$( expansion            */
    W_ASSIGN = 1U << 2,  /* unquoted NAME= prefix                        */
    W_SYNTH = 1U << 3    /* stands in for "no command" (`fi > x`)        */
};

/* What the next word IS, when an operator has decided it already. */
enum {
    EXP_NONE,
    EXP_REDIR,
    EXP_HEREDOC,
    EXP_HERESTR
};

enum {
    FOR_NONE,
    FOR_NAME,
    FOR_AFTER_NAME,
    FOR_LIST
};

enum {
    CASE_NONE,
    CASE_WORD,
    CASE_IN,
    CASE_BODY
};

#define SH_NO_OFF ((size_t)-1)

typedef struct ShWord {
    char *text;
    u8 flags;
} ShWord;

typedef struct CmdFr {
    u8 kind;
    /* The simple command so far. */
    ShWord *words;
    u32 n;
    u32 cap;
    u8 expect;
    u8 forst;
    u8 casest;
    u32 case_nest;
    bool dbracket; /* inside [[ … ]]: < > && || ( ) are words */
    /* The word being typed. */
    bool in_word;
    size_t w_start;
    Bytebuf dec;
    u8 w_flags;
    u8 qmode;
    bool w_digits;   /* all unquoted digits so far: an fd prefix candidate */
    bool w_tilde;    /* first byte an unquoted `~`                          */
    bool w_name;     /* still a valid NAME for an assignment                */
    bool w_eq;       /* NAME= seen                                           */
    bool w_tilde_eq; /* first byte after `=` an unquoted `~`                */
    size_t w_eq_raw; /* raw offset just past the `=`                        */
    size_t w_eq_dec; /* decoded length at the same point                    */
    size_t w_expand_raw;
} CmdFr;

typedef struct HereDoc {
    char *delim;
    bool strip_tabs;
} HereDoc;

enum {
    /* Total frames, double quotes included.  Two per level is the most
     * a caret can need (`"$("$(…`), plus the top level. */
    SH_STACK_MAX = 2 * YEW_SH_DEPTH_MAX + 2,
    SH_HEREDOC_MAX = 16
};

typedef struct Lexer {
    const char *line;
    size_t end; /* the caret: nothing at or past it is read */
    Arena *a;
    u8 kinds[SH_STACK_MAX];
    u32 nk;
    CmdFr cmd[YEW_SH_DEPTH_MAX + 1];
    u32 nc;
    HereDoc heredocs[SH_HEREDOC_MAX];
    u32 nheredocs;
    /* The scan has decided the answer and stops early. */
    bool stop;
    bool none;
    bool var;
    bool var_brace;
    size_t var_start;
} Lexer;

static bool is_name_start(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(unsigned char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int peek(const Lexer *L, size_t at)
{
    return at < L->end ? (int)(unsigned char)L->line[at] : -1;
}

static CmdFr *cur_cmd(Lexer *L)
{
    return &L->cmd[L->nc - 1U];
}

static u8 top_kind(const Lexer *L)
{
    return L->kinds[L->nk - 1U];
}

static void give_up(Lexer *L)
{
    L->none = true;
    L->stop = true;
}

static void frame_init(CmdFr *c, u8 kind)
{
    (void)memset(c, 0, sizeof(*c));
    c->kind = kind;
    bytebuf_init(&c->dec);
    c->w_expand_raw = SH_NO_OFF;
}

static bool push_cmd(Lexer *L, u8 kind)
{
    if (L->nc >= YEW_SH_DEPTH_MAX + 1U || L->nk >= SH_STACK_MAX) {
        give_up(L);
        return false;
    }
    frame_init(&L->cmd[L->nc], kind);
    L->nc++;
    L->kinds[L->nk++] = kind;
    return true;
}

static bool push_dq(Lexer *L)
{
    if (L->nk >= SH_STACK_MAX) {
        give_up(L);
        return false;
    }
    L->kinds[L->nk++] = FR_DQ;
    return true;
}

static void frame_dispose(CmdFr *c)
{
    bytebuf_free(&c->dec);
    yew_xfree(c->words);
    c->words = NULL;
}

/* Pops the top frame.  Callers only pop a command frame that is on top. */
static void pop_frame(Lexer *L)
{
    if (L->nk <= 1U)
        return;
    if (L->kinds[L->nk - 1U] != FR_DQ) {
        frame_dispose(&L->cmd[L->nc - 1U]);
        L->nc--;
    }
    L->nk--;
}

static void push_word(Lexer *L, CmdFr *c, char *text, u8 flags)
{
    (void)L;
    if (c->n == c->cap) {
        u32 next = c->cap == 0U ? 8U : c->cap * 2U;

        c->words = yew_xrealloc(c->words, (size_t)next * sizeof(*c->words));
        c->cap = next;
    }
    c->words[c->n].text = text;
    c->words[c->n].flags = flags;
    c->n++;
}

static void push_synth(Lexer *L, CmdFr *c)
{
    push_word(L, c, arena_strdup(L->a, ""), W_SYNTH);
}

static void word_begin(CmdFr *c, size_t at)
{
    if (c->in_word)
        return;
    c->in_word = true;
    c->w_start = at;
    c->dec.len = 0U;
    c->w_flags = 0U;
    c->w_digits = true;
    c->w_tilde = false;
    c->w_name = true;
    c->w_eq = false;
    c->w_tilde_eq = false;
    c->w_eq_raw = 0U;
    c->w_eq_dec = 0U;
    c->w_expand_raw = SH_NO_OFF;
}

/* One decoded byte joins the current word.  `raw` is where it came from,
 * for the `=` of an assignment. */
static void word_byte(CmdFr *c, unsigned char b, bool quoted, size_t raw)
{
    size_t before = c->dec.len;

    if (quoted) {
        c->w_flags |= W_QUOTED;
        c->w_digits = false;
        if (!c->w_eq)
            c->w_name = false;
    } else {
        if (!is_digit(b))
            c->w_digits = false;
        if (before == 0U && b == '~')
            c->w_tilde = true;
        if (c->w_eq && before == c->w_eq_dec && b == '~')
            c->w_tilde_eq = true;
        if (!c->w_eq && c->w_name) {
            if (b == '=' && before != 0U) {
                c->w_eq = true;
                c->w_eq_raw = raw + 1U;
                c->w_eq_dec = before + 1U;
            } else if (!(is_name_start(b) || (before != 0U && is_digit(b)))) {
                c->w_name = false;
            }
        }
    }
    bytebuf_push_u8(&c->dec, b);
}

static void word_expands(CmdFr *c, size_t raw)
{
    c->w_flags |= W_EXPANDS;
    c->w_digits = false;
    if (!c->w_eq)
        c->w_name = false;
    if (c->w_expand_raw == SH_NO_OFF)
        c->w_expand_raw = raw;
}

static bool word_is(const char *text, u8 flags, const char *want)
{
    return (flags & (W_QUOTED | W_EXPANDS)) == 0U && strcmp(text, want) == 0;
}

static void after_construct(Lexer *L, CmdFr *c)
{
    c->n = 0U;
    c->expect = EXP_NONE;
    c->dbracket = false;
    push_synth(L, c);
}

/* A reserved word that keeps the NEXT word in command position. */
static bool opens_command(const char *t)
{
    static const char *const words[] = {"!", "if", "then", "else", "elif",
                                        "do", "while", "until", "time"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(words); i++) {
        if (strcmp(t, words[i]) == 0)
            return true;
    }
    return false;
}

/*
 * Finish the current word.  Returns the command frame that is current
 * AFTERWARDS: a reserved `{` or `}` pushes or pops one, and a caller that
 * kept using `c` would then edit a frame that is no longer on the stack.
 */
static CmdFr *word_end(Lexer *L, CmdFr *c)
{
    char *text;
    u8 flags;

    if (!c->in_word)
        return cur_cmd(L);
    c->in_word = false;
    c->qmode = Q_NONE;
    text = arena_strndup(L->a, (const char *)c->dec.data, c->dec.len);
    flags = c->w_flags;
    if (c->w_eq)
        flags |= W_ASSIGN;
    switch (c->expect) {
    case EXP_REDIR:
    case EXP_HERESTR:
        /* A redirection's word is not an operand (§1 "Redirections"). */
        c->expect = EXP_NONE;
        return cur_cmd(L);
    case EXP_HEREDOC:
        c->expect = EXP_NONE;
        if (L->nheredocs >= SH_HEREDOC_MAX) {
            give_up(L);
            return cur_cmd(L);
        }
        /* strip_tabs was set by the operator (`<<-`). */
        L->heredocs[L->nheredocs].delim = text;
        L->nheredocs++;
        return cur_cmd(L);
    default:
        break;
    }
    if (c->casest == CASE_BODY) {
        if (word_is(text, flags, "case")) {
            c->case_nest++;
        } else if (word_is(text, flags, "esac")) {
            if (c->case_nest != 0U) {
                c->case_nest--;
            } else {
                c->casest = CASE_NONE;
                after_construct(L, c);
            }
        }
        return cur_cmd(L);
    }
    if (c->casest == CASE_WORD) {
        c->casest = CASE_IN;
        return cur_cmd(L);
    }
    if (c->casest == CASE_IN) {
        /* `in` or not, what follows is the body: a syntax error is not
         * worth a separate state. */
        c->casest = CASE_BODY;
        return cur_cmd(L);
    }
    if (c->forst == FOR_NAME) {
        c->forst = FOR_AFTER_NAME;
        return cur_cmd(L);
    }
    if (c->forst == FOR_AFTER_NAME) {
        if (word_is(text, flags, "in")) {
            c->forst = FOR_LIST;
            push_synth(L, c);
        } else {
            c->forst = FOR_NONE;
        }
        return cur_cmd(L);
    }
    if (c->dbracket) {
        if (word_is(text, flags, "]]"))
            c->dbracket = false;
        push_word(L, c, text, flags);
        return cur_cmd(L);
    }
    if (c->n == 0U && (flags & (W_QUOTED | W_EXPANDS)) == 0U) {
        if (opens_command(text))
            return cur_cmd(L);
        if (strcmp(text, "{") == 0) {
            (void)push_cmd(L, FR_BRACE);
            return cur_cmd(L);
        }
        if (strcmp(text, "}") == 0) {
            if (top_kind(L) == FR_BRACE) {
                pop_frame(L);
                after_construct(L, cur_cmd(L));
            } else {
                after_construct(L, c);
            }
            return cur_cmd(L);
        }
        if (strcmp(text, "fi") == 0 || strcmp(text, "done") == 0 ||
            strcmp(text, "esac") == 0) {
            after_construct(L, c);
            return cur_cmd(L);
        }
        if (strcmp(text, "for") == 0 || strcmp(text, "select") == 0) {
            c->forst = FOR_NAME;
            return cur_cmd(L);
        }
        if (strcmp(text, "case") == 0) {
            c->casest = CASE_WORD;
            push_synth(L, c);
            return cur_cmd(L);
        }
        if (strcmp(text, "[[") == 0)
            c->dbracket = true;
    }
    push_word(L, c, text, flags);
    return cur_cmd(L);
}

/* A control operator or newline: the simple command is over. */
static CmdFr *end_cmd(Lexer *L, CmdFr *c)
{
    c = word_end(L, c);
    c->n = 0U;
    c->expect = EXP_NONE;
    c->dbracket = false;
    if (c->forst != FOR_NAME)
        c->forst = FOR_NONE;
    return c;
}

/* `[[`'s operators are words, not control operators. */
static void dbracket_word(Lexer *L, CmdFr *c, const char *op)
{
    c = word_end(L, c);
    push_word(L, c, arena_strdup(L->a, op), 0U);
}

/*
 * Skip a here-document body that starts at `at` (just past the newline
 * that ended its command line).  Returns the offset after the last
 * delimiter line, or sets NONE when the caret is inside a body.
 */
static size_t skip_heredocs(Lexer *L, size_t at)
{
    u32 h;

    for (h = 0U; h < L->nheredocs; h++) {
        const char *delim = L->heredocs[h].delim;
        size_t dlen = strlen(delim);

        for (;;) {
            size_t ls = at;
            size_t le;

            if (at >= L->end) {
                give_up(L);
                return L->end;
            }
            while (at < L->end && L->line[at] != '\n')
                at++;
            if (at >= L->end) {
                /* The caret is on a body line: named limit, NONE. */
                give_up(L);
                return L->end;
            }
            le = at;
            at++;
            while (L->heredocs[h].strip_tabs && ls < le &&
                   L->line[ls] == '\t')
                ls++;
            if (le - ls == dlen && memcmp(L->line + ls, delim, dlen) == 0)
                break;
        }
    }
    L->nheredocs = 0U;
    return at;
}

/*
 * `$((…))` / `((…))`: skip to the closing `))`.  `from` is just past the
 * opening pair.  NONE if the caret is inside.
 */
static size_t skip_arith(Lexer *L, size_t from)
{
    size_t at = from;
    u32 depth = 0U;

    while (at < L->end) {
        char c = L->line[at];

        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth == 0U) {
                if (at + 1U < L->end && L->line[at + 1U] == ')')
                    return at + 2U;
                break;
            }
            depth--;
        }
        at++;
    }
    give_up(L);
    return L->end;
}

/* Append the UTF-8 encoding of `cp` (a $'\u…' escape) to the word. */
static void word_utf8(CmdFr *c, u32 cp, size_t raw)
{
    unsigned char b[4];
    size_t n;
    size_t i;

    if (cp < 0x80U) {
        b[0] = (unsigned char)cp;
        n = 1U;
    } else if (cp < 0x800U) {
        b[0] = (unsigned char)(0xC0U | (cp >> 6));
        b[1] = (unsigned char)(0x80U | (cp & 0x3FU));
        n = 2U;
    } else if (cp < 0x10000U) {
        b[0] = (unsigned char)(0xE0U | (cp >> 12));
        b[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3FU));
        b[2] = (unsigned char)(0x80U | (cp & 0x3FU));
        n = 3U;
    } else {
        b[0] = (unsigned char)(0xF0U | (cp >> 18));
        b[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3FU));
        b[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3FU));
        b[3] = (unsigned char)(0x80U | (cp & 0x3FU));
        n = 4U;
    }
    for (i = 0U; i < n; i++)
        word_byte(c, b[i], true, raw);
}

static int hex_val(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/*
 * One backslash escape inside `$'…'`, decoded for the STEM only (§1's
 * quoting table).  `at` is on the backslash; returns the offset after
 * the escape.  An escape the caret truncates decodes as far as it goes.
 */
static size_t dollar_escape(Lexer *L, CmdFr *c, size_t at)
{
    int e = peek(L, at + 1U);
    size_t p = at + 2U;
    u32 v = 0U;
    u32 digits = 0U;

    switch (e) {
    case -1:
        return at + 1U;
    case 'a': word_byte(c, 7U, true, at); return p;
    case 'b': word_byte(c, 8U, true, at); return p;
    case 'e':
    case 'E': word_byte(c, 27U, true, at); return p;
    case 'f': word_byte(c, 12U, true, at); return p;
    case 'n': word_byte(c, 10U, true, at); return p;
    case 'r': word_byte(c, 13U, true, at); return p;
    case 't': word_byte(c, 9U, true, at); return p;
    case 'v': word_byte(c, 11U, true, at); return p;
    case '\\':
    case '\'':
    case '"':
    case '?':
        word_byte(c, (unsigned char)e, true, at);
        return p;
    case 'c':
        if (peek(L, p) >= 0) {
            word_byte(c, (unsigned char)(peek(L, p) & 0x1F), true, at);
            return p + 1U;
        }
        return p;
    case 'x':
    case 'u':
    case 'U': {
        u32 max = e == 'x' ? 2U : e == 'u' ? 4U : 8U;

        while (digits < max && hex_val(peek(L, p)) >= 0) {
            v = v * 16U + (u32)hex_val(peek(L, p));
            p++;
            digits++;
        }
        if (digits == 0U) {
            word_byte(c, '\\', true, at);
            word_byte(c, (unsigned char)e, true, at);
            return p;
        }
        if (e == 'x')
            word_byte(c, (unsigned char)v, true, at);
        else if (v <= 0x10FFFFU && !(v >= 0xD800U && v <= 0xDFFFU))
            word_utf8(c, v, at);
        return p;
    }
    default:
        break;
    }
    if (e >= '0' && e <= '7') {
        p = at + 1U;
        while (digits < 3U && peek(L, p) >= '0' && peek(L, p) <= '7') {
            v = v * 8U + (u32)(peek(L, p) - '0');
            p++;
            digits++;
        }
        word_byte(c, (unsigned char)(v & 0xFFU), true, at);
        return p;
    }
    /* Unknown: bash keeps both bytes. */
    word_byte(c, '\\', true, at);
    word_byte(c, (unsigned char)e, true, at);
    return p;
}

/*
 * `$` in a word, unquoted or inside "…".  Returns the next offset.  A
 * caret inside a variable NAME ends the scan with a VARIABLE answer.
 */
static size_t lex_dollar(Lexer *L, CmdFr *c, size_t at, bool in_dq)
{
    int n1 = peek(L, at + 1U);
    size_t j;

    word_begin(c, at);
    if (n1 < 0) {
        /* `$` right at the caret: the variable name is empty so far. */
        word_expands(c, at);
        L->var = true;
        L->var_start = at + 1U;
        L->stop = true;
        return L->end;
    }
    if (n1 == '(') {
        word_expands(c, at);
        if (peek(L, at + 2U) == '(')
            return skip_arith(L, at + 3U);
        (void)push_cmd(L, FR_CMDSUB);
        return at + 2U;
    }
    if (n1 == '{') {
        word_expands(c, at);
        j = at + 2U;
        if (peek(L, j) < 0 || is_name_start((unsigned char)peek(L, j))) {
            while (peek(L, j) >= 0 && is_name_char((unsigned char)peek(L, j)))
                j++;
            if (j >= L->end) {
                L->var = true;
                L->var_brace = true;
                L->var_start = at + 2U;
                L->stop = true;
                return L->end;
            }
        }
        {
            u32 depth = 0U;

            for (; j < L->end; j++) {
                char ch = L->line[j];

                if (ch == '\\') {
                    j++;
                } else if (ch == '{') {
                    depth++;
                } else if (ch == '}') {
                    if (depth == 0U)
                        return j + 1U;
                    depth--;
                }
            }
        }
        /* The caret is inside a ${…} operator: not a name, not a word. */
        give_up(L);
        return L->end;
    }
    if (!in_dq && n1 == '\'') {
        c->w_flags |= W_QUOTED;
        c->w_digits = false;
        c->w_name = c->w_eq ? c->w_name : false;
        c->qmode = Q_DOLLAR;
        return at + 2U;
    }
    if (!in_dq && n1 == '"') {
        /* $"…" is a locale-translated "…"; lexically the same. */
        c->w_flags |= W_QUOTED;
        c->w_digits = false;
        c->w_name = c->w_eq ? c->w_name : false;
        (void)push_dq(L);
        return at + 2U;
    }
    if (is_name_start((unsigned char)n1)) {
        word_expands(c, at);
        j = at + 1U;
        while (j < L->end && is_name_char((unsigned char)L->line[j]))
            j++;
        if (j >= L->end) {
            L->var = true;
            L->var_start = at + 1U;
            L->stop = true;
            return L->end;
        }
        return j;
    }
    if (is_digit((unsigned char)n1) ||
        (n1 != '\0' && strchr("?$!#@*-", n1) != NULL)) {
        word_expands(c, at);
        return at + 2U;
    }
    /* A lone `$` is a literal dollar sign. */
    word_byte(c, '$', in_dq, at);
    return at + 1U;
}

/* One step inside "…".  `c` is the command frame that owns the word. */
static size_t lex_dq(Lexer *L, CmdFr *c, size_t at)
{
    unsigned char ch = (unsigned char)L->line[at];
    int n1;

    switch (ch) {
    case '"':
        pop_frame(L);
        return at + 1U;
    case '\\':
        n1 = peek(L, at + 1U);
        if (n1 < 0)
            return at + 1U;
        if (n1 == '\n')
            return at + 2U;
        if (n1 == '"' || n1 == '\\' || n1 == '$' || n1 == '`') {
            word_byte(c, (unsigned char)n1, true, at + 1U);
            return at + 2U;
        }
        word_byte(c, '\\', true, at);
        return at + 1U;
    case '$':
        return lex_dollar(L, c, at, true);
    case '`':
        word_expands(c, at);
        (void)push_cmd(L, FR_BACKTICK);
        return at + 1U;
    default:
        word_byte(c, ch, true, at);
        return at + 1U;
    }
}

/* A redirection operator starting at `at` (on `<`, `>` or `&`). */
static size_t lex_redirect(Lexer *L, CmdFr *c, size_t at)
{
    char ch = L->line[at];
    int n1 = peek(L, at + 1U);
    size_t p;
    bool dup = false;

    /* A digit run is an fd prefix only when immediately followed by `<`
     * or `>` -- and then it is part of the operator, not an operand. */
    if (c->in_word && c->w_digits && c->dec.len != 0U && c->w_flags == 0U &&
        ch != '&')
        c->in_word = false;
    else
        c = word_end(L, c);
    if (ch == '&') {
        /* &> and &>> */
        p = at + 2U;
        if (peek(L, p) == '>')
            p++;
        c->expect = EXP_REDIR;
        return p;
    }
    if (ch == '<') {
        if (n1 == '<') {
            if (peek(L, at + 2U) == '<') {
                c->expect = EXP_HERESTR;
                return at + 3U;
            }
            c->expect = EXP_HEREDOC;
            if (peek(L, at + 2U) == '-') {
                /* Remember the tab stripping for the delimiter the next
                 * word will record. */
                c->expect = EXP_HEREDOC;
                p = at + 3U;
                /* The flag rides on the next heredoc slot. */
                if (L->nheredocs < SH_HEREDOC_MAX)
                    L->heredocs[L->nheredocs].strip_tabs = true;
                return p;
            }
            if (L->nheredocs < SH_HEREDOC_MAX)
                L->heredocs[L->nheredocs].strip_tabs = false;
            return at + 2U;
        }
        if (n1 == '>') {
            c->expect = EXP_REDIR;
            return at + 2U;
        }
        if (n1 == '&') {
            dup = true;
            p = at + 2U;
        } else {
            c->expect = EXP_REDIR;
            return at + 1U;
        }
    } else {
        if (n1 == '>' || n1 == '|') {
            c->expect = EXP_REDIR;
            return at + 2U;
        }
        if (n1 == '&') {
            dup = true;
            p = at + 2U;
        } else {
            c->expect = EXP_REDIR;
            return at + 1U;
        }
    }
    /* `>&` / `<&`: a digit or `-` is an fd duplication and consumes
     * nothing further; anything else is a file target (bash's `>&file`). */
    if (dup) {
        size_t q = p;

        while (q < L->end && is_digit((unsigned char)L->line[q]))
            q++;
        if (q < L->end && L->line[q] == '-')
            q++;
        if (q != p) {
            c->expect = EXP_NONE;
            return q;
        }
        c->expect = EXP_REDIR;
    }
    return p;
}

/* One step in a command-list context, outside any quote. */
static size_t lex_cmd(Lexer *L, CmdFr *c, size_t at)
{
    unsigned char ch = (unsigned char)L->line[at];
    int n1 = peek(L, at + 1U);

    switch (ch) {
    case ' ':
    case '\t':
        word_end(L, c);
        return at + 1U;
    case '\n':
        end_cmd(L, c);
        if (L->nheredocs != 0U)
            return skip_heredocs(L, at + 1U);
        return at + 1U;
    case '\\':
        word_begin(c, at);
        if (n1 < 0) {
            /* The escape is cut by the caret: it belongs to the word the
             * completion replaces, and decodes to nothing yet. */
            c->w_flags |= W_QUOTED;
            c->w_digits = false;
            return at + 1U;
        }
        if (n1 == '\n') {
            /* Line continuation: both bytes vanish.  A word that has not
             * started yet is not started by it. */
            if (c->dec.len == 0U && c->w_start == at)
                c->in_word = false;
            return at + 2U;
        }
        word_byte(c, (unsigned char)n1, true, at + 1U);
        return at + 2U;
    case '\'':
        word_begin(c, at);
        c->w_flags |= W_QUOTED;
        c->w_digits = false;
        if (!c->w_eq)
            c->w_name = false;
        c->qmode = Q_SINGLE;
        return at + 1U;
    case '"':
        word_begin(c, at);
        c->w_flags |= W_QUOTED;
        c->w_digits = false;
        if (!c->w_eq)
            c->w_name = false;
        (void)push_dq(L);
        return at + 1U;
    case '`':
        if (top_kind(L) == FR_BACKTICK) {
            word_end(L, c);
            pop_frame(L);
            return at + 1U;
        }
        word_begin(c, at);
        word_expands(c, at);
        (void)push_cmd(L, FR_BACKTICK);
        return at + 1U;
    case '$':
        return lex_dollar(L, c, at, false);
    case '#':
        if (c->in_word)
            break;
        /* A comment runs to the newline; a caret before it is NONE. */
        {
            size_t p = at;

            while (p < L->end && L->line[p] != '\n')
                p++;
            if (p >= L->end) {
                give_up(L);
                return L->end;
            }
            return p;
        }
    case '|':
        if (c->dbracket) {
            dbracket_word(L, c, n1 == '|' ? "||" : "|");
            return n1 == '|' ? at + 2U : at + 1U;
        }
        end_cmd(L, c);
        return n1 == '|' || n1 == '&' ? at + 2U : at + 1U;
    case '&':
        if (n1 == '>')
            return lex_redirect(L, c, at);
        if (c->dbracket && n1 == '&') {
            dbracket_word(L, c, "&&");
            return at + 2U;
        }
        end_cmd(L, c);
        return n1 == '&' ? at + 2U : at + 1U;
    case ';': {
        size_t p = at + 1U;

        if (peek(L, p) == ';') {
            p++;
            if (peek(L, p) == '&')
                p++;
        } else if (peek(L, p) == '&') {
            p++;
        }
        end_cmd(L, c);
        return p;
    }
    case '(':
        if (c->casest == CASE_BODY) {
            word_end(L, c);
            return at + 1U;
        }
        if (c->dbracket) {
            dbracket_word(L, c, "(");
            return at + 1U;
        }
        if (!c->in_word && n1 == '(' &&
            (c->n == 0U || c->forst == FOR_NAME)) {
            c->forst = FOR_NONE;
            at = skip_arith(L, at + 2U);
            if (!L->stop)
                after_construct(L, c);
            return at;
        }
        c = word_end(L, c);
        if (n1 == ')') {
            /* `NAME() {` is not recognised: `()` is an ordinary word. */
            word_begin(c, at);
            word_byte(c, '(', false, at);
            word_byte(c, ')', false, at + 1U);
            word_end(L, c);
            return at + 2U;
        }
        (void)push_cmd(L, FR_PAREN);
        return at + 1U;
    case ')':
        if (c->casest == CASE_BODY) {
            word_end(L, c);
            return at + 1U;
        }
        if (c->dbracket) {
            dbracket_word(L, c, ")");
            return at + 1U;
        }
        c = word_end(L, c);
        if (top_kind(L) == FR_PAREN) {
            pop_frame(L);
            after_construct(L, cur_cmd(L));
        } else if (top_kind(L) == FR_CMDSUB || top_kind(L) == FR_PROCSUB) {
            /* The enclosing word continues after the substitution. */
            pop_frame(L);
        } else {
            c->expect = EXP_NONE;
        }
        return at + 1U;
    case '<':
    case '>':
        if (c->dbracket) {
            dbracket_word(L, c, ch == '<' ? "<" : ">");
            return at + 1U;
        }
        if (n1 == '(') {
            word_begin(c, at);
            word_expands(c, at);
            (void)push_cmd(L, FR_PROCSUB);
            return at + 2U;
        }
        return lex_redirect(L, c, at);
    default:
        break;
    }
    word_begin(c, at);
    word_byte(c, ch, false, at);
    return at + 1U;
}

/* One step inside '…' or $'…' (a mode of the current word). */
static size_t lex_squote(Lexer *L, CmdFr *c, size_t at)
{
    unsigned char ch = (unsigned char)L->line[at];

    if (ch == '\'') {
        c->qmode = Q_NONE;
        return at + 1U;
    }
    if (c->qmode == Q_DOLLAR && ch == '\\')
        return dollar_escape(L, c, at);
    word_byte(c, ch, true, at);
    return at + 1U;
}

/* ---------------------------------------------------------------- */
/* §2: simple-command resolution                                     */
/* ---------------------------------------------------------------- */

typedef struct Wrapper {
    const char *name;
    const char *const *consumes; /* flags taking the next word; NULL-ended */
    bool duration;               /* timeout: then ONE duration word        */
    bool skips_assign;           /* env: NAME=value words are its own      */
} Wrapper;

static const char *const sudo_flags[] = {"-u", "-g", "-C", "-h", "-p", "-U",
                                         "-r", "-t", "-D", "-R", NULL};
static const char *const doas_flags[] = {"-u", "-C", NULL};
static const char *const env_flags[] = {"-u", "-C", "-S", NULL};
static const char *const nice_flags[] = {"-n", NULL};
static const char *const timeout_flags[] = {"-s", "-k", NULL};
static const char *const xargs_flags[] = {"-a", "-d", "-E", "-e", "-I", "-i",
                                          "-L", "-l", "-n", "-P", "-s",
                                          NULL};
static const char *const no_flags[] = {NULL};

/* §2's built-in table, replaceable by Sprint 57.24's specs. */
static const Wrapper wrappers[] = {
    {"sudo", sudo_flags, false, true},
    {"doas", doas_flags, false, false},
    {"env", env_flags, false, true},
    {"nice", nice_flags, false, false},
    {"timeout", timeout_flags, true, false},
    {"xargs", xargs_flags, false, false},
    {"nohup", no_flags, false, false},
    {"time", no_flags, false, false},
    {"command", no_flags, false, false},
    {"builtin", no_flags, false, false},
    {"exec", no_flags, false, false},
    {"caffeinate", no_flags, false, false},
    {"unbuffer", no_flags, false, false}
};

static const Wrapper *wrapper_of(const ShWord *w)
{
    size_t i;

    if ((w->flags & (W_QUOTED | W_EXPANDS | W_SYNTH)) != 0U)
        return NULL;
    for (i = 0U; i < YEW_ARRAY_LEN(wrappers); i++) {
        if (strcmp(w->text, wrappers[i].name) == 0)
            return &wrappers[i];
    }
    return NULL;
}

static bool wrapper_consumes(const Wrapper *wr, const char *flag)
{
    size_t i;

    for (i = 0U; wr->consumes[i] != NULL; i++) {
        if (strcmp(wr->consumes[i], flag) == 0)
            return true;
    }
    return false;
}

static u32 skip_assign(const ShWord *w, u32 i, u32 n)
{
    while (i < n && (w[i].flags & W_ASSIGN) != 0U)
        i++;
    return i;
}

typedef enum CaretRole {
    ROLE_WORD,
    ROLE_REDIR,
    ROLE_HERESTR
} CaretRole;

/*
 * Where does the command word sit once assignments and wrappers are
 * stripped?  Returns its index, or `n` for "the caret is in command
 * position".  The caret word itself is not in `w`: `caret_flag` says it
 * looks like an option, which keeps it with a wrapper that has not
 * finished its options.
 */
static u32 command_index(const ShWord *w, u32 n, CaretRole role,
                         bool caret_flag)
{
    u32 i = skip_assign(w, 0U, n);

    for (;;) {
        const Wrapper *wr;
        u32 j;
        bool ended = false;

        if (i >= n)
            return n;
        wr = wrapper_of(&w[i]);
        if (wr == NULL)
            return i;
        j = i + 1U;
        while (j < n) {
            const char *t = w[j].text;

            if ((w[j].flags & W_QUOTED) == 0U && strcmp(t, "--") == 0) {
                j++;
                ended = true;
                break;
            }
            if (t[0] == '-' && t[1] != '\0') {
                if (wrapper_consumes(wr, t)) {
                    if (j + 1U >= n)
                        return i; /* the caret is this flag's argument */
                    j += 2U;
                    continue;
                }
                j++;
                continue;
            }
            if (wr->skips_assign && (w[j].flags & W_ASSIGN) != 0U) {
                j++;
                continue;
            }
            break;
        }
        if (j >= n) {
            /* The caret comes straight after the wrapper's options. */
            if (role != ROLE_WORD)
                return i;
            if (!ended && caret_flag)
                return i;
            if (wr->duration)
                return i;
            return n;
        }
        if (wr->duration)
            j++;
        i = skip_assign(w, j, n);
    }
}

static void resolve(Lexer *L, const CmdFr *c, CaretRole role, bool assign,
                    YewShCtx *out)
{
    const ShWord *w = c->words;
    u32 n = c->n;
    bool caret_flag = out->stem[0] == '-' &&
                      (!c->in_word || (c->w_flags & W_QUOTED) == 0U);
    u32 cmd = command_index(w, n, role, caret_flag);
    u32 k;

    if (role == ROLE_WORD && cmd == n) {
        out->pos = assign ? YEW_SH_POS_ASSIGN : YEW_SH_POS_COMMAND;
        out->argc = 0U;
        out->arg_index = 0U;
        out->argv = arena_alloc(L->a, sizeof(char *), sizeof(void *));
        out->argv[0] = NULL;
        return;
    }
    out->pos = role == ROLE_REDIR ? YEW_SH_POS_REDIRECT
                                  : YEW_SH_POS_ARGUMENT;
    if (cmd == n) {
        /* A redirection with no command yet: the operands of nothing. */
        out->argc = 2U;
        out->arg_index = 1U;
        out->argv = arena_alloc(L->a, 3U * sizeof(char *), sizeof(void *));
        out->argv[0] = arena_strdup(L->a, "");
        out->argv[1] = out->stem;
        out->argv[2] = NULL;
        return;
    }
    out->arg_index = n - cmd;
    out->argc = out->arg_index + 1U;
    out->argv = arena_alloc(L->a, ((size_t)out->argc + 1U) * sizeof(char *),
                            sizeof(void *));
    for (k = cmd; k < n; k++)
        out->argv[k - cmd] = w[k].text;
    out->argv[out->arg_index] = out->stem;
    out->argv[out->argc] = NULL;
    if (role == ROLE_WORD) {
        for (k = cmd + 1U; k < n; k++) {
            if ((w[k].flags & W_QUOTED) == 0U && strcmp(w[k].text, "--") == 0)
                out->dashdash = true;
        }
    }
}

static void set_none(Lexer *L, YewShCtx *out)
{
    out->pos = YEW_SH_POS_NONE;
    out->replace = (Span){L->end, L->end};
    out->stem = arena_strdup(L->a, "");
    out->argc = 0U;
    out->arg_index = 0U;
    out->argv = arena_alloc(L->a, sizeof(char *), sizeof(void *));
    out->argv[0] = NULL;
    out->dashdash = false;
    out->brace_var = false;
    out->tilde = false;
    out->expands = false;
}

static void finish(Lexer *L, YewShCtx *out)
{
    CmdFr *c = cur_cmd(L);
    u32 i;
    bool assign = false;
    CaretRole role = ROLE_WORD;

    out->depth = 0U;
    for (i = 1U; i < L->nk; i++) {
        if (L->kinds[i] != FR_DQ)
            out->depth++;
    }
    out->quote = top_kind(L) == FR_DQ ? YEW_SH_Q_DOUBLE
                 : c->qmode == Q_SINGLE ? YEW_SH_Q_SINGLE
                 : c->qmode == Q_DOLLAR ? YEW_SH_Q_DOLLAR
                                        : YEW_SH_Q_NONE;
    if (L->none) {
        set_none(L, out);
        return;
    }
    for (i = 0U; i < L->nc; i++) {
        if (L->cmd[i].casest == CASE_BODY || L->cmd[i].casest == CASE_IN) {
            set_none(L, out);
            return;
        }
    }
    if (c->expect == EXP_HEREDOC || c->forst == FOR_NAME ||
        c->forst == FOR_AFTER_NAME) {
        set_none(L, out);
        return;
    }
    if (c->expect == EXP_REDIR)
        role = ROLE_REDIR;
    else if (c->expect == EXP_HERESTR)
        role = ROLE_HERESTR;
    if (L->var) {
        out->replace = (Span){L->var_start, L->end};
        out->stem = arena_strndup(L->a, L->line + L->var_start,
                                  L->end - L->var_start);
        out->brace_var = L->var_brace;
        out->tilde = false;
        out->expands = false;
    } else if (c->in_word) {
        out->replace = (Span){c->w_start, L->end};
        out->stem = arena_strndup(L->a, (const char *)c->dec.data,
                                  c->dec.len);
        out->tilde = c->w_tilde;
        out->expands = c->w_expand_raw != SH_NO_OFF;
        assign = role == ROLE_WORD && c->w_eq;
    } else {
        out->replace = (Span){L->end, L->end};
        out->stem = arena_strdup(L->a, "");
    }
    resolve(L, c, role, assign, out);
    if (L->var) {
        out->pos = YEW_SH_POS_VARIABLE;
        return;
    }
    if (out->pos == YEW_SH_POS_ASSIGN) {
        out->replace = (Span){c->w_eq_raw, L->end};
        out->stem = arena_strndup(L->a,
                                  (const char *)c->dec.data + c->w_eq_dec,
                                  c->dec.len - c->w_eq_dec);
        out->tilde = c->w_tilde_eq;
        out->expands = c->w_expand_raw != SH_NO_OFF &&
                       c->w_expand_raw >= c->w_eq_raw;
    }
}

bool yew_shctx_at(const char *line, size_t len, size_t cursor, Arena *a,
                  YewShCtx *out)
{
    Lexer *L;
    size_t at = 0U;
    u32 i;

    if (out == NULL || a == NULL || (line == NULL && len != 0U) ||
        cursor > len)
        return false;
    (void)memset(out, 0, sizeof(*out));
    /* ~10 KiB of frames: on the heap, not the stack of a keystroke. */
    L = yew_xcalloc(1U, sizeof(*L));
    L->line = line == NULL ? "" : line;
    L->end = cursor;
    L->a = a;
    L->kinds[0] = FR_TOP;
    L->nk = 1U;
    frame_init(&L->cmd[0], FR_TOP);
    L->nc = 1U;
    while (at < L->end && !L->stop) {
        CmdFr *c = cur_cmd(L);
        size_t next;

        if (top_kind(L) == FR_DQ)
            next = lex_dq(L, c, at);
        else if (c->qmode != Q_NONE)
            next = lex_squote(L, c, at);
        else
            next = lex_cmd(L, c, at);
        /* Every branch consumes at least one byte or stops. */
        at = next > at ? next : at + 1U;
    }
    finish(L, out);
    for (i = 0U; i < L->nc; i++)
        frame_dispose(&L->cmd[i]);
    yew_xfree(L);
    return true;
}

/* ---------------------------------------------------------------- */
/* §6: shell quoting                                                  */
/* ---------------------------------------------------------------- */

static bool has_control(const unsigned char *s, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        if (s[i] < 0x20U || s[i] == 0x7FU)
            return true;
    }
    return false;
}

/*
 * The unquoted-state escape set: §6's list, plus three the contract did
 * not name.  `~ # ^` are escaped EVERYWHERE, not only as the first byte:
 * bash expands `~` after `=` in an assignment-shaped argument, and a
 * zsh whose ~/.zshenv sets EXTENDED_GLOB (which `zsh -c` reads) treats
 * all three as glob operators mid-word.  A leading `=` is zsh's command
 * path expansion (`=ls` is /bin/ls).  Escaping one of these needlessly
 * costs a backslash; missing one changes which file `rm` removes.
 */
static bool none_special(unsigned char b, bool first)
{
    static const char always[] = " \t\\'\"$`!*?[](){}<>|&;~#^";

    if (b != '\0' && strchr(always, (int)b) != NULL)
        return true;
    return first && b == '=';
}

char *yew_shq_quote(Arena *a, const char *text, size_t len, YewShQuote q,
                    const char **closing)
{
    const unsigned char *s = (const unsigned char *)text;
    Bytebuf out;
    size_t i;
    char *result;

    if (closing != NULL)
        *closing = "";
    bytebuf_init(&out);
    switch (q) {
    case YEW_SH_Q_NONE:
        if (has_control(s, len)) {
            /* A backslash-newline is a line CONTINUATION and would
             * silently delete the byte; only '…' keeps a control byte. */
            yew_shell_quote(&out, s, len);
            break;
        }
        for (i = 0U; i < len; i++) {
            if (none_special(s[i], i == 0U))
                bytebuf_push_u8(&out, (u8)'\\');
            bytebuf_push_u8(&out, s[i]);
        }
        break;
    case YEW_SH_Q_SINGLE:
        for (i = 0U; i < len; i++) {
            if (s[i] == '\'')
                bytebuf_append(&out, "'\\''", 4U);
            else
                bytebuf_push_u8(&out, s[i]);
        }
        if (closing != NULL)
            *closing = "'";
        break;
    case YEW_SH_Q_DOUBLE:
        for (i = 0U; i < len; i++) {
            if (s[i] == '"' || s[i] == '$' || s[i] == '`' || s[i] == '\\')
                bytebuf_push_u8(&out, (u8)'\\');
            bytebuf_push_u8(&out, s[i]);
        }
        if (closing != NULL)
            *closing = "\"";
        break;
    case YEW_SH_Q_DOLLAR:
    default:
        for (i = 0U; i < len; i++) {
            if (s[i] == '\\' || s[i] == '\'') {
                bytebuf_push_u8(&out, (u8)'\\');
                bytebuf_push_u8(&out, s[i]);
            } else if (s[i] < 0x20U || s[i] == 0x7FU) {
                static const char hex[] = "0123456789abcdef";
                char esc[4];

                /* Always two digits: `\x1` followed by a hex letter
                 * would otherwise read three. */
                esc[0] = '\\';
                esc[1] = 'x';
                esc[2] = hex[s[i] >> 4];
                esc[3] = hex[s[i] & 0x0FU];
                bytebuf_append(&out, esc, 4U);
            } else {
                bytebuf_push_u8(&out, s[i]);
            }
        }
        if (closing != NULL)
            *closing = "'";
        break;
    }
    result = arena_strndup(a, out.data == NULL ? "" : (const char *)out.data,
                           out.len);
    bytebuf_free(&out);
    return result;
}

char *yew_shq_insert(Arena *a, const char *text, size_t len,
                     size_t literal_prefix, YewShQuote q,
                     const char **closing)
{
    Bytebuf out;
    char *rest;
    char *result;

    if (literal_prefix > len)
        literal_prefix = len;
    bytebuf_init(&out);
    if (literal_prefix != 0U) {
        /* `~user/`: the `~` stays bare so the shell expands it; the name
         * and slash are quoted like any other unquoted text (a user name
         * never needs it, but a hostile passwd entry should not be able
         * to smuggle a `$` in). */
        const char *pre = text;
        size_t plen = literal_prefix;

        if (pre[0] == '~') {
            bytebuf_push_u8(&out, (u8)'~');
            pre++;
            plen--;
        }
        if (plen != 0U) {
            char *qp = yew_shq_quote(a, pre, plen, YEW_SH_Q_NONE, NULL);

            bytebuf_append(&out, qp, strlen(qp));
        }
    }
    switch (q) {
    case YEW_SH_Q_SINGLE:
        bytebuf_push_u8(&out, (u8)'\'');
        break;
    case YEW_SH_Q_DOUBLE:
        bytebuf_push_u8(&out, (u8)'"');
        break;
    case YEW_SH_Q_DOLLAR:
        bytebuf_append(&out, "$'", 2U);
        break;
    case YEW_SH_Q_NONE:
    default:
        break;
    }
    rest = yew_shq_quote(a, text + literal_prefix, len - literal_prefix, q,
                         closing);
    bytebuf_append(&out, rest, strlen(rest));
    result = arena_strndup(a, out.data == NULL ? "" : (const char *)out.data,
                           out.len);
    bytebuf_free(&out);
    return result;
}
