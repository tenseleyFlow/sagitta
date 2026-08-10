#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/cmdcomp.h"
#include "ui/cmdparse.h"
#include "ui/menu.h"

typedef struct {
    char root[128];
    Ed ed;
} CompFixture;

static void fixture_init(CompFixture *fixture)
{
    (void)strcpy(fixture->root, "/tmp/yew-cmdcomp-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fixture->root));
    (void)memset(&fixture->ed, 0, sizeof(fixture->ed));
    arena_init(&fixture->ed.arena);
    fixture->ed.ws.dir = fixture->root;
}

static void fixture_file(const CompFixture *fixture, const char *name)
{
    char path[512];
    int fd;

    (void)snprintf(path, sizeof(path), "%s/%s", fixture->root, name);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void fixture_dir(const CompFixture *fixture, const char *name)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fixture->root, name);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void fixture_unlink(const CompFixture *fixture, const char *name,
                           bool is_dir)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fixture->root, name);
    if (is_dir)
        YEW_ASSERT_EQ_I64(rmdir(path), 0);
    else
        YEW_ASSERT_EQ_I64(unlink(path), 0);
}

static void fixture_dispose(CompFixture *fixture)
{
    arena_free_all(&fixture->ed.arena);
    YEW_ASSERT_EQ_I64(rmdir(fixture->root), 0);
}

static const CompItem *find_item(const Vec_CompItem *items,
                                 const char *text)
{
    size_t i;

    for (i = 0U; i < items->len; i++) {
        if (strcmp(items->data[i].text, text) == 0)
            return &items->data[i];
    }
    return NULL;
}

