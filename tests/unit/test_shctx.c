/*
 * Sprint 57.23: the shell completion context engine -- the corpus, the
 * lexer's totality, and the proof that §6's quoting is right against
 * real shells.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/job.h"
#include "shctx_corpus.h"
#include "ui/shctx.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/secret.h"

static const char caret_mark[] = "\xE2\x80\xB8"; /* ‸ */

/* Split a corpus input at its caret marker into (line, caret). */
static char *corpus_line(const char *in, size_t *len, size_t *caret)
{
    const char *mark = strstr(in, caret_mark);
    size_t n = strlen(in);
    size_t mlen = sizeof(caret_mark) - 1U;
    char *line = yew_xmalloc(n + 1U);

    if (mark == NULL) {
        (void)memcpy(line, in, n + 1U);
        *len = n;
        *caret = n;
        return line;
    }
    *caret = (size_t)(mark - in);
    (void)memcpy(line, in, *caret);
    (void)memcpy(line + *caret, mark + mlen, n - *caret - mlen);
    *len = n - mlen;
    line[*len] = '\0';
    return line;
}

static const char *pos_name(u32 pos)
{
    static const char *const names[] = {"COMMAND", "ARGUMENT", "REDIRECT",
                                        "ASSIGN", "VARIABLE", "NONE"};

    return pos < YEW_ARRAY_LEN(names) ? names[pos] : "?";
}

/* Loud on mismatch: which row, and every field the row pins. */
static bool corpus_row_ok(size_t index, const ShCorpusRow *row,
                          const char *line, size_t caret,
                          const YewShCtx *ctx)
{
    size_t raw_len = strlen(row->raw);
    bool ok = true;

    if (ctx->pos != (YewShPos)row->pos || ctx->quote != (YewShQuote)row->quote)
        ok = false;
    if (ctx->replace.hi != caret || caret < raw_len ||
        ctx->replace.lo != caret - raw_len ||
        memcmp(line + ctx->replace.lo, row->raw, raw_len) != 0)
        ok = false;
    if (ctx->stem == NULL || strcmp(ctx->stem, row->stem) != 0)
        ok = false;
    if (row->argv0 == NULL) {
        if (ctx->argc != 0U || ctx->arg_index != 0U)
            ok = false;
    } else if (ctx->argc == 0U || ctx->argv == NULL ||
               strcmp(ctx->argv[0], row->argv0) != 0 ||
               ctx->arg_index != row->arg_index ||
               ctx->argc != row->arg_index + 1U ||
               strcmp(ctx->argv[ctx->arg_index], ctx->stem) != 0) {
        ok = false;
    }
    if (ctx->dashdash != ((row->flags & SC_DASHDASH) != 0U) ||
        ctx->brace_var != ((row->flags & SC_BRACE) != 0U) ||
        ctx->tilde != ((row->flags & SC_TILDE) != 0U) ||
        ctx->expands != ((row->flags & SC_EXPANDS) != 0U) ||
        ctx->depth != row->depth)
        ok = false;
    if (!ok)
        (void)fprintf(stderr,
                      "shctx corpus row %zu `%s`: pos=%s quote=%u "
                      "replace=[%llu,%llu) stem=`%s` argc=%u arg_index=%u "
                      "argv0=`%s` dashdash=%d brace=%d tilde=%d expands=%d "
                      "depth=%u\n",
                      index, row->in, pos_name((u32)ctx->pos),
                      (unsigned)ctx->quote,
                      (unsigned long long)ctx->replace.lo,
                      (unsigned long long)ctx->replace.hi,
                      ctx->stem == NULL ? "(null)" : ctx->stem,
                      (unsigned)ctx->argc, (unsigned)ctx->arg_index,
                      ctx->argc == 0U ? "(none)" : ctx->argv[0],
                      ctx->dashdash, ctx->brace_var, ctx->tilde,
                      ctx->expands, (unsigned)ctx->depth);
    return ok;
}

/* DoD 2: ≥ 150 rows, each pinning pos, quote, replace, stem, argv. */
void test_shctx_corpus_rows(void)
{
    Arena a;
    size_t i;
    size_t failures = 0U;

    YEW_ASSERT(YEW_ARRAY_LEN(sh_corpus) >= 150U);
    arena_init(&a);
    for (i = 0U; i < YEW_ARRAY_LEN(sh_corpus); i++) {
        size_t len;
        size_t caret;
        char *line = corpus_line(sh_corpus[i].in, &len, &caret);
        YewShCtx ctx;

        YEW_ASSERT(yew_shctx_at(line, len, caret, &a, &ctx));
        if (!corpus_row_ok(i, &sh_corpus[i], line, caret, &ctx))
            failures++;
        yew_xfree(line);
    }
    arena_free_all(&a);
    YEW_ASSERT_EQ_U64((u64)failures, 0U);
}

