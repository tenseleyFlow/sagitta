#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/ed.h"
#include "ui/compspec.h"
#include "util/buf.h"
#include "util/runtime_asset.h"

/* ------------------------------------------------------------------ */
/* Every shipped spec validates (DoD 1)                                */
/* ------------------------------------------------------------------ */

static void check_desc(const char *file, const char *what, const char *desc)
{
    char msg[512];

    if (desc == NULL || strlen(desc) <= 60U)
        return;
    (void)snprintf(msg, sizeof(msg), "%s: %s description over 60 bytes: %s",
                   file, what, desc);
    yew_test_fail(__FILE__, __LINE__, msg);
}

static void check_arg_descs(const char *file, const YewSpecArg *arg)
{
    u32 i;

    if (arg == NULL)
        return;
    for (i = 0U; i < arg->n_values; i++)
        check_desc(file, arg->values[i].value, arg->values[i].desc);
}

static u32 check_node_descs(const char *file, const YewSpecNode *node)
{
    u32 i;
    u32 n = 1U;

    check_desc(file, node->name == NULL ? "root" : node->name, node->desc);
    for (i = 0U; i < node->n_flags; i++) {
        check_desc(file, node->flags[i].lng != NULL ? node->flags[i].lng
                                                    : node->flags[i].shrt,
                   node->flags[i].desc);
        check_arg_descs(file, node->flags[i].arg);
    }
    for (i = 0U; i < node->n_args; i++)
        check_arg_descs(file, &node->args[i]);
    check_arg_descs(file, node->dash_values);
    for (i = 0U; i < node->n_subs; i++)
        n += check_node_descs(file, &node->subs[i]);
    return n;
}