void test_cmdcomp_source_selection_and_score(void)
{
    static const CmdEntry entry = {
        {"ed.ui.probe", NULL, YEW_ARITY_OPT_STR, 0U, "probe", NULL},
        "fbov", YEW_RP_FORBID, NULL,
    };
    static const CmdEntry repeat = {
        {"ed.ui.probe", NULL, YEW_ARITY_OPT_STR, 0U, "probe", NULL},
        "f*", YEW_RP_FORBID, NULL,
    };
    static const CmdEntry free_string = {
        {"ed.ui.probe", NULL, YEW_ARITY_OPT_STR, 0U, "probe", NULL},
        "s", YEW_RP_FORBID, NULL,
    };
    YewCompKind kind = YEW_COMP_VALUE;
    Vec_CompItem commands = {0};
    Ed ed = {0};
    Arena scratch;
    YewCompQuery query;

    YEW_ASSERT(yew_comp_kind_for(NULL, 0U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_CMD);
    YEW_ASSERT(yew_comp_kind_for(&entry, 1U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_PATH);
    YEW_ASSERT(yew_comp_kind_for(&entry, 2U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_BUFFER);
    YEW_ASSERT(yew_comp_kind_for(&entry, 3U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_OPTION);
    YEW_ASSERT(yew_comp_kind_for(&entry, 4U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_VALUE);
    YEW_ASSERT(!yew_comp_kind_for(&entry, 5U, &kind));
    YEW_ASSERT(yew_comp_kind_for(&repeat, 1U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_PATH);
    YEW_ASSERT(yew_comp_kind_for(&repeat, 9U, &kind));
    YEW_ASSERT_EQ_I64(kind, YEW_COMP_PATH);
    YEW_ASSERT(!yew_comp_kind_for(&free_string, 1U, &kind));
    /*
     * Sprint 18.5 §2 closed the yew_comp_score seam; ranking is now
     * yew_fz_score's.  The sentinel moved from -1 to YEW_FZ_NO_MATCH,
     * and a genuine match may score NEGATIVE (the length penalty) -- a
     * `< 0` reject here would silently drop the longest real candidates.
     */
    YEW_ASSERT(yew_fz_score("fop", 3U, "file.open", 9U, NULL) !=
               YEW_FZ_NO_MATCH);
    YEW_ASSERT(yew_fz_score("file", 4U, "file.open", 9U, NULL) >
               yew_fz_score("fop", 3U, "file.open", 9U, NULL));
    YEW_ASSERT_EQ_I64(yew_fz_score("xyz", 3U, "file.open", 9U, NULL),
                      YEW_FZ_NO_MATCH);

    arena_init(&ed.arena);
    arena_init(&scratch);
    yew_cmd_shutdown();
    yew_cmd_init();
    YEW_ASSERT(yew_comp_query(&ed, "file.open al", 12U, 12U, &scratch,
                              &query));
    YEW_ASSERT_EQ_I64(query.kind, YEW_COMP_PATH);
    YEW_ASSERT_EQ_STR(query.stem, "al");
    YEW_ASSERT_EQ_U64(query.replace.lo, 10U);
    YEW_ASSERT_EQ_U64(query.replace.hi, 12U);
    YEW_ASSERT(!yew_comp_query(&ed, "not_a_command al", 16U, 16U,
                               &scratch, &query));
    YEW_ASSERT(yew_comp_query(&ed, "file.o", 6U, 6U, &scratch, &query));
    YEW_ASSERT_EQ_I64(query.kind, YEW_COMP_CMD);
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&ed, YEW_COMP_CMD, "cmdline.", &commands), 0U);
    YEW_ASSERT_EQ_U64(commands.len, 0U);
    (void)yew_comp_enumerate(&ed, YEW_COMP_CMD, "del.", &commands);
    YEW_ASSERT_NULL(find_item(&commands, "del.word_prev"));
    YEW_ASSERT_NULL(find_item(&commands, "del.to_home"));
    YEW_ASSERT_NULL(find_item(&commands, "del.to_end"));
    (void)yew_comp_enumerate(&ed, YEW_COMP_CMD, "mode.", &commands);
    YEW_ASSERT_NULL(find_item(&commands, "mode.enter"));
    YEW_ASSERT_NULL(find_item(&commands, "mode.escape"));
    Vec_CompItem_free(&commands);
    yew_cmd_shutdown();
    arena_free_all(&scratch);
    arena_free_all(&ed.arena);
}

static u32 stub_enumerate(const CompReq *req, Vec_CompItem *out)
{
    CompItem item;

    (void)memset(&item, 0, sizeof(item));
    item.text = "stub";
    item.kind = (u8)req->kind;
    out->len = 0U;
    Vec_CompItem_push(out, item);
    return 1U;
}

void test_cmdcomp_source_registry_replaces_by_kind(void)
{
    CompSource mine = {YEW_COMP_PATH, "stub", stub_enumerate, 0U};
    CompSource saved;
    CompFixture fixture;
    Vec_CompItem items = {0};

    fixture_init(&fixture);
    fixture_file(&fixture, "real");

    /* Every kind ships a source; an empty provider is data, not a gap. */
    YEW_ASSERT_EQ_U64(yew_comp_source_count(), (u64)YEW_COMP_KIND__N);
    YEW_ASSERT_NOT_NULL(yew_comp_source(YEW_COMP_OPTION));
    YEW_ASSERT_NULL(yew_comp_source(YEW_COMP_KIND__N));

    saved = *yew_comp_source(YEW_COMP_PATH);
    YEW_ASSERT_EQ_STR(saved.name, "path");
    YEW_ASSERT((saved.flags & YEW_COMP_SRC_SLOW) != 0U);

    /* Registration is idempotent by kind: this REPLACES the built-in
     * path source rather than adding a second one fighting over it. */
    yew_comp_source_register(&mine);
    YEW_ASSERT_EQ_U64(yew_comp_source_count(), (u64)YEW_COMP_KIND__N);
    YEW_ASSERT_EQ_STR(yew_comp_source(YEW_COMP_PATH)->name, "stub");
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "re", &items), 1U);
    YEW_ASSERT_EQ_STR(items.data[0].text, "stub");

    yew_comp_source_register(&saved);
    YEW_ASSERT_EQ_STR(yew_comp_source(YEW_COMP_PATH)->name, "path");
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "re", &items), 1U);
    YEW_ASSERT_EQ_STR(items.data[0].text, "real");

    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "real", false);
    fixture_dispose(&fixture);
}

