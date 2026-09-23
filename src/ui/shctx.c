#define _POSIX_C_SOURCE 200809L

#include "ui/shctx.h"

#include <pwd.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/job.h"
#include "unicode/utf8.h"
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
    W_SYNTH = 1U << 3,   /* stands in for "no command" (`fi > x`)        */
    W_GLOB = 1U << 4,    /* an unquoted * ? [ or { (glob, brace expansion) */
    W_TILDE = 1U << 5    /* a leading tilde prefix the shell expands      */
};

/* Sprint 57.32: the one expansion a directory operand may hold. */
enum {
    VK_NONE,
    VK_HOME,
    VK_PWD,
    VK_OLDPWD,
    VK_OTHER
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
    u8 var;    /* VK_*: what its expansion is, when W_EXPANDS          */
    bool var_q; /* that expansion sat inside "…" (no field splitting)  */
} ShWord;

/*
 * Sprint 57.32 §1: the directory state.
 *
 * A DirV is what one execution path knows about where the shell is:
 * UNREACH (no path gets here -- after `exit`), KNOWN (`path`, relative
 * to the directory `:!` commands start in, or absolute; lexically
 * normalised), or UNKNOWN (data-dependent).  Two paths JOIN to KNOWN
 * only when they agree.  `stk` is the line's own pushd stack, a
 * persistent list so a copy is a pointer copy.
 */
enum {
    DV_UNREACH,
    DV_KNOWN,
    DV_UNKNOWN
};

typedef struct DStack {
    const char *dir; /* NULL: the directory it saved was unknown */
    const struct DStack *next;
} DStack;

typedef struct DirV {
    u8 st;
    bool moved; /* a cd ran on this path: $OLDPWD is the line's, not env's */
    const char *path;
    const DStack *stk;
} DirV;

/* The pipeline connector a completed simple command ended with. */
enum {
    OP_LIST, /* ; newline ;; -- and the frame's end                      */
    OP_AND,
    OP_OR,
    OP_PIPE,
    OP_AMP
};

enum {
    CONN_NONE,
    CONN_AND,
    CONN_OR
};

/*
 * One command list's flow.  `ls`/`lf` are the and-or list so far: the
 * directory on the paths that end with success / failure.  A failure
 * nothing branches on (`cd a; x`) is assumed not to happen -- the next
 * list starts from `ls` -- which is §1's model: only `||`, `else` and
 * `!` make a failure a path anything runs on.
 */
typedef struct Flow {
    DirV list_in;  /* the and-or list's input: what `&` leaves behind    */
    DirV pipe_in;  /* the pipeline being built runs here (every stage)   */
    DirV ls;
    DirV lf;
    DirV last_s;   /* the previous list's ending, for then/else/do        */
    DirV last_f;
    DirV ov_s;     /* the command's outcome when a group decided it       */
    DirV ov_f;
    u32 stages;
    u8 conn;
    bool negate;
    bool override;
} Flow;

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
    /* Sprint 57.32 */
    size_t w_first_q; /* decoded offset of the first quoted byte          */
    u32 w_nexp;       /* expansions in the word                          */
    u8 w_var;         /* VK_* of its one $NAME / ${NAME}                 */
    bool w_var_q;
    bool cmd_started; /* the simple command has a word or redirection    */
    bool fn_pending;  /* `f()` ended a command: the next { or ( is its body */
    bool fn_body;     /* this frame IS a function body (runs later)      */
    Flow fl;
} CmdFr;

typedef struct HereDoc {
    char *delim;
    bool strip_tabs;
} HereDoc;

/*
 * Sprint 57.32: an open compound command.  Its body shares the frame's
 * shell, so it is not a frame; `saved` is the enclosing list's flow,
 * restored at the closing word with the construct's outcome as the
 * command's.
 */
enum {
    CK_IF,
    CK_LOOP,
    CK_CASE,
    CK_FN
};

typedef struct Construct {
    u8 kind;
    u8 loop; /* CK_LOOP: 0 while, 1 until, 2 for/select */
    bool has_else;
    bool dirty; /* CK_CASE: a body names cd, pushd or popd */
    u32 braces; /* CK_FN: `{` words inside the body's first command */
    u32 frame;
    DirV entry;
    DirV acc_s;
    DirV acc_f;
    DirV cond_f; /* the condition's other exit: else/elif, a loop's end */
    Flow saved;
} Construct;