/*
 * §1: the lexer runs to the CARET.  Whatever follows it -- an
 * unterminated quote, an operator, a substitution -- cannot change the
 * caret's context.
 */
void test_shctx_text_after_caret_is_ignored(void)
{
    static const char *const tails[] = {"'", "\"", " | x", "$(", "`",
                                        "\\", "# c", ") ;", "\n}"};
    Arena a;
    size_t i;
    size_t t;

    arena_init(&a);
    for (i = 0U; i < YEW_ARRAY_LEN(sh_corpus); i++) {
        size_t len;
        size_t caret;
        char *line = corpus_line(sh_corpus[i].in, &len, &caret);
        YewShCtx base;

        YEW_ASSERT(yew_shctx_at(line, len, caret, &a, &base));
        for (t = 0U; t < YEW_ARRAY_LEN(tails); t++) {
            size_t tl = strlen(tails[t]);
            char *mixed = yew_xmalloc(len + tl + 1U);
            YewShCtx ctx;

            (void)memcpy(mixed, line, caret);
            (void)memcpy(mixed + caret, tails[t], tl);
            (void)memcpy(mixed + caret + tl, line + caret, len - caret + 1U);
            YEW_ASSERT(yew_shctx_at(mixed, len + tl, caret, &a, &ctx));
            YEW_ASSERT_EQ_U64(ctx.pos, base.pos);
            YEW_ASSERT_EQ_U64(ctx.quote, base.quote);
            YEW_ASSERT_EQ_U64(ctx.replace.lo, base.replace.lo);
            YEW_ASSERT_EQ_U64(ctx.replace.hi, base.replace.hi);
            YEW_ASSERT_EQ_STR(ctx.stem, base.stem);
            YEW_ASSERT_EQ_U64(ctx.arg_index, base.arg_index);
            yew_xfree(mixed);
        }
        yew_xfree(line);
    }
    arena_free_all(&a);
}

/* §1 pitfall: the nesting stack is bounded at 64, and past it the lexer
 * says NONE rather than growing. */
void test_shctx_depth_is_capped(void)
{
    Arena a;
    Bytebuf line;
    YewShCtx ctx;
    u32 i;

    arena_init(&a);
    bytebuf_init(&line);
    for (i = 0U; i < YEW_SH_DEPTH_MAX; i++)
        bytebuf_append(&line, "$(", 2U);
    bytebuf_append(&line, "gi", 2U);
    YEW_ASSERT(yew_shctx_at((const char *)line.data, line.len, line.len, &a,
                            &ctx));
    YEW_ASSERT_EQ_U64(ctx.depth, YEW_SH_DEPTH_MAX);
    YEW_ASSERT_EQ_U64(ctx.pos, YEW_SH_POS_COMMAND);
    YEW_ASSERT_EQ_STR(ctx.stem, "gi");

    line.len = 0U;
    for (i = 0U; i < 4U * YEW_SH_DEPTH_MAX; i++)
        bytebuf_append(&line, i % 2U == 0U ? "$(" : "\"", i % 2U == 0U ? 2U
                                                                        : 1U);
    YEW_ASSERT(yew_shctx_at((const char *)line.data, line.len, line.len, &a,
                            &ctx));
    YEW_ASSERT_EQ_U64(ctx.pos, YEW_SH_POS_NONE);
    YEW_ASSERT(ctx.depth <= YEW_SH_DEPTH_MAX);

    line.len = 0U;
    for (i = 0U; i < 1000U; i++)
        bytebuf_push_u8(&line, (u8)'(');
    YEW_ASSERT(yew_shctx_at((const char *)line.data, line.len, line.len, &a,
                            &ctx));
    YEW_ASSERT_EQ_U64(ctx.pos, YEW_SH_POS_NONE);
    YEW_ASSERT(ctx.depth <= YEW_SH_DEPTH_MAX);
    bytebuf_free(&line);
    arena_free_all(&a);
}