void test_cmdcomp_path_head_len_is_the_one_split_rule(void)
{
    /* §4 keys its cache on this and the path source splits on it; two
     * split rules would let the cache serve one directory's entries
     * while the source read another's. */
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len("file"), 0U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len("src/file"), 4U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len("src/ui/"), 7U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len("/abs/path"), 5U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len("/"), 1U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len(""), 0U);
    YEW_ASSERT_EQ_U64(yew_comp_path_head_len(NULL), 0U);
}

static YewCompQuery path_query(const char *stem)
{
    YewCompQuery q;

    (void)memset(&q, 0, sizeof(q));
    q.kind = YEW_COMP_PATH;
    q.source = yew_comp_source(YEW_COMP_PATH);
    q.stem = stem;
    return q;
}

void test_cmdcomp_filter_reuses_the_set_while_the_pattern_only_grows(void)
{
    CompFilter filter;
    CompFixture fixture;
    Arena arena;
    Vec_CompItem out = {0};
    YewCompQuery q;

    fixture_init(&fixture);
    fixture_dir(&fixture, "sub");
    fixture_file(&fixture, "alpha");
    fixture_file(&fixture, "alpine");
    fixture_file(&fixture, "beta");
    arena_init(&arena);
    yew_comp_filter_init(&filter);
    yew_comp_test_reset_enumerate_count();

    /* Typing forward: one opendir, then pure re-ranking.  The opendir is
     * the cost that matters, which is why the cache is keyed on the
     * directory head and not on the whole stem. */
    /* Three, not two: matching is subsequence, so "beta" matches "a"
     * as surely as "alpha" does -- it just scores far below it. */
    q = path_query("a");
    YEW_ASSERT_EQ_U64(yew_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 3U);
    q = path_query("al");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    q = path_query("alp");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    q = path_query("alph");
    YEW_ASSERT_EQ_U64(yew_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 1U);
    YEW_ASSERT_EQ_STR(out.data[0].text, "alpha");
    YEW_ASSERT_EQ_U64(yew_comp_test_enumerate_count(), 1U);

    /*
     * Narrowing is measured against the pattern the set was ENUMERATED
     * with, not against the previous keystroke -- so backspacing all the
     * way to "a" still reuses the set that "a" produced.  Retreating
     * inside the cached superset is free; only leaving it costs.
     */
    q = path_query("a");
    YEW_ASSERT_EQ_U64(yew_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 3U);
    YEW_ASSERT_EQ_U64(yew_comp_test_enumerate_count(), 1U);

    /*
     * Backspacing PAST it does widen the set: the cached candidates were
     * the ones matching "a", and "" matches everything, including "sub"
     * which the cache never held.
     */
    q = path_query("");
    YEW_ASSERT_EQ_U64(yew_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 4U);
    YEW_ASSERT_EQ_U64(yew_comp_test_enumerate_count(), 2U);

    /* A different directory head is a different set entirely. */
    q = path_query("sub/");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    YEW_ASSERT_EQ_U64(yew_comp_test_enumerate_count(), 3U);

    Vec_CompItem_free(&out);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    fixture_unlink(&fixture, "beta", false);
    fixture_unlink(&fixture, "alpine", false);
    fixture_unlink(&fixture, "alpha", false);
    fixture_unlink(&fixture, "sub", true);
    fixture_dispose(&fixture);
}