static void check_one_shipped(const SpecFix *f, const char *file)
{
    char err[512];
    char path[PATH_MAX];
    char first[256];
    Bytebuf src;
    FILE *fp;
    YewCompSpec *spec;

    YEW_ASSERT_NOT_NULL(file);
    if (!yew_compspec_check_shipped(file, err, sizeof(err)))
        yew_test_fail(__FILE__, __LINE__, err);
    SPEC_FMT(path, sizeof(path), "%s/completions/%s", f->runtime,
                   file);
    fp = fopen(path, "rb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_NOT_NULL(fgets(first, sizeof(first), fp));
    if (strncmp(first, "# ", 2U) != 0 ||
        strpbrk(first, "0123456789") == NULL) {
        (void)snprintf(err, sizeof(err),
                       "%s: first line must name the tool version", file);
        yew_test_fail(__FILE__, __LINE__, err);
    }
    (void)fclose(fp);
    bytebuf_init(&src);
    spec_read_file(path, &src);
    spec = yew_compspec_load_text(file, (const char *)src.data, src.len, err,
                                  sizeof(err));
    YEW_ASSERT_NOT_NULL(spec);
    check_desc(file, "command", yew_compspec_description(spec));
    YEW_ASSERT(check_node_descs(file, yew_compspec_root(spec)) >= 1U);
    yew_compspec_free(spec);
    bytebuf_free(&src);
}

/*
 * Iterates the shipped LIST -- the same one the alias index is built
 * from -- so a new file is tested without editing this test.  Each must
 * validate, keep every description to 60 bytes, and open with a comment
 * naming the tool version it was written against (§6).
 *
 * YEW_COMPSPEC_ONLY=git.fl checks one file while it is being written; the
 * full run (and CI) checks them all.
 */
void test_compspec_every_shipped_file_validates(void)
{
    const char *only = getenv("YEW_COMPSPEC_ONLY");
    SpecFix f;
    size_t n;
    size_t i;

    spec_fix_init(&f);
    n = yew_compspec_shipped_count();
    YEW_ASSERT_EQ_STR(yew_compspec_shipped_source(), "disk");
    for (i = 0U; i < n; i++) {
        const char *file = yew_compspec_shipped_name(i);

        if (only != NULL && only[0] != '\0' && strcmp(only, file) != 0)
            continue;
        check_one_shipped(&f, file);
    }
    /* §6's batch -- wolf git brew ssh scp make cargo uv gh, cd pushd
     * rmdir mkdir, kill and nine precommands -- plus tar: 24.  Checked
     * after the per-file loop so a broken file is named before the count
     * is. */
    if (only == NULL || only[0] == '\0')
        YEW_ASSERT(n >= 24U);
    spec_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* One test per §1 rejection rule; the message names file and key      */
/* ------------------------------------------------------------------ */

static void expect_reject(const char *src, const char *needle)
{
    char err[512];
    char msg[1024];
    YewCompSpec *spec;

    spec = yew_compspec_load_text("completions/t.fl", src, strlen(src), err,
                                  sizeof(err));
    if (spec != NULL) {
        yew_compspec_free(spec);
        (void)snprintf(msg, sizeof(msg), "accepted: %s", src);
        yew_test_fail(__FILE__, __LINE__, msg);
    }
    if (strstr(err, "completions/t.fl") == NULL ||
        strstr(err, needle) == NULL) {
        (void)snprintf(msg, sizeof(msg), "wanted '%s' in: %s", needle, err);
        yew_test_fail(__FILE__, __LINE__, msg);
    }
}

static void expect_accept(const char *src)
{
    char err[512];
    YewCompSpec *spec;

    spec = yew_compspec_load_text("completions/t.fl", src, strlen(src), err,
                                  sizeof(err));
    if (spec == NULL)
        yew_test_fail(__FILE__, __LINE__, err);
    yew_compspec_free(spec);
}

void test_compspec_rejects_unknown_keys_naming_key_and_path(void)
{
    expect_accept("{ completion: 1, command: \"t\" }");
    expect_reject("{ completion: 1, command: \"t\", bogus: 1 }",
                  "unknown key 'bogus'");
    expect_reject("{ completion: 1, command: \"t\", subcommands: ["
                  "{ name: \"a\", flags: [ { long: \"x\", colour: 1 } ] } ] }",
                  "unknown key 'colour' (at subcommands[0].flags[0])");
    expect_reject("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"path\", extension: [\"c\"] } ] }",
                  "unknown key 'extension' (at args[0])");
    /* A top-level key is not a subcommand key. */
    expect_reject("{ completion: 1, command: \"t\", subcommands: ["
                  "{ name: \"a\", command: \"b\" } ] }",
                  "unknown key 'command'");
}

void test_compspec_rejects_wrong_types(void)
{
    expect_reject("{ completion: 1, command: 5 }", "'command'");
    expect_reject("{ completion: 1, command: \"t\", description: [] }",
                  "'description' must be a string");
    expect_reject("{ completion: 1, command: \"t\", flags: {} }",
                  "'flags' must be a list");
    expect_reject("{ completion: 1, command: \"t\", flags: ["
                  "{ long: \"x\", global: \"yes\" } ] }",
                  "'global' must be true or false");
    expect_reject("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"colour\" } ] }",
                  "unknown argument kind 'colour'");
    expect_reject("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"values\" } ] }",
                  "kind \"values\" needs 'values'");
    expect_reject("{ completion: 1, command: \"t\", flags: ["
                  "{ long: \"--x\" } ] }",
                  "'long'");
    expect_reject("{ completion: 1, command: \"t\", flags: ["
                  "{ short: \"xy\" } ] }",
                  "'short'");
}

void test_compspec_rejects_a_bad_schema_version(void)
{
    expect_reject("{ completion: 2, command: \"t\" }", "'completion'");
    expect_reject("{ completion: \"1\", command: \"t\" }", "'completion'");
    expect_reject("{ command: \"t\" }", "missing required key 'completion'");
    expect_reject("{ completion: 1 }", "missing required key 'command'");
    expect_reject("{ completion: 1, command: \"t\", name: \"t\" }",
                  "'name' is for subcommands");
    /* Not data at all: the parser's line is named. */
    expect_reject("{ completion: 1, command: ", "line ");
    expect_reject("{ completion: 1, command: \"t\", subcommands: [ {} ] }",
                  "a subcommand needs 'name'");
}

void test_compspec_rejects_repeat_on_a_non_last_slot(void)
{
    expect_accept("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"path\" }, { kind: \"path\", repeat: true } ] }");
    expect_reject("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"path\", repeat: true }, { kind: \"dir\" } ] }",
                  "only the last argument may set 'repeat' (at args[0])");
}

void test_compspec_rejects_a_flag_with_no_spelling(void)
{
    expect_reject("{ completion: 1, command: \"t\", flags: ["
                  "{ desc: \"nameless\" } ] }",
                  "a flag needs 'long' or 'short' (at flags[0])");
    expect_reject("{ completion: 1, command: \"t\", flags: ["
                  "{ long: \"x\", arg_optional: true } ] }",
                  "'arg_optional' needs 'arg'");
}

void test_compspec_rejects_an_unresolved_generator(void)
{
    expect_accept("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"generator\", generator: \"make_targets\" } ] }");
    expect_accept("{ completion: 1, command: \"t\", generators: {"
                  " g: { argv: [\"true\"] } }, args: ["
                  "{ kind: \"generator\", generator: \"g\" } ] }");
    expect_reject("{ completion: 1, command: \"t\", args: ["
                  "{ kind: \"generator\", generator: \"nope\" } ] }",
                  "generator 'nope' is neither built in nor in 'generators'");
    expect_reject("{ completion: 1, command: \"t\", generators: {"
                  " g: { cache_ms: 5 } } }",
                  "a generator needs a non-empty 'argv' (at generators.g)");
    expect_reject("{ completion: 1, command: \"t\", generators: {"
                  " hosts: { argv: [\"x\"] } } }",
                  "shadows a built-in");
}

void test_compspec_rejects_duplicate_subcommand_names(void)
{
    expect_reject("{ completion: 1, command: \"t\", subcommands: ["
                  "{ name: \"a\" }, { name: \"a\" } ] }",
                  "duplicate subcommand name or alias 'a'");
    expect_reject("{ completion: 1, command: \"t\", subcommands: ["
                  "{ name: \"co\" }, { name: \"checkout\", aliases: [\"co\"] }"
                  " ] }",
                  "duplicate subcommand name or alias 'co'");
    /* The same name under two different parents is fine. */
    expect_accept("{ completion: 1, command: \"t\", subcommands: ["
                  "{ name: \"a\", subcommands: [ { name: \"x\" } ] },"
                  "{ name: \"b\", subcommands: [ { name: \"x\" } ] } ] }");
}

/* ------------------------------------------------------------------ */
/* Lookup: basename, user override, whole-file replace, report once    */
/* ------------------------------------------------------------------ */

void test_compspec_lookup_by_basename_and_alias(void)
{
    SpecFix f;
    const YewCompSpec *wolf;

    spec_fix_init(&f);
    wolf = yew_compspec_get(NULL, "wolf");
    YEW_ASSERT_NOT_NULL(wolf);
    YEW_ASSERT_EQ_STR(yew_compspec_origin(wolf), "completions/wolf.fl");
    YEW_ASSERT(yew_compspec_get(NULL, "/usr/local/bin/wolf") == wolf);
    YEW_ASSERT(yew_compspec_get(NULL, "./wolf") == wolf);
    /* A script path finds `build.sh.fl` only if someone wrote one. */
    YEW_ASSERT_NULL(yew_compspec_get(NULL, "./build.sh"));
    YEW_ASSERT_NULL(yew_compspec_get(NULL, ".."));
    YEW_ASSERT_NULL(yew_compspec_get(NULL, ""));
    YEW_ASSERT_EQ_STR(yew_compspec_describe("wolf"), "the wolf toolchain");
    YEW_ASSERT_NULL(yew_compspec_describe("no-such-command-s5724"));
    spec_fix_drop(&f);
}

void test_compspec_user_file_replaces_shipped_whole(void)
{
    SpecFix f;
    const YewCompSpec *spec;
    const YewSpecNode *root;

    spec_fix_init(&f);
    spec_fix_user(&f, "wolf",
                  "# test 1\n{ completion: 1, command: \"wolf\","
                  " subcommands: [ { name: \"mine\" } ] }\n");
    spec = yew_compspec_get(NULL, "wolf");
    YEW_ASSERT_NOT_NULL(spec);
    root = yew_compspec_root(spec);
    /* Replaced, never merged: the shipped subcommands are gone. */
    YEW_ASSERT_EQ_U64(root->n_subs, 1U);
    YEW_ASSERT_EQ_STR(root->subs[0].name, "mine");
    spec_fix_drop(&f);
}

void test_compspec_broken_user_file_is_reported_once(void)
{
    SpecFix f;
    Ed ed;

    spec_fix_init(&f);
    yew_ed_init(&ed);
    spec_fix_user(&f, "wolf", "{ completion: 1, command: \"wolf\", zap: 1 }");
    YEW_ASSERT_NULL(yew_compspec_get(&ed, "wolf"));
    YEW_ASSERT(!ed.msg.active); /* queued, never from inside a lookup */
    YEW_ASSERT(yew_compspec_notice(&ed));
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "wolf.fl"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "unknown key 'zap'"));
    yew_msg_clear(&ed);
    /* Once per session: the next lookup says nothing. */
    YEW_ASSERT_NULL(yew_compspec_get(&ed, "wolf"));
    YEW_ASSERT(!yew_compspec_notice(&ed));
    YEW_ASSERT(!ed.msg.active);
    /* It never hides the rest of the line's specs. */
    YEW_ASSERT_NOT_NULL(yew_compspec_get(&ed, "git"));
    yew_ed_free(&ed);
    spec_fix_drop(&f);
}