static u64 rng_next(u64 *s)
{
    u64 x = *s;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/*
 * "Never fails on any byte sequence": every caret of every seeded line,
 * including NUL and invalid UTF-8, answers, and the answer obeys the
 * fuzz harness's invariants.
 */
void test_shctx_every_caret_of_random_bytes(void)
{
    static const char alphabet[] = "abc -$(){}[]'\"`\\|&;<>#=~\n\t!*?21";
    Arena a;
    u64 seed = UINT64_C(0x5723AB12CD34EF56);
    u32 round;

    arena_init(&a);
    for (round = 0U; round < 400U; round++) {
        char line[48];
        size_t len = (size_t)(rng_next(&seed) % sizeof(line));
        size_t i;
        size_t caret;

        for (i = 0U; i < len; i++) {
            u64 r = rng_next(&seed);

            line[i] = (r & 7U) == 0U
                          ? (char)(unsigned char)(r >> 8)
                          : alphabet[(r >> 8) % (sizeof(alphabet) - 1U)];
        }
        for (caret = 0U; caret <= len; caret++) {
            YewShCtx ctx;

            YEW_ASSERT(yew_shctx_at(line, len, caret, &a, &ctx));
            YEW_ASSERT(ctx.replace.lo <= ctx.replace.hi);
            YEW_ASSERT_EQ_U64(ctx.replace.hi, caret);
            YEW_ASSERT(ctx.arg_index <= ctx.argc);
            YEW_ASSERT(ctx.depth <= YEW_SH_DEPTH_MAX);
            YEW_ASSERT_NOT_NULL(ctx.stem);
            YEW_ASSERT_NOT_NULL(ctx.argv);
            YEW_ASSERT_NULL(ctx.argv[ctx.argc]);
        }
        arena_free_all(&a);
    }
    {
        YewShCtx ctx;

        YEW_ASSERT(!yew_shctx_at("ls", 2U, 3U, &a, &ctx));
        YEW_ASSERT(yew_shctx_at(NULL, 0U, 0U, &a, &ctx));
        YEW_ASSERT_EQ_U64(ctx.pos, YEW_SH_POS_COMMAND);
    }
    arena_free_all(&a);
}

/* §6's table, row by row, on bytes that exercise each rule. */
void test_shq_quote_rules(void)
{
    static const struct {
        const char *text;
        YewShQuote q;
        const char *want;
        const char *closing;
    } rows[] = {
        {"plain", YEW_SH_Q_NONE, "plain", ""},
        {"a b", YEW_SH_Q_NONE, "a\\ b", ""},
        {"$HOME", YEW_SH_Q_NONE, "\\$HOME", ""},
        {"a*b", YEW_SH_Q_NONE, "a\\*b", ""},
        {"a$b c", YEW_SH_Q_NONE, "a\\$b\\ c", ""},
        {"#x", YEW_SH_Q_NONE, "\\#x", ""},
        {"~x", YEW_SH_Q_NONE, "\\~x", ""},
        {"a=~b", YEW_SH_Q_NONE, "a=\\~b", ""},
        {"=ls", YEW_SH_Q_NONE, "\\=ls", ""},
        {"it's", YEW_SH_Q_NONE, "it\\'s", ""},
        {"a\nb", YEW_SH_Q_NONE, "'a\nb'", ""},
        {"it's\t", YEW_SH_Q_NONE, "'it'\\''s\t'", ""},
        {"\xff\xfe", YEW_SH_Q_NONE, "\xff\xfe", ""},
        {"it's", YEW_SH_Q_SINGLE, "it'\\''s", "'"},
        {"$x `y` \\z", YEW_SH_Q_SINGLE, "$x `y` \\z", "'"},
        {"a\"$`\\b", YEW_SH_Q_DOUBLE, "a\\\"\\$\\`\\\\b", "\""},
        {"it's", YEW_SH_Q_DOUBLE, "it's", "\""},
        {"it's\\", YEW_SH_Q_DOLLAR, "it\\'s\\\\", "'"},
        {"a\nb\x7f", YEW_SH_Q_DOLLAR, "a\\x0ab\\x7f", "'"},
        {"\x01" "f", YEW_SH_Q_DOLLAR, "\\x01f", "'"}
    };
    Arena a;
    size_t i;

    arena_init(&a);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        const char *closing = NULL;
        char *got = yew_shq_quote(&a, rows[i].text, strlen(rows[i].text),
                                  rows[i].q, &closing);

        YEW_ASSERT_EQ_STR(got, rows[i].want);
        YEW_ASSERT_EQ_STR(closing, rows[i].closing);
    }
    /* The whole insertion: opener, then the quoted rest; a `~user/`
     * prefix stays bare so it still expands. */
    {
        const char *closing = NULL;

        YEW_ASSERT_EQ_STR(yew_shq_insert(&a, "my dir/", 7U, 0U,
                                         YEW_SH_Q_DOUBLE, &closing),
                          "\"my dir/");
        YEW_ASSERT_EQ_STR(closing, "\"");
        YEW_ASSERT_EQ_STR(yew_shq_insert(&a, "~/a b", 5U, 2U, YEW_SH_Q_NONE,
                                         &closing),
                          "~/a\\ b");
        YEW_ASSERT_EQ_STR(closing, "");
        YEW_ASSERT_EQ_STR(yew_shq_insert(&a, "~root/x y", 9U, 6U,
                                         YEW_SH_Q_SINGLE, &closing),
                          "~root/'x y");
        YEW_ASSERT_EQ_STR(yew_shq_insert(&a, "x", 1U, 0U, YEW_SH_Q_DOLLAR,
                                         &closing),
                          "$'x");
    }
    arena_free_all(&a);
}

