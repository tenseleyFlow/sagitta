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
    (void)strcpy(fixture->root, "/tmp/sagitta-cmdcomp-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(fixture->root));
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
    SAG_ASSERT(fd >= 0);
    SAG_ASSERT_EQ_I64(close(fd), 0);
}

static void fixture_dir(const CompFixture *fixture, const char *name)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fixture->root, name);
    SAG_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void fixture_unlink(const CompFixture *fixture, const char *name,
                           bool is_dir)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fixture->root, name);
    if (is_dir)
        SAG_ASSERT_EQ_I64(rmdir(path), 0);
    else
        SAG_ASSERT_EQ_I64(unlink(path), 0);
}

static void fixture_dispose(CompFixture *fixture)
{
    arena_free_all(&fixture->ed.arena);
    SAG_ASSERT_EQ_I64(rmdir(fixture->root), 0);
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
        {"ed.ui.probe", NULL, SAG_ARITY_OPT_STR, 0U, "probe"},
        "fbov", SAG_RP_FORBID, NULL,
    };
    static const CmdEntry repeat = {
        {"ed.ui.probe", NULL, SAG_ARITY_OPT_STR, 0U, "probe"},
        "f*", SAG_RP_FORBID, NULL,
    };
    static const CmdEntry free_string = {
        {"ed.ui.probe", NULL, SAG_ARITY_OPT_STR, 0U, "probe"},
        "s", SAG_RP_FORBID, NULL,
    };
    SagCompKind kind = SAG_COMP_VALUE;
    Vec_CompItem commands = {0};
    Ed ed = {0};
    Arena scratch;
    SagCompQuery query;

    SAG_ASSERT(sag_comp_kind_for(NULL, 0U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_CMD);
    SAG_ASSERT(sag_comp_kind_for(&entry, 1U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_PATH);
    SAG_ASSERT(sag_comp_kind_for(&entry, 2U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_BUFFER);
    SAG_ASSERT(sag_comp_kind_for(&entry, 3U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_OPTION);
    SAG_ASSERT(sag_comp_kind_for(&entry, 4U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_VALUE);
    SAG_ASSERT(!sag_comp_kind_for(&entry, 5U, &kind));
    SAG_ASSERT(sag_comp_kind_for(&repeat, 1U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_PATH);
    SAG_ASSERT(sag_comp_kind_for(&repeat, 9U, &kind));
    SAG_ASSERT_EQ_I64(kind, SAG_COMP_PATH);
    SAG_ASSERT(!sag_comp_kind_for(&free_string, 1U, &kind));
    /*
     * Sprint 18.5 §2 closed the sag_comp_score seam; ranking is now
     * sag_fz_score's.  The sentinel moved from -1 to SAG_FZ_NO_MATCH,
     * and a genuine match may score NEGATIVE (the length penalty) -- a
     * `< 0` reject here would silently drop the longest real candidates.
     */
    SAG_ASSERT(sag_fz_score("fop", 3U, "file.open", 9U, NULL) !=
               SAG_FZ_NO_MATCH);
    SAG_ASSERT(sag_fz_score("file", 4U, "file.open", 9U, NULL) >
               sag_fz_score("fop", 3U, "file.open", 9U, NULL));
    SAG_ASSERT_EQ_I64(sag_fz_score("xyz", 3U, "file.open", 9U, NULL),
                      SAG_FZ_NO_MATCH);

    arena_init(&ed.arena);
    arena_init(&scratch);
    sag_cmd_shutdown();
    sag_cmd_init();
    SAG_ASSERT(sag_comp_query(&ed, "file.open al", 12U, 12U, &scratch,
                              &query));
    SAG_ASSERT_EQ_I64(query.kind, SAG_COMP_PATH);
    SAG_ASSERT_EQ_STR(query.stem, "al");
    SAG_ASSERT_EQ_U64(query.replace.lo, 10U);
    SAG_ASSERT_EQ_U64(query.replace.hi, 12U);
    SAG_ASSERT(!sag_comp_query(&ed, "not_a_command al", 16U, 16U,
                               &scratch, &query));
    SAG_ASSERT(sag_comp_query(&ed, "file.o", 6U, 6U, &scratch, &query));
    SAG_ASSERT_EQ_I64(query.kind, SAG_COMP_CMD);
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&ed, SAG_COMP_CMD, "cmdline.", &commands), 0U);
    SAG_ASSERT_EQ_U64(commands.len, 0U);
    (void)sag_comp_enumerate(&ed, SAG_COMP_CMD, "del.", &commands);
    SAG_ASSERT_NULL(find_item(&commands, "del.word_prev"));
    SAG_ASSERT_NULL(find_item(&commands, "del.to_home"));
    SAG_ASSERT_NULL(find_item(&commands, "del.to_end"));
    (void)sag_comp_enumerate(&ed, SAG_COMP_CMD, "mode.", &commands);
    SAG_ASSERT_NULL(find_item(&commands, "mode.enter"));
    SAG_ASSERT_NULL(find_item(&commands, "mode.escape"));
    Vec_CompItem_free(&commands);
    sag_cmd_shutdown();
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
    CompSource mine = {SAG_COMP_PATH, "stub", stub_enumerate, 0U};
    CompSource saved;
    CompFixture fixture;
    Vec_CompItem items = {0};

    fixture_init(&fixture);
    fixture_file(&fixture, "real");

    /* Every kind ships a source; an empty provider is data, not a gap. */
    SAG_ASSERT_EQ_U64(sag_comp_source_count(), (u64)SAG_COMP_KIND__N);
    SAG_ASSERT_NOT_NULL(sag_comp_source(SAG_COMP_OPTION));
    SAG_ASSERT_NULL(sag_comp_source(SAG_COMP_KIND__N));

    saved = *sag_comp_source(SAG_COMP_PATH);
    SAG_ASSERT_EQ_STR(saved.name, "path");
    SAG_ASSERT((saved.flags & SAG_COMP_SRC_SLOW) != 0U);

    /* Registration is idempotent by kind: this REPLACES the built-in
     * path source rather than adding a second one fighting over it. */
    sag_comp_source_register(&mine);
    SAG_ASSERT_EQ_U64(sag_comp_source_count(), (u64)SAG_COMP_KIND__N);
    SAG_ASSERT_EQ_STR(sag_comp_source(SAG_COMP_PATH)->name, "stub");
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "re", &items), 1U);
    SAG_ASSERT_EQ_STR(items.data[0].text, "stub");

    sag_comp_source_register(&saved);
    SAG_ASSERT_EQ_STR(sag_comp_source(SAG_COMP_PATH)->name, "path");
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "re", &items), 1U);
    SAG_ASSERT_EQ_STR(items.data[0].text, "real");

    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "real", false);
    fixture_dispose(&fixture);
}