void test_cmdcomp_filter_narrowing_equals_a_full_reenumerate(void)
{
    static const char alphabet[] = "abcdefgh";
    CompFixture fixture;
    Arena arena;
    CompFilter narrowed;
    CompFilter fresh;
    Vec_CompItem a = {0};
    Vec_CompItem b = {0};
    char name[16];
    char pattern[8];
    u32 seed = 12345U;
    u32 trial;
    u32 i;

    fixture_init(&fixture);
    /* A deterministic spread of names over a small alphabet, so patterns
     * hit varied match counts without the cap ever biting. */
    for (i = 0U; i < 240U; i++) {
        (void)snprintf(name, sizeof(name), "%c%c%c%02u",
                       alphabet[i % 8U], alphabet[(i / 8U) % 8U],
                       alphabet[(i / 64U) % 8U], (unsigned)(i % 40U));
        fixture_file(&fixture, name);
    }
    arena_init(&arena);

    for (trial = 0U; trial < 400U; trial++) {
        u32 len;
        u32 step;

        /* Deterministic LCG: a random-looking walk that is byte-identical
         * on every run and every libc (invariant 5). */
        seed = seed * 1103515245U + 12345U;
        len = 1U + (seed >> 16U) % 3U;
        for (i = 0U; i < len; i++) {
            seed = seed * 1103515245U + 12345U;
            pattern[i] = alphabet[(seed >> 16U) % 8U];
        }
        pattern[len] = '\0';

        yew_comp_filter_init(&narrowed);
        /* Build the cached set from the first character, then narrow it
         * one character at a time up to the full pattern. */
        for (step = 1U; step <= len; step++) {
            char prefix[8];
            YewCompQuery q;

            (void)memcpy(prefix, pattern, step);
            prefix[step] = '\0';
            q = path_query(prefix);
            (void)yew_comp_filter_run(&fixture.ed, &narrowed, &arena, &q, 0,
                                      &a);
        }
        /* The same final pattern, straight from the source. */
        {
            Arena fresh_arena;
            YewCompQuery q = path_query(pattern);

            arena_init(&fresh_arena);
            yew_comp_filter_init(&fresh);
            (void)yew_comp_filter_run(&fixture.ed, &fresh, &fresh_arena, &q,
                                      0, &b);
            YEW_ASSERT_EQ_U64(a.len, b.len);
            for (i = 0U; i < a.len; i++)
                YEW_ASSERT_EQ_STR(a.data[i].text, b.data[i].text);
            yew_comp_filter_free(&fresh);
            arena_free_all(&fresh_arena);
        }
        yew_comp_filter_free(&narrowed);
        arena_free_all(&arena);
    }

    Vec_CompItem_free(&a);
    Vec_CompItem_free(&b);
    arena_free_all(&arena);
    for (i = 0U; i < 240U; i++) {
        (void)snprintf(name, sizeof(name), "%c%c%c%02u",
                       alphabet[i % 8U], alphabet[(i / 8U) % 8U],
                       alphabet[(i / 64U) % 8U], (unsigned)(i % 40U));
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}

void test_cmdcomp_filter_refuses_to_narrow_a_capped_set(void)
{
    CompFilter filter;
    CompFixture fixture;
    Arena arena;
    Vec_CompItem out = {0};
    YewCompQuery q;
    char name[32];
    u32 i;

    fixture_init(&fixture);
    /* More matches than YEW_COMP_MAX, so the source has to cap. */
    for (i = 0U; i < YEW_COMP_MAX + 200U; i++) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)i);
        fixture_file(&fixture, name);
    }
    arena_init(&arena);
    yew_comp_filter_init(&filter);
    yew_comp_test_reset_enumerate_count();

    q = path_query("file");
    YEW_ASSERT_EQ_U64(yew_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out),
                      YEW_COMP_MAX + 200U);
    YEW_ASSERT_EQ_U64(out.len, YEW_COMP_MAX);

    /*
     * The cached set holds the 500 best matches for "file"; an entry the
     * cap cut can still be among the best for "file06", so narrowing
     * over it would silently lose rows.  The filter re-enumerates
     * instead, and the answer is exact.
     */
    q = path_query("file06");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    YEW_ASSERT_EQ_U64(yew_comp_test_enumerate_count(), 2U);
    /*
     * Exactly ten entries have "file06" as a PREFIX and so sit in the
     * basename tier; many more match it as a subsequence ("file0160"
     * contains f,i,l,e,0,6 in order) and rank below them.  Asserting the
     * tier count rather than the raw match count is what pins the
     * ordering the user actually sees.
     */
    {
        u32 tiered = 0U;

        for (i = 0U; i < out.len; i++) {
            if (out.data[i].score >= YEW_FZ_BASENAME_TIER)
                tiered++;
        }
        /* file0600..file0699 -- the fixture stops at 699. */
        YEW_ASSERT_EQ_U64(tiered, 100U);
    }
    YEW_ASSERT_EQ_STR(out.data[0].text, "file0600");
    YEW_ASSERT_EQ_STR(out.data[9].text, "file0609");

    Vec_CompItem_free(&out);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    for (i = 0U; i < YEW_COMP_MAX + 200U; i++) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)i);
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}