void test_compspec_user_file_is_rechecked_once_per_prompt(void)
{
    SpecFix f;
    const YewCompSpec *spec;
    struct timespec later[2];
    char path[PATH_MAX];

    spec_fix_init(&f);
    spec_fix_user(&f, "t5724",
                  "# t\n{ completion: 1, command: \"t5724\","
                  " subcommands: [ { name: \"one\" } ] }\n");
    spec = yew_compspec_get(NULL, "t5724");
    YEW_ASSERT_NOT_NULL(spec);
    YEW_ASSERT_EQ_STR(yew_compspec_root(spec)->subs[0].name, "one");
    spec_fix_user(&f, "t5724",
                  "# t\n{ completion: 1, command: \"t5724\","
                  " subcommands: [ { name: \"two\" } ] }\n");
    /* Force a different mtime even on a coarse filesystem clock. */
    SPEC_FMT(path, sizeof(path), "%s/yew/completions/t5724.fl",
                   f.config);
    later[0].tv_sec = 2000000000;
    later[0].tv_nsec = 0;
    later[1] = later[0];
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, path, later, 0), 0);
    /* Same prompt: not re-read. */
    spec = yew_compspec_get(NULL, "t5724");
    YEW_ASSERT_EQ_STR(yew_compspec_root(spec)->subs[0].name, "one");
    yew_compspec_prompt_closed();
    spec = yew_compspec_get(NULL, "t5724");
    YEW_ASSERT_EQ_STR(yew_compspec_root(spec)->subs[0].name, "two");
    spec_fix_drop(&f);
}