enum {
    /* Total frames, double quotes included.  Two per level is the most
     * a caret can need (`"$("$(…`), plus the top level. */
    SH_STACK_MAX = 2 * YEW_SH_DEPTH_MAX + 2,
    SH_HEREDOC_MAX = 16,
    /* Open compound commands across all frames; past it the directory
     * is unknown rather than guessed. */
    SH_CONSTRUCT_MAX = 32
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
    /* Sprint 57.24 §1: a spec's `precommand` replaces the table row. */
    YewShWrapperLookup wrap_lookup;
    void *wrap_ud;
    /* Sprint 57.32 */
    const YewShEnv *env;
    Construct cons[SH_CONSTRUCT_MAX];
    u32 ncons;
    bool dir_lost;  /* a bound was exceeded: the directory is unknown     */
    bool env_dirty; /* a word named HOME, PWD or CDPATH: they may change  */
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

/* ---------------------------------------------------------------- */
/* Sprint 57.32 §1: directories                                      */
/* ---------------------------------------------------------------- */

/* A pushd stack that stopped meaning one thing: two paths that pushed
 * differently were joined.  popd on it is unknown, as on an empty one. */
static const DStack stk_poison = {NULL, NULL};

static DirV dv_make(u8 st, const char *path)
{
    DirV v;

    v.st = st;
    v.moved = false;
    v.path = path;
    v.stk = NULL;
    return v;
}

static DirV dv_unknown(void)
{
    return dv_make(DV_UNKNOWN, NULL);
}

static DirV dv_unreach(void)
{
    return dv_make(DV_UNREACH, NULL);
}

static bool dv_same(DirV a, DirV b)
{
    if (a.st != b.st)
        return false;
    return a.st != DV_KNOWN || strcmp(a.path, b.path) == 0;
}

static bool stk_equal(const DStack *a, const DStack *b)
{
    while (a != NULL && b != NULL) {
        if (a == &stk_poison || b == &stk_poison)
            return a == b;
        if ((a->dir == NULL) != (b->dir == NULL) ||
            (a->dir != NULL && strcmp(a->dir, b->dir) != 0))
            return false;
        a = a->next;
        b = b->next;
    }
    return a == b;
}

/* Where the shell is when either path may have been taken. */
static DirV dv_join(DirV a, DirV b)
{
    if (a.st == DV_UNREACH)
        return b;
    if (b.st == DV_UNREACH)
        return a;
    if (a.st == DV_KNOWN && b.st == DV_KNOWN && strcmp(a.path, b.path) == 0) {
        a.moved = a.moved || b.moved;
        if (!stk_equal(a.stk, b.stk))
            a.stk = &stk_poison;
        return a;
    }
    return dv_unknown();
}

static void flow_list(Flow *f, DirV in)
{
    f->list_in = in;
    f->pipe_in = in;
    f->ls = dv_unreach();
    f->lf = dv_unreach();
    f->conn = CONN_NONE;
    f->stages = 0U;
    f->negate = false;
}

static void flow_init(Flow *f, DirV in)
{
    flow_list(f, in);
    f->last_s = in;
    f->last_f = in;
    f->override = false;
}

/* The next list's input after `;`: the success path, else (only an
 * `exit` on it) the failure path. */
static DirV flow_pick(DirV s, DirV f)
{
    return s.st != DV_UNREACH ? s : f;
}

/*
 * The directory a command in frame `idx` runs in when its flow says
 * `v`, with every enclosing loop checked: a loop whose body or
 * condition moved the shell runs its second iteration somewhere else
 * (`while cd a; do x` runs x in a, then a/a).  A `{ … }` frame shares
 * its parent's shell, so the parent's loops enclose it too; a subshell
 * was checked when it opened.
 */
static DirV frame_eff_in(const Lexer *L, u32 idx, DirV v)
{
    for (;;) {
        const CmdFr *c = &L->cmd[idx];
        u32 k;

        for (k = 0U; k < L->ncons; k++) {
            if (L->cons[k].frame == idx && L->cons[k].kind == CK_LOOP &&
                !dv_same(L->cons[k].entry, v))
                return dv_unknown();
        }
        if (c->kind != FR_BRACE || c->fn_body || idx == 0U)
            break;
        idx--;
    }
    if (L->dir_lost)
        return dv_unknown();
    return v;
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
    {
        /*
         * Sprint 57.32: a subshell starts with a COPY of its parent's
         * directory and is discarded at its close; a group shares it
         * (copied in, copied back at `}`).  A function body runs when
         * it is called, from wherever the shell is then.
         */
        CmdFr *parent = &L->cmd[L->nc - 2U];
        CmdFr *child = &L->cmd[L->nc - 1U];
        DirV in = frame_eff_in(L, L->nc - 2U, parent->fl.pipe_in);

        if (parent->fn_pending && (kind == FR_BRACE || kind == FR_PAREN)) {
            parent->fn_pending = false;
            child->fn_body = true;
            in = dv_unknown();
        }
        flow_init(&child->fl, in);
    }
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
        /* A frame's open compound commands close with it. */
        while (L->ncons != 0U && L->cons[L->ncons - 1U].frame >= L->nc)
            L->ncons--;
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
    c->words[c->n].var = VK_NONE;
    c->words[c->n].var_q = false;
    c->n++;
}

static void push_synth(Lexer *L, CmdFr *c)
{
    push_word(L, c, arena_strdup(L->a, ""), W_SYNTH);
}

static void word_begin(CmdFr *c, size_t at)
{
    c->cmd_started = true;
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
    c->w_first_q = SH_NO_OFF;
    c->w_nexp = 0U;
    c->w_var = VK_NONE;
    c->w_var_q = false;
}

/* One decoded byte joins the current word.  `raw` is where it came from,
 * for the `=` of an assignment. */
static void word_byte(CmdFr *c, unsigned char b, bool quoted, size_t raw)
{
    size_t before = c->dec.len;

    if (quoted) {
        c->w_flags |= W_QUOTED;
        if (c->w_first_q == SH_NO_OFF)
            c->w_first_q = before;
        c->w_digits = false;
        if (!c->w_eq)
            c->w_name = false;
    } else {
        if (!is_digit(b))
            c->w_digits = false;
        if (b == '*' || b == '?' || b == '[' || b == '{')
            c->w_flags |= W_GLOB;
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
    c->w_nexp++;
}

/*
 * Sprint 57.32: a `$NAME` / `${NAME}` just lexed.  A directory operand
 * may hold ONE expansion, at its start, of a variable whose value is
 * known here -- $HOME, $PWD, $OLDPWD -- and nothing else.
 */
static void word_var(CmdFr *c, const char *name, size_t n, bool in_dq)
{
    u8 kind = VK_OTHER;

    if (c->dec.len == 0U) {
        if (n == 4U && memcmp(name, "HOME", 4U) == 0)
            kind = VK_HOME;
        else if (n == 3U && memcmp(name, "PWD", 3U) == 0)
            kind = VK_PWD;
        else if (n == 6U && memcmp(name, "OLDPWD", 6U) == 0)
            kind = VK_OLDPWD;
    }
    c->w_var = kind;
    c->w_var_q = in_dq;
}

static bool word_is(const char *text, u8 flags, const char *want)
{
    return (flags & (W_QUOTED | W_EXPANDS)) == 0U && strcmp(text, want) == 0;
}

/* ---------------------------------------------------------------- */
/* Sprint 57.32 §1: which commands move the shell                    */
/* ---------------------------------------------------------------- */

static const char *env_get(const Lexer *L, const char *name)
{
    if (L->env == NULL || L->env->get == NULL)
        return NULL;
    return L->env->get(L->env->ud, name);
}

/*
 * Lexical normalisation, the way a LOGICAL cd (the default, -L)
 * resolves a path: `.` and empty components vanish, `..` removes the
 * component before it (at the root it stays the root; a relative path
 * keeps its leading `..`s), no trailing `/`.  "" is "unchanged".
 */
static char *norm_path(Arena *a, const char *s)
{
    Bytebuf b;
    bool abs = s[0] == '/';
    size_t base;
    size_t dots = 0U; /* bytes of leading `..` components (relative) */
    const char *p = s;
    char *out;

    bytebuf_init(&b);
    if (abs)
        bytebuf_push_u8(&b, (u8)'/');
    base = b.len;
    while (*p != '\0') {
        const char *q;
        size_t n;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        q = p;
        while (*q != '\0' && *q != '/')
            q++;
        n = (size_t)(q - p);
        if (n == 1U && p[0] == '.') {
            /* nothing */
        } else if (n == 2U && p[0] == '.' && p[1] == '.') {
            if (b.len > base + dots) {
                size_t cut = b.len;

                while (cut > base + dots && b.data[cut - 1U] != '/')
                    cut--;
                /* `cut` is just past the separator, or at the start of
                 * the poppable region; drop the separator too. */
                if (cut > base + dots)
                    cut--;
                b.len = cut;
            } else if (!abs) {
                if (b.len > base)
                    bytebuf_push_u8(&b, (u8)'/');
                bytebuf_append(&b, "..", 2U);
                dots = b.len - base;
            }
        } else {
            if (b.len > base)
                bytebuf_push_u8(&b, (u8)'/');
            bytebuf_append(&b, p, n);
        }
        p = q;
    }
    out = arena_strndup(a, b.data == NULL ? "" : (const char *)b.data, b.len);
    bytebuf_free(&b);
    return out;
}

char *yew_sh_dir_join(Arena *a, const char *dir, const char *operand)
{
    Bytebuf b;
    char *out;

    if (a == NULL || operand == NULL)
        return NULL;
    if (operand[0] == '/' || dir == NULL || dir[0] == '\0')
        return norm_path(a, operand);
    bytebuf_init(&b);
    bytebuf_append(&b, dir, strlen(dir));
    bytebuf_push_u8(&b, (u8)'/');
    bytebuf_append(&b, operand, strlen(operand) + 1U);
    out = norm_path(a, (const char *)b.data);
    bytebuf_free(&b);
    return out;
}

/* `path` as the filesystem sees it, or NULL when that needs the start
 * directory and the caller gave none. */
static const char *abs_of(const Lexer *L, const char *path)
{
    if (path[0] == '/')
        return path;
    if (L->env == NULL || L->env->base == NULL)
        return NULL;
    return yew_sh_dir_join(L->a, L->env->base, path);
}

/* Any byte the shell would split an unquoted expansion on, or glob. */
static bool splits(const char *s)
{
    return s != NULL && strpbrk(s, " \t\n*?[") != NULL;
}

/* A word the lexer saw no quote, expansion or construct in. */
static bool plain(const ShWord *w, const char *want)
{
    if ((w->flags & (W_QUOTED | W_EXPANDS | W_SYNTH | W_GLOB)) != 0U)
        return false;
    return want == NULL || strcmp(w->text, want) == 0;
}

/*
 * The word as `cd` would receive it: tilde and $HOME / $PWD / $OLDPWD
 * expanded, or NULL when only the shell can know (any other expansion,
 * a glob, an unknown user).  `how`: WD_LITERAL nothing was expanded, so
 * CDPATH may apply; WD_RESOLVED it is $PWD-based and already relative
 * to the start directory (the frame's own `in`), so it is not joined
 * again; WD_EXPANDED otherwise.  `full` false refuses $PWD (a caller
 * that joins the result to a directory of its own) and `~user` (a
 * password-database lookup is too slow for every operand of every
 * keystroke; only a `cd` pays it).
 */
enum {
    WD_EXPANDED,
    WD_LITERAL,
    WD_RESOLVED
};

static const char *word_dir(Lexer *L, const ShWord *w, DirV in, bool full,
                            u8 *how)
{
    const char *home = NULL;

    *how = WD_EXPANDED;
    if ((w->flags & (W_GLOB | W_SYNTH)) != 0U)
        return NULL;
    if ((w->flags & W_EXPANDS) != 0U) {
        const char *val = NULL;

        if ((w->flags & W_TILDE) != 0U)
            return NULL;
        switch (w->var) {
        case VK_HOME:
            val = L->env_dirty ? NULL : env_get(L, "HOME");
            break;
        case VK_OLDPWD:
            val = L->env_dirty || in.moved ? NULL : env_get(L, "OLDPWD");
            break;
        case VK_PWD:
            /* The rest must start a new component: `${PWD}x` names a
             * sibling this representation cannot spell. */
            if (!full || L->env_dirty || in.st != DV_KNOWN ||
                (w->text[0] != '\0' && w->text[0] != '/'))
                return NULL;
            if (!w->var_q && (splits(in.path) ||
                              (in.path[0] != '/' &&
                               (L->env == NULL || splits(L->env->base)))))
                return NULL;
            *how = WD_RESOLVED;
            return yew_sh_dir_join(L->a, in.path,
                                   w->text[0] == '/' ? w->text + 1 : "");
        default:
            return NULL;
        }
        if (val == NULL || val[0] != '/' || (!w->var_q && splits(val)))
            return NULL;
        {
            /* The value, then whatever followed it: `${HOME}x` is a
             * sibling of $HOME, `$HOME/x` a child. */
            size_t nv = strlen(val);
            size_t nt = strlen(w->text);
            char *cat = arena_alloc(L->a, nv + nt + 1U, 1U);

            (void)memcpy(cat, val, nv);
            (void)memcpy(cat + nv, w->text, nt + 1U);
            return norm_path(L->a, cat);
        }
    }
    if ((w->flags & W_TILDE) != 0U) {
        const char *slash = strchr(w->text, '/');
        size_t ulen = slash == NULL ? strlen(w->text + 1)
                                    : (size_t)(slash - (w->text + 1));

        if (ulen == 0U) {
            home = L->env_dirty ? NULL : env_get(L, "HOME");
        } else if (full) {
            char *user = arena_strndup(L->a, w->text + 1, ulen);
            struct passwd *pw = getpwnam(user);

            /* An unknown user leaves `~name` literal in the shell; not
             * worth a guess either way. */
            home = pw == NULL ? NULL : arena_strdup(L->a, pw->pw_dir);
        }
        if (home == NULL || home[0] != '/')
            return NULL;
        return yew_sh_dir_join(L->a, home, slash == NULL ? "" : slash + 1);
    }
    if (w->text[0] == '\0')
        return NULL;
    *how = WD_LITERAL;
    return w->text;
}

static bool dir_exists(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Is CDPATH consulted for this operand?  bash: not for one starting
 * with `/`, nor one whose first component is `.` or `..`. */
static bool cdpath_applies(const char *op)
{
    if (op[0] == '/')
        return false;
    if (op[0] == '.' && (op[1] == '\0' || op[1] == '/'))
        return false;
    return !(op[0] == '.' && op[1] == '.' && (op[2] == '\0' || op[2] == '/'));
}

/* `cd op` from `in`: the directory it lands in, CDPATH included. */
static DirV cd_target(Lexer *L, DirV in, const char *op, u8 how)
{
    DirV out = in;
    const char *cdpath;
    bool literal = how == WD_LITERAL;

    out.moved = true;
    if (op == NULL)
        return dv_unknown();
    if (op[0] == '/' || how == WD_RESOLVED) {
        out.st = DV_KNOWN;
        out.path = norm_path(L->a, op);
        return out;
    }
    if (in.st != DV_KNOWN)
        return dv_unknown();
    cdpath = env_get(L, "CDPATH");
    if (literal && cdpath_applies(op) &&
        (L->env_dirty || (cdpath != NULL && cdpath[0] != '\0'))) {
        const char *p = cdpath;

        if (L->env_dirty || p == NULL)
            return dv_unknown();
        for (;;) {
            const char *colon = strchr(p, ':');
            size_t n = colon == NULL ? strlen(p) : (size_t)(colon - p);
            char *entry = arena_strndup(L->a, p, n);
            /* An empty entry (or `.`) is the current directory; a
             * relative one is relative to it. */
            const char *cand = yew_sh_dir_join(L->a, in.path,
                                               yew_sh_dir_join(L->a, entry,
                                                               op));
            const char *abs = abs_of(L, cand);

            if (abs == NULL)
                return dv_unknown();
            if (dir_exists(abs)) {
                out.path = cand;
                return out;
            }
            if (colon == NULL)
                break;
            p = colon + 1;
        }
    }
    out.path = yew_sh_dir_join(L->a, in.path, op);
    return out;
}

/* After this command, HOME, PWD and CDPATH may not be what the
 * environment said: `HOME=/x; cd`, `unset CDPATH`, `read PWD`. */
static void env_taint(Lexer *L, const CmdFr *c)
{
    u32 k;

    for (k = 0U; k < c->n; k++) {
        const char *t = c->words[k].text;

        if (strstr(t, "HOME") != NULL || strstr(t, "PWD") != NULL ||
            strstr(t, "CDPATH") != NULL)
            L->env_dirty = true;
    }
}

/* `cd [-L|-P]... [--] [dir]`, `pushd dir`, `popd` from `in` (words
 * w[k .. n) are the arguments): the success path's directory. */
static DirV cd_effect(Lexer *L, const char *name, const ShWord *w, u32 k,
                      u32 n, DirV in)
{
    u8 how = WD_EXPANDED;
    DirV s;

    if (strcmp(name, "popd") == 0) {
        /* The line's own stack only: what the shell pushed before the
         * line is invisible. */
        if (k != n || in.stk == NULL || in.stk == &stk_poison ||
            in.stk->dir == NULL)
            return dv_unknown();
        s = in;
        s.st = DV_KNOWN;
        s.path = in.stk->dir;
        s.stk = in.stk->next;
        s.moved = true;
        return s;
    }
    if (strcmp(name, "pushd") == 0) {
        DStack *top;

        /* No operand swaps the shell's stack; `-n`, `+N`, `-N` are
         * about the stack, not a directory. */
        if (k + 1U != n || w[k].text[0] == '-' || w[k].text[0] == '+')
            return dv_unknown();
        s = cd_target(L, in, word_dir(L, &w[k], in, true, &how), how);
        if (s.st != DV_KNOWN)
            return s;
        top = arena_alloc(L->a, sizeof(*top), sizeof(void *));
        top->dir = in.st == DV_KNOWN ? in.path : NULL;
        top->next = in.stk;
        s.stk = top;
        return s;
    }
    while (k < n) {
        const char *t = w[k].text;

        if (!plain(&w[k], NULL) || t[0] != '-' || t[1] == '\0')
            break;
        k++;
        if (strcmp(t, "--") == 0)
            break;
        /* -L and -P are the lexical cd modelled here (§1); -e, -@ are
         * not. */
        if (strspn(t + 1, "LP") != strlen(t + 1))
            return dv_unknown();
    }
    if (k == n) {
        const char *home = L->env_dirty ? NULL : env_get(L, "HOME");

        if (home == NULL || home[0] != '/')
            return dv_unknown();
        return cd_target(L, in, home, WD_EXPANDED);
    }
    /* `cd -` is $OLDPWD from before the line; zsh's `cd old new`
     * substitutes into $PWD. */
    if (k + 1U != n || plain(&w[k], "-"))
        return dv_unknown();
    return cd_target(L, in, word_dir(L, &w[k], in, true, &how), how);
}

/*
 * §1's first table: a completed simple command, run from `in`, ends on
 * `*s` (success) or `*f` (failure).  `cd`, `pushd`, `popd` move the
 * success path; `exit` and `return` end both, whatever their arguments.
 */
static void cmd_effect(Lexer *L, const CmdFr *c, DirV in, DirV *s, DirV *f)
{
    const ShWord *w = c->words;
    u32 n = c->n;
    u32 i = 0U;
    bool assigns = false;

    *s = in;
    *f = in;
    while (i < n && (w[i].flags & W_ASSIGN) != 0U) {
        assigns = true;
        i++;
    }
    while (i < n && (plain(&w[i], "builtin") || plain(&w[i], "command")))
        i++;
    if (i < n && (plain(&w[i], "exit") || plain(&w[i], "return"))) {
        *s = dv_unreach();
        *f = dv_unreach();
    } else if (i < n && in.st != DV_UNREACH &&
               (plain(&w[i], "cd") || plain(&w[i], "pushd") ||
                plain(&w[i], "popd"))) {
        /* A temporary `CDPATH=… cd x` or `HOME=… cd` changes the
         * answer; not worth modelling. */
        *s = assigns ? dv_unknown()
                     : cd_effect(L, w[i].text, w, i + 1U, n, in);
    }
    env_taint(L, c);
}

/*
 * §1's second table: a simple command ended with connector `op`.  See
 * Flow for the model.  An empty command (`&&` at a line's end, a
 * newline right after `then`) changes nothing: the list goes on.
 */
static void cmd_done(Lexer *L, CmdFr *c, u8 op)
{
    Flow *fl = &c->fl;
    DirV s;
    DirV f;

    if (!c->cmd_started && c->n == 0U && !fl->override)
        return;
    c->cmd_started = false;
    cmd_effect(L, c, fl->pipe_in, &s, &f);
    if (fl->override) {
        s = fl->ov_s;
        f = fl->ov_f;
        fl->override = false;
    }
    if (op == OP_PIPE) {
        /* Every stage runs in a subshell from the pipeline's input. */
        fl->stages++;
        return;
    }
    if (fl->stages != 0U) {
        /* The last stage: a subshell in bash, the current shell in zsh.
         * Only an answer both agree on survives. */
        s = dv_join(fl->pipe_in, s);
        f = dv_join(fl->pipe_in, f);
    }
    if (fl->negate) {
        s = dv_join(s, f);
        f = s;
    }
    fl->stages = 0U;
    fl->negate = false;
    switch (fl->conn) {
    case CONN_AND:
        fl->ls = s;
        fl->lf = dv_join(fl->lf, f);
        break;
    case CONN_OR:
        fl->ls = dv_join(fl->ls, s);
        fl->lf = f;
        break;
    default:
        fl->ls = s;
        fl->lf = f;
        break;
    }
    switch (op) {
    case OP_AND:
        fl->conn = CONN_AND;
        fl->pipe_in = fl->ls;
        break;
    case OP_OR:
        fl->conn = CONN_OR;
        fl->pipe_in = fl->lf;
        break;
    case OP_AMP:
        /* The whole list ran in a background subshell. */
        fl->last_s = fl->list_in;
        fl->last_f = fl->list_in;
        flow_list(fl, fl->list_in);
        break;
    default:
        fl->last_s = fl->ls;
        fl->last_f = fl->lf;
        flow_list(fl, flow_pick(fl->ls, fl->lf));
        break;
    }
}

static Construct *con_top(Lexer *L, u8 kind)
{
    Construct *k;

    if (L->ncons == 0U)
        return NULL;
    k = &L->cons[L->ncons - 1U];
    return k->frame == L->nc - 1U && k->kind == kind ? k : NULL;
}

/* A compound command opens: its body is a list of its own, in the same
 * shell, starting where the command would run. */
static void con_open(Lexer *L, CmdFr *c, u8 kind, u8 loop)
{
    Construct *k;

    if (L->ncons >= SH_CONSTRUCT_MAX) {
        L->dir_lost = true;
        return;
    }
    k = &L->cons[L->ncons++];
    (void)memset(k, 0, sizeof(*k));
    k->kind = kind;
    k->loop = loop;
    k->frame = L->nc - 1U;
    k->entry = c->fl.pipe_in;
    k->acc_s = dv_unreach();
    k->acc_f = dv_unreach();
    k->cond_f = dv_unreach();
    k->saved = c->fl;
    flow_init(&c->fl, kind == CK_FN ? dv_unknown() : k->entry);
}

/* It closes: the enclosing list resumes, and this command's outcome is
 * (s, f).  A function definition runs nothing. */
static void con_close(Lexer *L, CmdFr *c, DirV s, DirV f)
{
    Construct *k = &L->cons[--L->ncons];

    c->fl = k->saved;
    if (k->kind == CK_FN)
        return;
    c->fl.override = true;
    c->fl.ov_s = s;
    c->fl.ov_f = f;
}

/* A reserved word at command position moves the flow. */
static void flow_reserved(Lexer *L, CmdFr *c, const char *t)
{
    Construct *k;

    c->cmd_started = false;
    if (strcmp(t, "if") == 0) {
        con_open(L, c, CK_IF, 0U);
    } else if (strcmp(t, "while") == 0 || strcmp(t, "until") == 0) {
        con_open(L, c, CK_LOOP, t[0] == 'u' ? 1U : 0U);
    } else if (strcmp(t, "!") == 0) {
        c->fl.negate = true;
    } else if (strcmp(t, "then") == 0) {
        if ((k = con_top(L, CK_IF)) != NULL) {
            k->cond_f = c->fl.last_f;
            flow_init(&c->fl, c->fl.last_s);
        }
    } else if (strcmp(t, "elif") == 0 || strcmp(t, "else") == 0) {
        if ((k = con_top(L, CK_IF)) != NULL) {
            k->acc_s = dv_join(k->acc_s, c->fl.last_s);
            k->acc_f = dv_join(k->acc_f, c->fl.last_f);
            k->has_else = t[2] == 's';
            flow_init(&c->fl, k->cond_f);
        }
    } else if (strcmp(t, "do") == 0) {
        if ((k = con_top(L, CK_LOOP)) != NULL) {
            DirV body;

            if (k->loop == 1U) {
                body = c->fl.last_f;
                k->cond_f = c->fl.last_s;
            } else if (k->loop == 0U) {
                body = c->fl.last_s;
                k->cond_f = c->fl.last_f;
            } else {
                body = flow_pick(c->fl.last_s, c->fl.last_f);
                k->cond_f = body;
            }
            flow_init(&c->fl, body);
        }
    }
}

/* `fi`, `done`, `esac` (before the lexer's after_construct). */
static void flow_closer(Lexer *L, CmdFr *c, const char *t)
{
    Construct *k;

    if (strcmp(t, "fi") == 0 && (k = con_top(L, CK_IF)) != NULL) {
        DirV s = dv_join(k->acc_s, c->fl.last_s);
        DirV f = dv_join(k->acc_f, c->fl.last_f);

        if (!k->has_else)
            s = dv_join(s, k->cond_f); /* no branch ran: status 0 */
        con_close(L, c, s, f);
    } else if (strcmp(t, "done") == 0 && (k = con_top(L, CK_LOOP)) != NULL) {
        DirV end = flow_pick(c->fl.last_s, c->fl.last_f);
        DirV out = dv_same(end, k->entry) &&
                           (k->cond_f.st == DV_UNREACH ||
                            dv_same(k->cond_f, k->entry))
                       ? k->entry
                       : dv_unknown();

        con_close(L, c, out, out);
    } else if (strcmp(t, "esac") == 0 &&
               (k = con_top(L, CK_CASE)) != NULL) {
        DirV out = k->dirty ? dv_unknown() : k->entry;

        con_close(L, c, out, out);
    }
}

/* `{` ending [NAME, ()] or [function, NAME, (()] is a function body. */
static bool fn_header(const CmdFr *c)
{
    const ShWord *w = c->words;

    if (c->n == 2U && plain(&w[1], "()"))
        return true;
    return (c->n == 2U || (c->n == 3U && plain(&w[2], "()"))) &&
           plain(&w[0], "function");
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
    u8 var = VK_NONE;

    if (!c->in_word)
        return cur_cmd(L);
    c->in_word = false;
    c->qmode = Q_NONE;
    text = arena_strndup(L->a, (const char *)c->dec.data, c->dec.len);
    flags = c->w_flags;
    if (c->w_eq)
        flags |= W_ASSIGN;
    if (c->w_tilde) {
        /* Sprint 57.32: the shell expands `~name/` only when the prefix
         * up to the first `/` is all unquoted. */
        const char *slash = strchr(text, '/');
        size_t prefix = slash == NULL ? strlen(text) : (size_t)(slash - text);

        if (c->w_first_q == SH_NO_OFF || c->w_first_q >= prefix)
            flags |= W_TILDE;
    }
    if (c->w_nexp != 0U) {
        var = c->w_nexp == 1U ? c->w_var : VK_OTHER;
        if (var == VK_NONE)
            var = VK_OTHER;
    }
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
                flow_closer(L, c, "esac");
                after_construct(L, c);
            }
        } else if (word_is(text, flags, "cd") ||
                   word_is(text, flags, "pushd") ||
                   word_is(text, flags, "popd")) {
            /* Sprint 57.32: a case body is not lexed as commands; any
             * branch that may move the shell makes the rest unknown. */
            Construct *k = con_top(L, CK_CASE);

            if (k != NULL)
                k->dirty = true;
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
        bool pending = c->fn_pending;

        c->fn_pending = false;
        if (opens_command(text)) {
            flow_reserved(L, c, text);
            return cur_cmd(L);
        }
        if (strcmp(text, "{") == 0) {
            c->fn_pending = pending; /* push_cmd reads it */
            c->cmd_started = false;
            (void)push_cmd(L, FR_BRACE);
            c->fn_pending = false;
            return cur_cmd(L);
        }
        if (strcmp(text, "}") == 0) {
            Construct *fn = con_top(L, CK_FN);

            if (fn != NULL && fn->braces != 0U) {
                /* Closes a `{` word of the body's first command. */
                fn->braces--;
                after_construct(L, c);
            } else if (fn != NULL) {
                /* The end of a `f() { … }` body lexed in this frame. */
                con_close(L, c, dv_unknown(), dv_unknown());
                after_construct(L, c);
            } else if (top_kind(L) == FR_BRACE) {
                /* Sprint 57.32: a group shares the shell -- its last
                 * list's ending is the command's outcome. */
                DirV s = c->fl.last_s;
                DirV f = c->fl.last_f;
                bool fn = c->fn_body;
                CmdFr *parent;

                pop_frame(L);
                parent = cur_cmd(L);
                after_construct(L, parent);
                if (!fn) {
                    parent->fl.override = true;
                    parent->fl.ov_s = s;
                    parent->fl.ov_f = f;
                }
            } else {
                after_construct(L, c);
            }
            return cur_cmd(L);
        }
        if (strcmp(text, "fi") == 0 || strcmp(text, "done") == 0 ||
            strcmp(text, "esac") == 0) {
            flow_closer(L, c, text);
            after_construct(L, c);
            return cur_cmd(L);
        }
        if (strcmp(text, "for") == 0 || strcmp(text, "select") == 0) {
            c->forst = FOR_NAME;
            c->cmd_started = false;
            con_open(L, c, CK_LOOP, 2U);
            return cur_cmd(L);
        }
        if (strcmp(text, "case") == 0) {
            c->casest = CASE_WORD;
            c->cmd_started = false;
            con_open(L, c, CK_CASE, 0U);
            push_synth(L, c);
            return cur_cmd(L);
        }
        if (strcmp(text, "[[") == 0)
            c->dbracket = true;
    }
    if (word_is(text, flags, "{") && con_top(L, CK_FN) != NULL)
        con_top(L, CK_FN)->braces++;
    else if (word_is(text, flags, "{") && fn_header(c)) {
        /* `f() { body; }` on one line: the body's words are lexed in
         * this frame, and run only when f is called. */
        push_word(L, c, text, flags);
        con_open(L, c, CK_FN, 0U);
        return cur_cmd(L);
    }
    push_word(L, c, text, flags);
    c->words[c->n - 1U].var = var;
    c->words[c->n - 1U].var_q = c->w_var_q;
    return cur_cmd(L);
}

/* A control operator or newline: the simple command is over. */
static CmdFr *end_cmd(Lexer *L, CmdFr *c, u8 op)
{
    c = word_end(L, c);
    /* Sprint 57.32: `f()` alone -- its body is the next compound. */
    c->fn_pending = c->n != 0U && fn_header(c);
    cmd_done(L, c, op);
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
            if (L->line[j] == '}' && j > at + 2U)
                word_var(c, L->line + at + 2U, j - (at + 2U), in_dq);
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
        word_var(c, L->line + at + 1U, j - (at + 1U), in_dq);
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

    c->cmd_started = true;
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
        end_cmd(L, c, OP_LIST);
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
        end_cmd(L, c, n1 == '|' ? OP_OR : OP_PIPE);
        return n1 == '|' || n1 == '&' ? at + 2U : at + 1U;
    case '&':
        if (n1 == '>')
            return lex_redirect(L, c, at);
        if (c->dbracket && n1 == '&') {
            dbracket_word(L, c, "&&");
            return at + 2U;
        }
        end_cmd(L, c, n1 == '&' ? OP_AND : OP_AMP);
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
        end_cmd(L, c, OP_LIST);
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
        /* Sprint 57.32: `f() ( body )` defines, it does not run. */
        if (fn_header(c))
            c->fn_pending = true;
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
    u32 operands;                /* timeout: then ONE duration word        */
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
    {"sudo", sudo_flags, 0U, true},
    {"doas", doas_flags, 0U, false},
    {"env", env_flags, 0U, true},
    {"nice", nice_flags, 0U, false},
    {"timeout", timeout_flags, 1U, false},
    {"xargs", xargs_flags, 0U, false},
    {"nohup", no_flags, 0U, false},
    {"time", no_flags, 0U, false},
    {"command", no_flags, 0U, false},
    {"builtin", no_flags, 0U, false},
    {"exec", no_flags, 0U, false},
    {"caffeinate", no_flags, 0U, false},
    {"unbuffer", no_flags, 0U, false}
};

/*
 * Sprint 57.24 §1: a completion spec's `precommand` REPLACES this
 * table's row for its command (and a spec without one un-wraps it --
 * whole-file replace).  The lookup answers 1 (a wrapper, `out` filled),
 * 0 (not one) or -1 (no opinion: use the table).  `scratch` holds the
 * answer so the caller can keep a pointer to it for one word.
 */
static const Wrapper *wrapper_of(const Lexer *L, const ShWord *w,
                                 Wrapper *scratch)
{
    size_t i;

    if ((w->flags & (W_QUOTED | W_EXPANDS | W_SYNTH)) != 0U)
        return NULL;
    if (L->wrap_lookup != NULL) {
        YewShWrapper got;
        int verdict;

        (void)memset(&got, 0, sizeof(got));
        verdict = L->wrap_lookup(L->wrap_ud, w->text, &got);
        if (verdict == 0)
            return NULL;
        if (verdict > 0) {
            static const char *const none[] = {NULL};

            scratch->name = w->text;
            scratch->consumes = got.consumes == NULL ? none : got.consumes;
            scratch->operands = got.operands;
            scratch->skips_assign = got.skips_assign;
            return scratch;
        }
    }
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
static u32 command_index(const Lexer *L, const ShWord *w, u32 n,
                         CaretRole role, bool caret_flag)
{
    u32 i = skip_assign(w, 0U, n);

    for (;;) {
        Wrapper scratch;
        const Wrapper *wr;
        u32 j;
        u32 k;
        bool ended = false;

        if (i >= n)
            return n;
        wr = wrapper_of(L, &w[i], &scratch);
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
            if (wr->operands != 0U)
                return i;
            return n;
        }
        for (k = 0U; k < wr->operands; k++) {
            if (j >= n)
                return i; /* the caret is one of the wrapper's operands */
            j++;
        }
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
    u32 cmd = command_index(L, w, n, role, caret_flag);
    u32 k;

    if (role == ROLE_WORD && cmd == n) {
        out->pos = assign ? YEW_SH_POS_ASSIGN : YEW_SH_POS_COMMAND;
        out->argc = 0U;
        out->arg_index = 0U;
        out->argv = arena_alloc(L->a, sizeof(char *), sizeof(void *));
        out->argv[0] = NULL;
        out->dirv = arena_alloc(L->a, sizeof(char *), sizeof(void *));
        out->dirv[0] = NULL;
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
        out->dirv = arena_alloc(L->a, 3U * sizeof(char *), sizeof(void *));
        out->dirv[0] = NULL;
        out->dirv[1] = NULL;
        out->dirv[2] = NULL;
        return;
    }
    out->arg_index = n - cmd;
    out->argc = out->arg_index + 1U;
    out->argv = arena_alloc(L->a, ((size_t)out->argc + 1U) * sizeof(char *),
                            sizeof(void *));
    out->dirv = arena_alloc(L->a, ((size_t)out->argc + 1U) * sizeof(char *),
                            sizeof(void *));
    for (k = cmd; k < n; k++) {
        u8 how;
        const char *d = word_dir(L, &w[k], dv_unknown(), false, &how);

        out->argv[k - cmd] = w[k].text;
        out->dirv[k - cmd] = (char *)d;
    }
    out->argv[out->arg_index] = out->stem;
    out->argv[out->argc] = NULL;
    out->dirv[out->arg_index] = NULL;
    out->dirv[out->argc] = NULL;
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
    out->dirv = arena_alloc(L->a, sizeof(char *), sizeof(void *));
    out->dirv[0] = NULL;
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
    {
        /* Sprint 57.32 §1: where the caret's command will run. */
        DirV v = frame_eff_in(L, L->nc - 1U, c->fl.pipe_in);

        out->cwd_known = v.st == DV_KNOWN;
        out->cwd = arena_strdup(L->a, out->cwd_known ? v.path : "");
    }
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
    return yew_shctx_at_with(line, len, cursor, a, NULL, NULL, out);
}

bool yew_shctx_at_with(const char *line, size_t len, size_t cursor,
                       Arena *a, YewShWrapperLookup lookup, void *ud,
                       YewShCtx *out)
{
    return yew_shctx_at_env(line, len, cursor, a, lookup, ud, NULL, out);
}

bool yew_shctx_at_env(const char *line, size_t len, size_t cursor, Arena *a,
                      YewShWrapperLookup lookup, void *ud,
                      const YewShEnv *env, YewShCtx *out)
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
    L->wrap_lookup = lookup;
    L->wrap_ud = ud;
    L->env = env;
    L->kinds[0] = FR_TOP;
    L->nk = 1U;
    frame_init(&L->cmd[0], FR_TOP);
    flow_init(&L->cmd[0].fl, dv_make(DV_KNOWN, ""));
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

/*
 * Must this word be single-quoted whole rather than backslash-escaped?
 *
 * A control byte: yes, because a backslash-newline is a line continuation
 * and deletes the byte.  Invalid UTF-8: yes, because macOS 14's bash 3.2
 * (also its /bin/sh) truncates an UNQUOTED word at the first invalid
 * sequence under a UTF-8 locale -- CI's round trip got `20 3e 25` back
 * for `20 3e 25 a0 ed 2d 40` -- while the same bytes inside '…' survive
 * on that same shell.  A truncated word names a different file.  macOS
 * 26's bash does not have the bug, so it cannot be reproduced there;
 * the CI lane on the older image is what caught it.
 */
static bool needs_single_quote(const unsigned char *s, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        if (s[i] < 0x20U || s[i] == 0x7FU)
            return true;
    }
    return yew_utf8_validate((const u8 *)s, len) != len;
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
        if (needs_single_quote(s, len)) {
            /* See needs_single_quote: '…' is the one form that keeps a
             * control byte or an invalid UTF-8 sequence intact. */
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

char *yew_shctx_dir(const YewShCtx *ctx, const char *base, Arena *a)
{
    if (ctx == NULL || a == NULL || !ctx->cwd_known || ctx->cwd == NULL)
        return NULL;
    if (base == NULL || base[0] == '\0')
        base = ".";
    if (ctx->cwd[0] == '\0')
        return arena_strdup(a, base);
    {
        char *out = yew_sh_dir_join(a, base, ctx->cwd);

        return out[0] == '\0' ? arena_strdup(a, ".") : out;
    }
}