void test_cmdcomp_path_hidden_directory_and_unknown_dtype(void)
{
    CompFixture fixture;
    Vec_CompItem items = {0};
    const CompItem *dir;
    u32 total;

    fixture_init(&fixture);
    fixture_file(&fixture, "alpha");
    fixture_file(&fixture, "apricot");
    fixture_file(&fixture, ".amber");
    fixture_dir(&fixture, "archive");

    total = yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "a", &items);
    YEW_ASSERT_EQ_U64(total, 3U);
    YEW_ASSERT_EQ_U64(items.len, 3U);
    YEW_ASSERT_NULL(find_item(&items, ".amber"));
    dir = find_item(&items, "archive/");
    YEW_ASSERT_NOT_NULL(dir);
    YEW_ASSERT(dir->is_dir);
    YEW_ASSERT_EQ_STR(items.data[0].text, "alpha");
    YEW_ASSERT_EQ_STR(items.data[1].text, "apricot");
    YEW_ASSERT_EQ_STR(items.data[2].text, "archive/");

    total = yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, ".", &items);
    YEW_ASSERT_EQ_U64(total, 1U);
    YEW_ASSERT_EQ_STR(items.data[0].text, ".amber");

    yew_comp_test_force_dtype_unknown(true);
    total = yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "arch", &items);
    YEW_ASSERT_EQ_U64(total, 1U);
    YEW_ASSERT(items.data[0].is_dir);
    YEW_ASSERT_EQ_U64(yew_comp_test_lstat_count(), 1U);
    yew_comp_test_force_dtype_unknown(false);

    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "archive", true);
    fixture_unlink(&fixture, ".amber", false);
    fixture_unlink(&fixture, "apricot", false);
    fixture_unlink(&fixture, "alpha", false);
    fixture_dispose(&fixture);
}

void test_cmdcomp_cap_and_deterministic_order(void)
{
    CompFixture fixture;
    Vec_CompItem first = {0};
    Vec_CompItem second = {0};
    char name[32];
    u32 i;

    fixture_init(&fixture);
    for (i = 1200U; i != 0U; i--) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)(i - 1U));
        fixture_file(&fixture, name);
    }
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "file", &first),
        1200U);
    YEW_ASSERT_EQ_U64(first.len, YEW_COMP_MAX);
    YEW_ASSERT_EQ_STR(first.data[0].text, "file0000");
    YEW_ASSERT_EQ_STR(first.data[YEW_COMP_MAX - 1U].text, "file0499");
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "file", &second),
        1200U);
    YEW_ASSERT_EQ_U64(second.len, first.len);
    for (i = 0U; i < YEW_COMP_MAX; i++)
        YEW_ASSERT_EQ_STR(second.data[i].text, first.data[i].text);

    Vec_CompItem_free(&second);
    Vec_CompItem_free(&first);
    for (i = 0U; i < 1200U; i++) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)i);
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}