/* §7: decorating the command column reads the index -- a SCAN of the
 * shipped files' top level, no parse per keystroke and none at all for a
 * command nobody completes. */
void test_compspec_description_index_parses_nothing(void)
{
    SpecFix f;
    u32 before;
    u32 i;

    spec_fix_init(&f);
    before = yew_compspec_test_parse_count();
    YEW_ASSERT_EQ_STR(yew_compspec_describe("wolf"), "the wolf toolchain");
    YEW_ASSERT_NOT_NULL(yew_compspec_describe("git"));
    /* `gmake` is only in make.fl's command list. */
    YEW_ASSERT_EQ_STR(yew_compspec_describe("gmake"),
                      yew_compspec_describe("make"));
    for (i = 0U; i < 100U; i++) {
        (void)yew_compspec_describe("wolf");
        (void)yew_compspec_describe("no-such-s5724");
    }
    YEW_ASSERT_EQ_U64(yew_compspec_test_parse_count(), before);
    /* Completing a command parses its file, once. */
    YEW_ASSERT_NOT_NULL(yew_compspec_get(NULL, "gmake"));
    YEW_ASSERT_NOT_NULL(yew_compspec_get(NULL, "make"));
    YEW_ASSERT_EQ_U64(yew_compspec_test_parse_count(), before + 1U);
    spec_fix_drop(&f);
}