/* ---------------------------------------------------------------- */
/* §6 proof: the quoting round trip through real shells              */
/* ---------------------------------------------------------------- */

/*
 * Run `argv` with stdin from /dev/null and capture stdout.  Mirrors
 * test_shell_quote.c's sh_capture: the job layer's sync path
 * (yew_job_run_sync) is the inherit-tty handover and deliberately does
 * not capture output, so this is the capture the round trip needs.
 */
static bool shell_capture(char *const argv[], Bytebuf *out)
{
    int fds[2];
    pid_t pid;
    int status = 0;
    bool ok = true;

    if (!yew_pipe_cloexec(fds))
        return false;
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);

        if (devnull >= 0)
            (void)dup2(devnull, STDIN_FILENO);
        (void)dup2(fds[1], STDOUT_FILENO);
        (void)close(fds[0]);
        (void)close(fds[1]);
        (void)execv(argv[0], argv);
        _exit(127);
    }
    (void)close(fds[1]);
    for (;;) {
        u8 chunk[4096];
        ssize_t got = read(fds[0], chunk, sizeof(chunk));

        if (got > 0) {
            bytebuf_append(out, chunk, (size_t)got);
            continue;
        }
        if (got == 0)
            break;
        ok = false;
        break;
    }
    (void)close(fds[0]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        ok = false;
    return ok;
}

typedef struct RtShell {
    const char *label;
    const char *path;
    const char *opt; /* one extra flag before -c, or NULL */
} RtShell;

enum { RT_STRINGS = 500 };

/* The seeded strings.  The first ones are the hand-picked hazards; the
 * rest are random draws biased toward exactly those bytes. */
static size_t rt_make(u64 *seed, u32 index, char *buf, size_t cap)
{
    static const char *const fixed[] = {
        "$HOME", "a*b", "a$b c", "'", "\"", "\\", "\n", "\t", "~root", "=ls",
        "-n", "a b", "#x", "\xff\xfe", "x", "`id`", "$(id)", "!!", "a\\\nb",
        "~", "=", "^x", "a~b", "a=~b", "*", "?", "[a]", "{a,b}", "\x7f",
        "\x01\x02", "it's", "\"$x\"", "%s", "\\x41", "\xc3\xa9t\xc3\xa9"};
    static const char hazard[] = "$`*'\"\\\n\t ?[]{}()<>|&;~#^=!%,-@:";
    size_t len;
    size_t i;

    if (index < YEW_ARRAY_LEN(fixed)) {
        len = strlen(fixed[index]);
        (void)memcpy(buf, fixed[index], len);
        return len;
    }
    len = 1U + (size_t)(rng_next(seed) % (cap - 1U));
    for (i = 0U; i < len; i++) {
        u64 r = rng_next(seed);
        u32 cls = (u32)(r % 10U);

        if (cls < 4U)
            buf[i] = hazard[(r >> 8) % (sizeof(hazard) - 1U)];
        else if (cls < 6U)
            buf[i] = (char)(unsigned char)(0x80U + ((r >> 8) % 0x80U));
        else if (cls < 7U)
            buf[i] = (char)(unsigned char)(1U + ((r >> 8) % 31U));
        else
            buf[i] = "abcXYZ019._/"[(r >> 8) % 12U];
    }
    return len;
}