void test_cmdcomp_path_quoting_retokenizes_one_argv(void)
{
    CompFixture fixture;
    Vec_CompItem items = {0};
    Arena parse_arena;
    CmdParse parsed;
    const char raw[] = "a space\"quote'$\\percent%and\nline";
    const CompItem *item;
    char line[1024];

    fixture_init(&fixture);
    fixture_file(&fixture, raw);
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "a", &items), 1U);
    YEW_ASSERT_EQ_U64(items.len, 1U);
    item = &items.data[0];
    YEW_ASSERT(item->text[0] == '"');
    YEW_ASSERT(strstr(item->text, "\\\"") != NULL);
    YEW_ASSERT(strstr(item->text, "\\\\") != NULL);
    YEW_ASSERT(strstr(item->text, "\\%") != NULL);
    YEW_ASSERT(strstr(item->text, "\\n") != NULL);
    (void)snprintf(line, sizeof(line), "file.open %s", item->text);
    arena_init(&parse_arena);
    YEW_ASSERT(yew_cmd_parse(&fixture.ed, line, strlen(line), &parse_arena,
                             &parsed));
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    arena_init(&parse_arena);
    YEW_ASSERT(yew_cmd_parse(
        &fixture.ed,
        "file.open a\\ space\\\"quote\\'$\\\\percent\\%and\\\nline",
        sizeof("file.open a\\ space\\\"quote\\'$\\\\percent\\%and\\\nline") -
            1U,
        &parse_arena, &parsed));
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.open");
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    arena_init(&parse_arena);
    YEW_ASSERT(yew_cmd_parse(
        &fixture.ed,
        "file.open 'a space\"quote'\\''$\\percent%and\nline'",
        sizeof("file.open 'a space\"quote'\\''$\\percent%and\nline'") - 1U,
        &parse_arena, &parsed));
    YEW_ASSERT_EQ_U64(parsed.argv.n, 2U);
    YEW_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.open");
    YEW_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    fixture_unlink(&fixture, raw, false);
    fixture_file(&fixture, "cr\rname");
    items.len = 0U;
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "cr", &items), 1U);
    YEW_ASSERT_EQ_U64(items.len, 1U);
    YEW_ASSERT_NULL(strchr(items.data[0].text, '\r'));
    YEW_ASSERT_NOT_NULL(strstr(items.data[0].text, "cr name"));
    fixture_unlink(&fixture, "cr\rname", false);
    Vec_CompItem_free(&items);
    fixture_dispose(&fixture);
}

void test_cmdcomp_lcp_menu_and_empty_providers(void)
{
    CompFixture fixture;
    Vec_CompItem items = {0};
    Menu menu;
    Arena scratch;
    char *lcp;

    fixture_init(&fixture);
    fixture_file(&fixture, "alpha");
    fixture_file(&fixture, "alpine");
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "al", &items), 2U);
    arena_init(&scratch);
    lcp = yew_comp_lcp(&scratch, &items);
    YEW_ASSERT_EQ_STR(lcp, "alp");
    yew_menu_init(&menu, NULL);
    YEW_ASSERT_EQ_I64(menu.sel, -1);
    YEW_ASSERT(!menu.explicit_sel);
    yew_menu_reset(&menu, items, 2U, (Span){0U, 0U});
    items = (Vec_CompItem){0};
    yew_menu_free(&menu);
    YEW_ASSERT_EQ_I64(menu.sel, -1);
    YEW_ASSERT_NULL(menu.items.data);
    YEW_ASSERT(yew_comp_enumerate(&fixture.ed, YEW_COMP_OPTION,
                                  "tab", &items) >= 1U);
    YEW_ASSERT_EQ_U64(yew_comp_enumerate(&fixture.ed, YEW_COMP_VALUE,
                                          "4", &items), 0U);
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "lan",
                                  .kind = YEW_COMP_VALUE}));
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xaa" "tre",
                                  .kind = YEW_COMP_VALUE}));
    lcp = yew_comp_lcp(&scratch, &items);
    YEW_ASSERT_EQ_STR(lcp, "");
    items.len = 0U;
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "clair",
                                  .kind = YEW_COMP_VALUE}));
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "toile",
                                  .kind = YEW_COMP_VALUE}));
    lcp = yew_comp_lcp(&scratch, &items);
    YEW_ASSERT_EQ_STR(lcp, "\xc3\xa9");
    arena_free_all(&scratch);
    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "alpine", false);
    fixture_unlink(&fixture, "alpha", false);
    fixture_dispose(&fixture);
}