/* A precommand spec replaces the lexer's wrapper row. */
void test_compspec_precommand_feeds_the_lexer(void)
{
    SpecFix f;
    YewShWrapper w;

    spec_fix_init(&f);
    YEW_ASSERT_EQ_I64(yew_compspec_wrapper(NULL, "sudo", &w), 1);
    YEW_ASSERT_NOT_NULL(w.consumes);
    YEW_ASSERT(w.skips_assign);
    YEW_ASSERT_EQ_I64(yew_compspec_wrapper(NULL, "timeout", &w), 1);
    YEW_ASSERT_EQ_U64(w.operands, 1U);
    /* A spec without `precommand` un-wraps; no spec defers to the table. */
    YEW_ASSERT_EQ_I64(yew_compspec_wrapper(NULL, "git", &w), 0);
    YEW_ASSERT_EQ_I64(yew_compspec_wrapper(NULL, "unbuffer", &w), -1);
    spec_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* §3/§4: the resolution corpus                                        */
/* ------------------------------------------------------------------ */

#include "compspec_corpus.h"
#include "ui/cmdcomp.h"

typedef struct CorpusEd {
    Ed ed;
    char ws[128];
} CorpusEd;

static void corpus_ed_init(CorpusEd *c, const SpecFix *f)
{
    (void)memset(c, 0, sizeof(*c));
    SPEC_FMT(c->ws, sizeof(c->ws), "%s/ws", f->root);
    YEW_ASSERT_EQ_I64(mkdir(c->ws, 0700), 0);
    arena_init(&c->ed.arena);
    c->ed.ws.dir = c->ws;
}

static void corpus_ed_drop(CorpusEd *c)
{
    yew_comp_listing_invalidate();
    arena_free_all(&c->ed.arena);
}

/* The rows the live filter would show for `body` with the caret at
 * `caret`, space-joined in rank order. */
static void corpus_rows(Ed *ed, const char *body, size_t caret, Bytebuf *out)
{
    Bytebuf line;
    Arena scratch;
    Arena arena;
    CompFilter filter;
    YewCompQuery q;
    Vec_CompItem rows = {0};
    size_t i;

    bytebuf_init(&line);
    bytebuf_append(&line, ":!", 2U);
    bytebuf_append(&line, body, strlen(body));
    arena_init(&scratch);
    arena_init(&arena);
    yew_comp_filter_init(&filter);
    if (yew_comp_query(ed, (const char *)line.data, line.len, caret + 2U,
                       &scratch, &q))
        (void)yew_comp_filter_run(ed, &filter, &arena, &q, 0, &rows);
    for (i = 0U; i < rows.len; i++) {
        if (i != 0U)
            bytebuf_push_u8(out, (u8)' ');
        bytebuf_append(out, rows.data[i].text, strlen(rows.data[i].text));
    }
    bytebuf_push_u8(out, 0U);
    Vec_CompItem_free(&rows);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    arena_free_all(&scratch);
    bytebuf_free(&line);
}

/* Does the space-separated `set` hold `word`? */
static bool word_in(const char *set, const char *word, size_t n)
{
    const char *p = set;

    while (*p != '\0') {
        const char *end = strchr(p, ' ');
        size_t len = end == NULL ? strlen(p) : (size_t)(end - p);

        if (len == n && memcmp(p, word, n) == 0)
            return true;
        if (end == NULL)
            break;
        p = end + 1;
    }
    return false;
}

/* Every word of `want` is in `got`; with `exact`, and nothing else. */
static bool rows_match(const char *got, const char *want, bool exact)
{
    const char *p = want;
    size_t n_want = 0U;
    size_t n_got = 0U;

    while (*p != '\0') {
        const char *end = strchr(p, ' ');
        size_t len = end == NULL ? strlen(p) : (size_t)(end - p);

        if (!word_in(got, p, len))
            return false;
        n_want++;
        if (end == NULL)
            break;
        p = end + 1;
    }
    for (p = got; *p != '\0'; p++) {
        if (*p == ' ')
            n_got++;
    }
    if (got[0] != '\0')
        n_got++;
    return !exact || n_got == n_want;
}

void test_compspec_resolution_corpus(void)
{
    static const char caret_mark[] = "\xe2\x80\xb8";
    SpecFix f;
    CorpusEd c;
    size_t i;

    YEW_ASSERT(YEW_ARRAY_LEN(spec_corpus) >= 80U);
    spec_fix_init(&f);
    corpus_ed_init(&c, &f);
    for (i = 0U; i < YEW_ARRAY_LEN(spec_corpus); i++) {
        const SpecCorpusRow *row = &spec_corpus[i];
        const char *mark = strstr(row->in, caret_mark);
        char body[256];
        size_t caret;
        size_t tail;
        Arena a;
        YewShCtx ctx;
        char *got;
        char msg[768];

        YEW_ASSERT_NOT_NULL(mark);
        caret = (size_t)(mark - row->in);
        tail = strlen(mark + 3);
        YEW_ASSERT(caret + tail < sizeof(body));
        (void)memcpy(body, row->in, caret);
        (void)memcpy(body + caret, mark + 3, tail + 1U);
        arena_init(&a);
        YEW_ASSERT(yew_shctx_at_with(body, strlen(body), caret, &a,
                                     yew_compspec_wrapper, NULL, &ctx));
        got = yew_comp_shell_describe(&c.ed, &ctx, &a);
        if (strcmp(got, row->kind) != 0) {
            (void)snprintf(msg, sizeof(msg), "row %u `%s`: kind %s, want %s",
                           (unsigned)i, body, got, row->kind);
            yew_test_fail(__FILE__, __LINE__, msg);
        }
        arena_free_all(&a);
        if (row->rows != NULL) {
            Bytebuf rows;
            bool exact = row->rows[0] != '~';

            bytebuf_init(&rows);
            corpus_rows(&c.ed, body, caret, &rows);
            if (!rows_match((const char *)rows.data,
                            exact ? row->rows : row->rows + 1, exact)) {
                (void)snprintf(msg, sizeof(msg),
                               "row %u `%s`: rows [%s], want %s[%s]",
                               (unsigned)i, body, (const char *)rows.data,
                               exact ? "" : "at least ",
                               exact ? row->rows : row->rows + 1);
                yew_test_fail(__FILE__, __LINE__, msg);
            }
            bytebuf_free(&rows);
        }
    }
    corpus_ed_drop(&c);
    spec_fix_drop(&f);
}

/*
 * §7: in COMMAND position an executable whose name has a spec shows the
 * spec's description beside it -- `wolf  the wolf toolchain` next to
 * `which` -- read from the index, with no spec parsed to draw it.
 */
void test_compspec_command_rows_show_the_description(void)
{
    SpecFix f;
    CorpusEd c;
    char bin[256];
    char exe[300];
    char *saved_path = getenv("PATH") == NULL ? NULL
                                               : yew_xstrdup(getenv("PATH"));
    static const char *const names[] = {"wolf", "wobble"};
    Arena scratch;
    Arena arena;
    CompFilter filter;
    YewCompQuery q;
    Vec_CompItem items = {0};
    u32 before;
    size_t i;
    bool saw_wolf = false;

    spec_fix_init(&f);
    corpus_ed_init(&c, &f);
    SPEC_FMT(bin, sizeof(bin), "%s/bin", f.root);
    YEW_ASSERT_EQ_I64(mkdir(bin, 0700), 0);
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        FILE *fp;

        SPEC_FMT(exe, sizeof(exe), "%s/%s", bin, names[i]);
        fp = fopen(exe, "wb");
        YEW_ASSERT_NOT_NULL(fp);
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
        YEW_ASSERT_EQ_I64(chmod(exe, 0700), 0);
    }
    YEW_ASSERT_EQ_I64(setenv("PATH", bin, 1), 0);
    yew_comp_listing_invalidate();
    before = yew_compspec_test_parse_count();
    arena_init(&scratch);
    arena_init(&arena);
    yew_comp_filter_init(&filter);
    YEW_ASSERT(yew_comp_query(&c.ed, ":!wo", 4U, 4U, &scratch, &q));
    (void)yew_comp_filter_run(&c.ed, &filter, &arena, &q, 0, &items);
    for (i = 0U; i < items.len; i++) {
        if (strcmp(items.data[i].text, "wolf") == 0) {
            YEW_ASSERT_EQ_STR(items.data[i].detail, "the wolf toolchain");
            saw_wolf = true;
        } else if (strcmp(items.data[i].text, "wobble") == 0) {
            /* No spec: 57.18's detail, the $PATH element. */
            YEW_ASSERT_EQ_STR(items.data[i].detail, bin);
        }
    }
    YEW_ASSERT(saw_wolf);
    YEW_ASSERT_EQ_U64(yew_compspec_test_parse_count(), before);
    Vec_CompItem_free(&items);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    arena_free_all(&scratch);
    if (saved_path != NULL)
        YEW_ASSERT_EQ_I64(setenv("PATH", saved_path, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("PATH"), 0);
    yew_xfree(saved_path);
    yew_comp_listing_invalidate();
    corpus_ed_drop(&c);
    spec_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* Where shipped specs may come from: the runtime init.fl came from    */
/* ------------------------------------------------------------------ */

typedef struct RootFix {
    SpecFix f;
    char cwd[PATH_MAX];
    char prefix[256];
    char repo[256];
} RootFix;

static void root_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, strlen(text), fp), strlen(text));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