/* Does this shell know $'…' at all?  dash (a common Linux /bin/sh) does
 * not, and §1's table scopes the state to bash and zsh. */
static bool rt_has_dollar(const RtShell *sh)
{
    char *argv[5];
    Bytebuf out;
    bool ok;
    size_t n = 0U;

    argv[n++] = (char *)sh->path;
    if (sh->opt != NULL)
        argv[n++] = (char *)sh->opt;
    argv[n++] = (char *)"-c";
    argv[n++] = (char *)"printf %s $'a\\x41'";
    argv[n] = NULL;
    bytebuf_init(&out);
    ok = shell_capture(argv, &out) && out.len == 2U &&
         memcmp(out.data, "aA", 2U) == 0;
    bytebuf_free(&out);
    return ok;
}

static void rt_hex(const char *label, const u8 *s, size_t n)
{
    size_t i;

    (void)fprintf(stderr, "  %s (%zu):", label, n);
    for (i = 0U; i < n; i++)
        (void)fprintf(stderr, " %02x", (unsigned)s[i]);
    (void)fprintf(stderr, "\n");
}

/*
 * One shell, one quote state, all RT_STRINGS strings in ONE script: each
 * line is `printf '%s\0' <opener><quoted><closing>`, and stdout split on
 * NUL must be the original strings, byte for byte.
 */
static u32 rt_run(const RtShell *sh, YewShQuote q, char strings[][40],
                  const size_t *lens)
{
    Arena a;
    Bytebuf script;
    Bytebuf out;
    char *argv[5];
    size_t n = 0U;
    size_t off = 0U;
    u32 i;
    u32 bad = 0U;

    arena_init(&a);
    bytebuf_init(&script);
    bytebuf_init(&out);
    for (i = 0U; i < RT_STRINGS; i++) {
        const char *closing = NULL;
        char *word = yew_shq_insert(&a, strings[i], lens[i], 0U, q, &closing);

        bytebuf_append(&script, "printf '%s\\0' ", 14U);
        bytebuf_append(&script, word, strlen(word));
        bytebuf_append(&script, closing, strlen(closing));
        bytebuf_push_u8(&script, (u8)'\n');
    }
    bytebuf_push_u8(&script, 0U);
    argv[n++] = (char *)sh->path;
    if (sh->opt != NULL)
        argv[n++] = (char *)sh->opt;
    argv[n++] = (char *)"-c";
    argv[n++] = (char *)script.data;
    argv[n] = NULL;
    if (!shell_capture(argv, &out)) {
        (void)fprintf(stderr, "shq round trip: %s exited non-zero, state %u\n",
                      sh->label, (unsigned)q);
        bad++;
    }
    for (i = 0U; i < RT_STRINGS; i++) {
        const u8 *nul = off < out.len
                            ? memchr(out.data + off, 0, out.len - off)
                            : NULL;
        size_t got = nul == NULL ? 0U : (size_t)(nul - (out.data + off));

        if (nul == NULL || got != lens[i] ||
            memcmp(out.data + off, strings[i], got) != 0) {
            const char *closing = NULL;
            char *word = yew_shq_insert(&a, strings[i], lens[i], 0U, q,
                                        &closing);

            (void)fprintf(stderr,
                          "shq round trip: %s state %u string %u differs\n",
                          sh->label, (unsigned)q, (unsigned)i);
            rt_hex("want", (const u8 *)strings[i], lens[i]);
            rt_hex("inserted", (const u8 *)word, strlen(word));
            if (nul != NULL)
                rt_hex("got", out.data + off, got);
            bad++;
            break; /* later rows are misaligned once one is */
        }
        off += got + 1U;
    }
    bytebuf_free(&script);
    bytebuf_free(&out);
    arena_free_all(&a);
    return bad;
}

/*
 * DoD 3: 500 seeded strings -- `$`, backtick, `*`, quotes, backslash,
 * newline, tab, invalid UTF-8 -- in every quote state, through /bin/sh
 * and every zsh and bash installed.  zsh runs twice: with -f (its
 * defaults) and with EXTENDED_GLOB, which a ~/.zshenv can turn on for
 * every `zsh -c` and which makes `~ # ^` glob operators.
 */