/*
 * Sprint 18.5 DoD 10: one opendir per directory, however many keystrokes
 * follow.
 *
 * The ranked set caps at YEW_COMP_MAX and a capped set cannot answer a
 * narrowed pattern, so before the listing cache every keystroke past the
 * directory head rescanned.  This asserts the COUNT, which is what the
 * DoD says; a latency number would pass on a fast disk and hide it.
 */
void test_cmdcomp_listing_scans_a_directory_once(void)
{
    CompFixture fixture;
    CompFilter filter;
    Arena arena;
    Vec_CompItem out = {0};
    static const char *const steps[] = {"e", "en", "ent", "entr", "entry",
                                        "entry0", "entry01"};
    u64 before;
    u32 i;

    fixture_init(&fixture);
    /* Enough entries that the ranked set caps -- below the cap the filter
     * could narrow on its own and the listing would prove nothing. */
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_file(&fixture, name);
    }
    arena_init(&arena);
    yew_comp_filter_init(&filter);
    /* One warm-up so the count excludes whatever the first call loads. */
    {
        YewCompQuery q = path_query("e");

        (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    }
    before = yew_comp_listing_opendirs();
    for (i = 0U; i < YEW_ARRAY_LEN(steps); i++) {
        YewCompQuery q = path_query(steps[i]);

        (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    }
    YEW_ASSERT_EQ_U64(yew_comp_listing_opendirs() - before, 0U);
    YEW_ASSERT(out.len != 0U);

    /*
     * And the cache is not allowed to outlive the menu: a directory that
     * changed between prompts must be seen.  yew_comp_enumerate is the
     * fresh-read form, so it both rescans and retires the listing.
     */
    fixture_file(&fixture, "zebra");
    out.len = 0U;
    YEW_ASSERT_EQ_U64(
        yew_comp_enumerate(&fixture.ed, YEW_COMP_PATH, "zebra", &out), 1U);
    fixture_unlink(&fixture, "zebra", false);

    Vec_CompItem_free(&out);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}

/*
 * The directory scan is SLICED, and resuming it must not rescan.
 *
 * Reading a big directory used to land entirely on one keystroke -- the
 * first character typed after the argument's space -- and on a CI runner
 * that was ~4 ms of invariant 4's 5 ms budget.  It is now bounded per
 * call and continued from the idle path.
 *
 * Driven with a 1 us budget rather than a real one so the assertions are
 * about the MECHANISM and not about how fast this machine reads a
 * directory: the scan checks its clock every 256 entries, so any budget
 * this small stops at the first check and a 600-entry fixture is
 * guaranteed to come back partial.  A timing-based version of this test
 * would pass vacuously on a fast filesystem -- which is exactly what
 * happens locally, where the full scan fits inside one real slice.
 */
void test_cmdcomp_listing_slices_and_resumes_without_rescanning(void)
{
    CompFixture fixture;
    CompFilter filter;
    CompFilter fresh;
    Arena arena;
    Arena fresh_arena;
    Vec_CompItem sliced = {0};
    Vec_CompItem whole = {0};
    YewCompQuery q;
    u64 before;
    u32 slices = 0U;
    u32 i;

    fixture_init(&fixture);
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_file(&fixture, name);
    }
    arena_init(&arena);
    yew_comp_filter_init(&filter);

    before = yew_comp_listing_opendirs();
    q = path_query("e");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 1, &sliced);
    /* Stopped early, with the handle held open for the resume. */
    YEW_ASSERT(yew_comp_listing_pending());
    YEW_ASSERT_EQ_U64(yew_comp_listing_opendirs() - before, 1U);
    /* Partial is still USEFUL: the menu shows the best of what was read
     * rather than nothing at all. */
    YEW_ASSERT(sliced.len != 0U);

    /* The idle path drains it.  ONE opendir for the whole scan, however
     * many slices it takes -- a resume that reopened would both re-read
     * from the top and never terminate. */
    while (yew_comp_listing_advance(1)) {
        slices++;
        YEW_ASSERT(slices < 100U);
    }
    YEW_ASSERT(slices != 0U);
    YEW_ASSERT(!yew_comp_listing_pending());
    YEW_ASSERT_EQ_U64(yew_comp_listing_opendirs() - before, 1U);

    /*
     * And the finished scan answers exactly what an unsliced one would.
     * Compared against a fresh unbounded enumerate rather than against a
     * count, so a slice boundary that dropped or duplicated an entry
     * shows up as a different set and not merely a different total.
     *
     * TWO arenas and two filters, because yew_comp_filter_run resets the
     * arena it is handed whenever it re-enumerates -- the second result
     * would otherwise free the first one's strings out from under this
     * comparison.  ASan caught exactly that when both shared one arena.
     */
    yew_comp_filter_invalidate(&filter);
    q = path_query("e");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &sliced);

    yew_comp_listing_invalidate();
    arena_init(&fresh_arena);
    yew_comp_filter_init(&fresh);
    q = path_query("e");
    (void)yew_comp_filter_run(&fixture.ed, &fresh, &fresh_arena, &q, 0,
                              &whole);
    /* Tab's unlimited budget finishes in the one call it is given. */
    YEW_ASSERT(!yew_comp_listing_pending());
    YEW_ASSERT_EQ_U64(sliced.len, whole.len);
    YEW_ASSERT(whole.len != 0U);
    for (i = 0U; i < (u32)sliced.len; i++)
        YEW_ASSERT_EQ_I64(strcmp(sliced.data[i].text, whole.data[i].text), 0);

    Vec_CompItem_free(&whole);
    Vec_CompItem_free(&sliced);
    yew_comp_filter_free(&fresh);
    yew_comp_filter_free(&filter);
    arena_free_all(&fresh_arena);
    arena_free_all(&arena);
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}