/* A hostile repository in the cwd: runtime/init.fl, and a spec whose
 * generator would run a program if anything loaded it. */
static void root_fix_init(RootFix *r, bool with_repo_init)
{
    char path[512];

    spec_fix_init(&r->f);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    YEW_ASSERT_NOT_NULL(getcwd(r->cwd, sizeof(r->cwd)));
    SPEC_FMT(r->prefix, sizeof(r->prefix), "%s/prefix", r->f.root);
    SPEC_FMT(r->repo, sizeof(r->repo), "%s/repo", r->f.root);
    YEW_ASSERT_EQ_I64(mkdir(r->prefix, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(r->repo, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/runtime", r->repo);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/runtime/completions", r->repo);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    if (with_repo_init) {
        SPEC_FMT(path, sizeof(path), "%s/runtime/init.fl", r->repo);
        root_write(path, "# repo init\n");
    }
    SPEC_FMT(path, sizeof(path), "%s/runtime/completions/lsx5724.fl",
             r->repo);
    root_write(path, "# evil 1\n{ completion: 1, command: \"lsx5724\",\n"
                     "  generators: { g: { argv: [\"sh\", \"-c\", \"x\"] } },\n"
                     "  args: [ { kind: \"generator\", generator: \"g\" } ] }\n");
    YEW_ASSERT_EQ_I64(chdir(r->repo), 0);
    yew_compspec_test_set_default_root(r->prefix);
}

static void root_fix_drop(RootFix *r)
{
    YEW_ASSERT_EQ_I64(chdir(r->cwd), 0);
    yew_compspec_test_set_default_root(NULL);
    spec_fix_drop(&r->f);
}

/*
 * The stale-install case: the prefix has init.fl but no completions/.
 * The cwd's runtime/completions is NOT loaded -- that would run a
 * repository's program on Tab -- and the reinstall message says so once.
 */
void test_compspec_stale_install_never_falls_back_to_cwd(void)
{
    RootFix r;
    char path[512];
    Ed ed;

    root_fix_init(&r, true);
    SPEC_FMT(path, sizeof(path), "%s/init.fl", r.prefix);
    root_write(path, "# installed init\n");
    yew_compspec_invalidate_all();
    YEW_ASSERT_EQ_I64(yew_runtime_root(NULL), YEW_RUNTIME_ROOT_PREFIX);
    yew_ed_init(&ed);
    YEW_ASSERT_NULL(yew_compspec_get(&ed, "lsx5724"));
    YEW_ASSERT(yew_compspec_notice(&ed));
    /* Only the embedded image, if this build has one -- never the cwd. */
    YEW_ASSERT_EQ_STR(yew_compspec_shipped_source(), "embedded");
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, r.prefix));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "no completions/"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "reinstall"));
    yew_msg_clear(&ed);
    YEW_ASSERT_NULL(yew_compspec_get(&ed, "lsx5724"));
    YEW_ASSERT_NULL(yew_compspec_get(&ed, "git"));
    YEW_ASSERT(!yew_compspec_notice(&ed));
    YEW_ASSERT(!ed.msg.active);
    yew_ed_free(&ed);
    /* With completions/ in the prefix, the prefix's specs are used. */
    SPEC_FMT(path, sizeof(path), "%s/completions", r.prefix);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/completions/pfx5724.fl", r.prefix);
    root_write(path, "# t 1\n{ completion: 1, command: \"pfx5724\" }\n");
    yew_compspec_invalidate_all();
    YEW_ASSERT_NOT_NULL(yew_compspec_get(NULL, "pfx5724"));
    YEW_ASSERT_NULL(yew_compspec_get(NULL, "lsx5724"));
    root_fix_drop(&r);
}

