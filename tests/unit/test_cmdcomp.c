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
    SAG_ASSERT(sag_comp_score("fop", "file.open") >= 0);
    SAG_ASSERT(sag_comp_score("file", "file.open") >
               sag_comp_score("fop", "file.open"));
    SAG_ASSERT_EQ_I64(sag_comp_score("xyz", "file.open"), -1);

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
    sag_cmd_shutdown();
    arena_free_all(&scratch);
    arena_free_all(&ed.arena);
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
    for (i = 0U; i < 1200U; i++) {
        (void)snprintf(name, sizeof(name), "file%04u", (unsigned)i);
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

    fixture_unlink(&fixture, raw, false);
    Vec_CompItem_free(&items);
    fixture_dispose(&fixture);
}

void test_cmdcomp_lcp_menu_and_empty_providers(void)
{
    CompFixture fixture;
    Vec_CompItem items = {0};
    CompMenu menu;
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
    sag_comp_menu_init(&menu);
    SAG_ASSERT_EQ_I64(menu.sel, -1);
    SAG_ASSERT(!menu.cycling);
    menu.items = items;
    items = (Vec_CompItem){0};
    sag_comp_menu_free(&menu);
    SAG_ASSERT_EQ_I64(menu.sel, -1);
    SAG_ASSERT_NULL(menu.items.data);
    SAG_ASSERT_EQ_U64(sag_comp_enumerate(&fixture.ed, SAG_COMP_OPTION,
                                          "tab", &items), 0U);
    SAG_ASSERT_EQ_U64(sag_comp_enumerate(&fixture.ed, SAG_COMP_VALUE,
                                          "4", &items), 0U);
    arena_free_all(&scratch);
    Vec_CompItem_free(&items);
    fixture_unlink(&fixture, "alpine", false);
    fixture_unlink(&fixture, "alpha", false);
    fixture_dispose(&fixture);
}