void test_cmdcomp_path_head_len_is_the_one_split_rule(void)
{
    /* §4 keys its cache on this and the path source splits on it; two
     * split rules would let the cache serve one directory's entries
     * while the source read another's. */
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len("file"), 0U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len("src/file"), 4U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len("src/ui/"), 7U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len("/abs/path"), 5U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len("/"), 1U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len(""), 0U);
    SAG_ASSERT_EQ_U64(sag_comp_path_head_len(NULL), 0U);
}

static SagCompQuery path_query(const char *stem)
{
    SagCompQuery q;

    (void)memset(&q, 0, sizeof(q));
    q.kind = SAG_COMP_PATH;
    q.source = sag_comp_source(SAG_COMP_PATH);
    q.stem = stem;
    return q;
}

void test_cmdcomp_filter_reuses_the_set_while_the_pattern_only_grows(void)
{
    CompFilter filter;
    CompFixture fixture;
    Arena arena;
    Vec_CompItem out = {0};
    SagCompQuery q;

    fixture_init(&fixture);
    fixture_dir(&fixture, "sub");
    fixture_file(&fixture, "alpha");
    fixture_file(&fixture, "alpine");
    fixture_file(&fixture, "beta");
    arena_init(&arena);
    sag_comp_filter_init(&filter);
    sag_comp_test_reset_enumerate_count();

    /* Typing forward: one opendir, then pure re-ranking.  The opendir is
     * the cost that matters, which is why the cache is keyed on the
     * directory head and not on the whole stem. */
    /* Three, not two: matching is subsequence, so "beta" matches "a"
     * as surely as "alpha" does -- it just scores far below it. */
    q = path_query("a");
    SAG_ASSERT_EQ_U64(sag_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 3U);
    q = path_query("al");
    (void)sag_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    q = path_query("alp");
    (void)sag_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    q = path_query("alph");
    SAG_ASSERT_EQ_U64(sag_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 1U);
    SAG_ASSERT_EQ_STR(out.data[0].text, "alpha");
    SAG_ASSERT_EQ_U64(sag_comp_test_enumerate_count(), 1U);

    /*
     * Narrowing is measured against the pattern the set was ENUMERATED
     * with, not against the previous keystroke -- so backspacing all the
     * way to "a" still reuses the set that "a" produced.  Retreating
     * inside the cached superset is free; only leaving it costs.
     */
    q = path_query("a");
    SAG_ASSERT_EQ_U64(sag_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 3U);
    SAG_ASSERT_EQ_U64(sag_comp_test_enumerate_count(), 1U);

    /*
     * Backspacing PAST it does widen the set: the cached candidates were
     * the ones matching "a", and "" matches everything, including "sub"
     * which the cache never held.
     */
    q = path_query("");
    SAG_ASSERT_EQ_U64(sag_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out), 4U);
    SAG_ASSERT_EQ_U64(sag_comp_test_enumerate_count(), 2U);

    /* A different directory head is a different set entirely. */
    q = path_query("sub/");
    (void)sag_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    SAG_ASSERT_EQ_U64(sag_comp_test_enumerate_count(), 3U);

    Vec_CompItem_free(&out);
    sag_comp_filter_free(&filter);
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

        sag_comp_filter_init(&narrowed);
        /* Build the cached set from the first character, then narrow it
         * one character at a time up to the full pattern. */
        for (step = 1U; step <= len; step++) {
            char prefix[8];
            SagCompQuery q;

            (void)memcpy(prefix, pattern, step);
            prefix[step] = '\0';
            q = path_query(prefix);
            (void)sag_comp_filter_run(&fixture.ed, &narrowed, &arena, &q, 0,
                                      &a);
        }
        /* The same final pattern, straight from the source. */
        {
            Arena fresh_arena;
            SagCompQuery q = path_query(pattern);

            arena_init(&fresh_arena);
            sag_comp_filter_init(&fresh);
            (void)sag_comp_filter_run(&fixture.ed, &fresh, &fresh_arena, &q,
                                      0, &b);
            SAG_ASSERT_EQ_U64(a.len, b.len);
            for (i = 0U; i < a.len; i++)
                SAG_ASSERT_EQ_STR(a.data[i].text, b.data[i].text);
            sag_comp_filter_free(&fresh);
            arena_free_all(&fresh_arena);
        }
        sag_comp_filter_free(&narrowed);
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
    SagCompQuery q;
    char name[32];
    u32 i;

    fixture_init(&fixture);
    /* More matches than SAG_COMP_MAX, so the source has to cap. */
    for (i = 0U; i < SAG_COMP_MAX + 200U; i++) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)i);
        fixture_file(&fixture, name);
    }
    arena_init(&arena);
    sag_comp_filter_init(&filter);
    sag_comp_test_reset_enumerate_count();

    q = path_query("file");
    SAG_ASSERT_EQ_U64(sag_comp_filter_run(&fixture.ed, &filter, &arena, &q,
                                          0, &out),
                      SAG_COMP_MAX + 200U);
    SAG_ASSERT_EQ_U64(out.len, SAG_COMP_MAX);

    /*
     * The cached set holds the 500 best matches for "file"; an entry the
     * cap cut can still be among the best for "file06", so narrowing
     * over it would silently lose rows.  The filter re-enumerates
     * instead, and the answer is exact.
     */
    q = path_query("file06");
    (void)sag_comp_filter_run(&fixture.ed, &filter, &arena, &q, 0, &out);
    SAG_ASSERT_EQ_U64(sag_comp_test_enumerate_count(), 2U);
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
            if (out.data[i].score >= SAG_FZ_BASENAME_TIER)
                tiered++;
        }
        /* file0600..file0699 -- the fixture stops at 699. */
        SAG_ASSERT_EQ_U64(tiered, 100U);
    }
    SAG_ASSERT_EQ_STR(out.data[0].text, "file0600");
    SAG_ASSERT_EQ_STR(out.data[9].text, "file0609");

    Vec_CompItem_free(&out);
    sag_comp_filter_free(&filter);
    arena_free_all(&arena);
    for (i = 0U; i < SAG_COMP_MAX + 200U; i++) {
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

    total = sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "a", &items);
    SAG_ASSERT_EQ_U64(total, 3U);
    SAG_ASSERT_EQ_U64(items.len, 3U);
    SAG_ASSERT_NULL(find_item(&items, ".amber"));
    dir = find_item(&items, "archive/");
    SAG_ASSERT_NOT_NULL(dir);
    SAG_ASSERT(dir->is_dir);
    SAG_ASSERT_EQ_STR(items.data[0].text, "alpha");
    SAG_ASSERT_EQ_STR(items.data[1].text, "apricot");
    SAG_ASSERT_EQ_STR(items.data[2].text, "archive/");

    total = sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, ".", &items);
    SAG_ASSERT_EQ_U64(total, 1U);
    SAG_ASSERT_EQ_STR(items.data[0].text, ".amber");

    sag_comp_test_force_dtype_unknown(true);
    total = sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "arch", &items);
    SAG_ASSERT_EQ_U64(total, 1U);
    SAG_ASSERT(items.data[0].is_dir);
    SAG_ASSERT_EQ_U64(sag_comp_test_lstat_count(), 1U);
    sag_comp_test_force_dtype_unknown(false);

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
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "file", &first),
        1200U);
    SAG_ASSERT_EQ_U64(first.len, SAG_COMP_MAX);
    SAG_ASSERT_EQ_STR(first.data[0].text, "file0000");
    SAG_ASSERT_EQ_STR(first.data[SAG_COMP_MAX - 1U].text, "file0499");
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "file", &second),
        1200U);
    SAG_ASSERT_EQ_U64(second.len, first.len);
    for (i = 0U; i < SAG_COMP_MAX; i++)
        SAG_ASSERT_EQ_STR(second.data[i].text, first.data[i].text);

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
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "a", &items), 1U);
    SAG_ASSERT_EQ_U64(items.len, 1U);
    item = &items.data[0];
    SAG_ASSERT(item->text[0] == '"');
    SAG_ASSERT(strstr(item->text, "\\\"") != NULL);
    SAG_ASSERT(strstr(item->text, "\\\\") != NULL);
    SAG_ASSERT(strstr(item->text, "\\%") != NULL);
    SAG_ASSERT(strstr(item->text, "\\n") != NULL);
    (void)snprintf(line, sizeof(line), "file.open %s", item->text);
    arena_init(&parse_arena);
    SAG_ASSERT(sag_cmd_parse(&fixture.ed, line, strlen(line), &parse_arena,
                             &parsed));
    SAG_ASSERT_EQ_U64(parsed.argv.n, 2U);
    SAG_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    arena_init(&parse_arena);
    SAG_ASSERT(sag_cmd_parse(
        &fixture.ed,
        "file.open a\\ space\\\"quote\\'$\\\\percent\\%and\\\nline",
        sizeof("file.open a\\ space\\\"quote\\'$\\\\percent\\%and\\\nline") -
            1U,
        &parse_arena, &parsed));
    SAG_ASSERT_EQ_U64(parsed.argv.n, 2U);
    SAG_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.open");
    SAG_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    arena_init(&parse_arena);
    SAG_ASSERT(sag_cmd_parse(
        &fixture.ed,
        "file.open 'a space\"quote'\\''$\\percent%and\nline'",
        sizeof("file.open 'a space\"quote'\\''$\\percent%and\nline'") - 1U,
        &parse_arena, &parsed));
    SAG_ASSERT_EQ_U64(parsed.argv.n, 2U);
    SAG_ASSERT_EQ_STR(parsed.argv.v[0], "ed.file.open");
    SAG_ASSERT_EQ_STR(parsed.argv.v[1], raw);
    arena_free_all(&parse_arena);

    fixture_unlink(&fixture, raw, false);
    fixture_file(&fixture, "cr\rname");
    items.len = 0U;
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "cr", &items), 1U);
    SAG_ASSERT_EQ_U64(items.len, 1U);
    SAG_ASSERT_NULL(strchr(items.data[0].text, '\r'));
    SAG_ASSERT_NOT_NULL(strstr(items.data[0].text, "cr name"));
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
    SAG_ASSERT_EQ_U64(
        sag_comp_enumerate(&fixture.ed, SAG_COMP_PATH, "al", &items), 2U);
    arena_init(&scratch);
    lcp = sag_comp_lcp(&scratch, &items);
    SAG_ASSERT_EQ_STR(lcp, "alp");
    sag_menu_init(&menu, NULL);
    SAG_ASSERT_EQ_I64(menu.sel, -1);
    SAG_ASSERT(!menu.explicit_sel);
    sag_menu_reset(&menu, items, 2U, (Span){0U, 0U});
    items = (Vec_CompItem){0};
    sag_menu_free(&menu);
    SAG_ASSERT_EQ_I64(menu.sel, -1);
    SAG_ASSERT_NULL(menu.items.data);
    SAG_ASSERT_EQ_U64(sag_comp_enumerate(&fixture.ed, SAG_COMP_OPTION,
                                          "tab", &items), 0U);
    SAG_ASSERT_EQ_U64(sag_comp_enumerate(&fixture.ed, SAG_COMP_VALUE,
                                          "4", &items), 0U);
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "lan",
                                  .kind = SAG_COMP_VALUE}));
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xaa" "tre",
                                  .kind = SAG_COMP_VALUE}));
    lcp = sag_comp_lcp(&scratch, &items);
    SAG_ASSERT_EQ_STR(lcp, "");
    items.len = 0U;
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "clair",
                                  .kind = SAG_COMP_VALUE}));
    Vec_CompItem_push(&items,
                      ((CompItem){.text = "\xc3\xa9" "toile",
                                  .kind = SAG_COMP_VALUE}));
    lcp = sag_comp_lcp(&scratch, &items);
    SAG_ASSERT_EQ_STR(lcp, "\xc3\xa9");
    arena_free_all(&scratch);
    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "alpine", false);
    fixture_unlink(&fixture, "alpha", false);
    fixture_dispose(&fixture);
}