/* An uninstalled dev build run from its repository root keeps working:
 * that repository's init.fl is already the one running. */
void test_compspec_uninstalled_build_uses_its_source_tree(void)
{
    RootFix r;

    root_fix_init(&r, true);
    YEW_ASSERT_EQ_I64(yew_runtime_root(NULL), YEW_RUNTIME_ROOT_SOURCE);
    YEW_ASSERT_NOT_NULL(yew_compspec_get(NULL, "lsx5724"));
    YEW_ASSERT_EQ_STR(yew_compspec_shipped_source(), "disk");
    root_fix_drop(&r);
}

/* $YEW_RUNTIME_DIR is the one directory, even with a prefix and a
 * source tree both present; and a cwd without runtime/init.fl is never
 * a runtime at all. */
void test_compspec_runtime_dir_env_is_the_only_directory(void)
{
    RootFix r;
    char path[512];
    char env[512];

    root_fix_init(&r, false);
    SPEC_FMT(path, sizeof(path), "%s/init.fl", r.prefix);
    root_write(path, "# installed init\n");
    SPEC_FMT(path, sizeof(path), "%s/completions", r.prefix);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/completions/pfx5724.fl", r.prefix);
    root_write(path, "# t 1\n{ completion: 1, command: \"pfx5724\" }\n");
    SPEC_FMT(env, sizeof(env), "%s/envrt", r.f.root);
    YEW_ASSERT_EQ_I64(mkdir(env, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/completions", env);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/completions/env5724.fl", env);
    root_write(path, "# t 1\n{ completion: 1, command: \"env5724\" }\n");
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", env, 1), 0);
    yew_compspec_invalidate_all();
    YEW_ASSERT_EQ_I64(yew_runtime_root(NULL), YEW_RUNTIME_ROOT_ENV);
    YEW_ASSERT_NOT_NULL(yew_compspec_get(NULL, "env5724"));
    YEW_ASSERT_NULL(yew_compspec_get(NULL, "pfx5724"));
    YEW_ASSERT_NULL(yew_compspec_get(NULL, "lsx5724"));
    YEW_ASSERT_EQ_U64(yew_compspec_shipped_count(), 1U);
    /* No env, no prefix init, no ./runtime/init.fl: the repo's specs
     * are still not loaded. */
    YEW_ASSERT_EQ_I64(unsetenv("YEW_RUNTIME_DIR"), 0);
    SPEC_FMT(path, sizeof(path), "%s/init.fl", r.prefix);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    yew_compspec_invalidate_all();
    YEW_ASSERT_NULL(yew_compspec_get(NULL, "lsx5724"));
    root_fix_drop(&r);
}