void test_shq_roundtrip_real_shells(void)
{
    static const char *const zsh_paths[] = {"/bin/zsh", "/usr/bin/zsh",
                                            "/usr/local/bin/zsh",
                                            "/opt/homebrew/bin/zsh"};
    static const char *const bash_paths[] = {"/bin/bash", "/usr/bin/bash",
                                             "/usr/local/bin/bash",
                                             "/opt/homebrew/bin/bash"};
    static char strings[RT_STRINGS][40];
    static size_t lens[RT_STRINGS];
    RtShell shells[5];
    u32 nshells = 0U;
    u64 seed = UINT64_C(0x5723000000006001);
    u32 i;
    u32 s;
    u32 bad = 0U;
    u32 runs = 0U;

    for (i = 0U; i < RT_STRINGS; i++)
        lens[i] = rt_make(&seed, i, strings[i], sizeof(strings[i]));
    shells[nshells++] = (RtShell){"/bin/sh", "/bin/sh", NULL};
    for (i = 0U; i < YEW_ARRAY_LEN(zsh_paths); i++) {
        if (access(zsh_paths[i], X_OK) == 0) {
            shells[nshells++] = (RtShell){"zsh -f", zsh_paths[i], "-f"};
            shells[nshells++] = (RtShell){"zsh -o extendedglob",
                                          zsh_paths[i], "-oextendedglob"};
            break;
        }
    }
    for (i = 0U; i < YEW_ARRAY_LEN(bash_paths); i++) {
        if (access(bash_paths[i], X_OK) == 0) {
            shells[nshells++] = (RtShell){"bash", bash_paths[i], NULL};
            break;
        }
    }
    for (s = 0U; s < nshells; s++) {
        YewShQuote q;

        for (q = YEW_SH_Q_NONE; q <= YEW_SH_Q_DOLLAR; q++) {
            if (q == YEW_SH_Q_DOLLAR && !rt_has_dollar(&shells[s])) {
                (void)fprintf(stderr,
                              "shq round trip: %s has no $'...'; "
                              "state skipped\n",
                              shells[s].label);
                continue;
            }
            bad += rt_run(&shells[s], q, strings, lens);
            runs++;
        }
    }
    (void)fprintf(stderr, "shq round trip: %u shell/state runs x %u strings\n",
                  (unsigned)runs, (unsigned)RT_STRINGS);
    YEW_ASSERT(runs >= 3U);
    YEW_ASSERT_EQ_U64(bad, 0U);
}

/* §4: the core redaction predicate (the AI suite pins it to the rule). */
void test_shctx_secret_name_is_core(void)
{
    YEW_ASSERT(yew_secret_name("GITHUB_TOKEN"));
    YEW_ASSERT(yew_secret_name("aws_secret_access_key"));
    YEW_ASSERT(yew_secret_name("MyApiKey"));
    YEW_ASSERT(yew_secret_name("DB_PASSWD"));
    YEW_ASSERT(yew_secret_name("x_private_key_y"));
    YEW_ASSERT(yew_secret_name("NPM_CREDENTIALS"));
    YEW_ASSERT(!yew_secret_name("EDITOR"));
    YEW_ASSERT(!yew_secret_name("PATH"));
    YEW_ASSERT(!yew_secret_name("TOKE"));
    YEW_ASSERT(!yew_secret_name(""));
    YEW_ASSERT(!yew_secret_name(NULL));
}

/* The fuzz harness's seed corpus IS this corpus: one file per row, byte
 * for byte, so a row added here without its seed fails. */
void test_shctx_fuzz_seeds_match_the_corpus(void)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(sh_corpus); i++) {
        char path[96];
        char buf[256];
        FILE *f;
        size_t got;
        size_t want = strlen(sh_corpus[i].in);

        (void)snprintf(path, sizeof(path),
                       "tests/fuzz/corpus/fuzz_shctx/row-%03zu", i);
        f = fopen(path, "rb");
        if (f == NULL)
            (void)fprintf(stderr, "missing fuzz seed %s\n", path);
        YEW_ASSERT_NOT_NULL(f);
        got = fread(buf, 1U, sizeof(buf), f);
        YEW_ASSERT_EQ_I64(fclose(f), 0);
        YEW_ASSERT_EQ_U64((u64)got, (u64)want);
        YEW_ASSERT_EQ_MEM(buf, sh_corpus[i].in, want);
    }
}