/*
 * Overflowing the hold limit MID-SLICE must not free the cache key it is
 * still being resumed with.
 *
 * yew_comp_listing_advance resumes a scan by passing comp_listing.dir
 * straight back to the stepper, so on the overflow path `scan_dir` and
 * the cache's own key are the SAME pointer.  Disposing the listing there
 * and re-duplicating the name reads freed memory — quietly, on a build
 * without a sanitizer, and only for a directory past YEW_COMP_LIST_MAX.
 * The limit is lowered here so the path is reachable at 600 entries; the
 * sanitizer lanes are what turn this into a hard failure.
 */
void test_cmdcomp_listing_overflow_midslice_keeps_its_key(void)
{
    CompFixture fixture;
    CompFilter filter;
    Arena arena;
    Vec_CompItem out = {0};
    YewCompQuery q;
    u32 i;

    fixture_init(&fixture);
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_file(&fixture, name);
    }
    arena_init(&arena);
    yew_comp_filter_init(&filter);

    /* Small enough that the fixture overflows it, large enough that the
     * first slice ends before it does. */
    yew_comp_test_set_list_max(400U);
    q = path_query("e");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 1, &out);
    YEW_ASSERT(yew_comp_listing_pending());

    /* Resumes straight into the overflow.  Nothing is cached afterwards,
     * and the scan is over rather than pending. */
    while (yew_comp_listing_advance(1))
        ;
    YEW_ASSERT(!yew_comp_listing_pending());

    /*
     * And an overflowed directory still COMPLETES, by streaming: the
     * cache declining to hold it is not the menu declining to answer.
     */
    yew_comp_filter_invalidate(&filter);
    q = path_query("entry1");
    (void)yew_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    YEW_ASSERT(out.len != 0U);

    yew_comp_test_set_list_max(0U);
    Vec_CompItem_free(&out);
    yew_comp_filter_free(&filter);
    arena_free_all(&arena);
    for (i = 0U; i < 600U; i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "entry%03u", (unsigned)i);
        fixture_unlink(&fixture, name, false);
    }
    fixture_dispose(&fixture);
}
