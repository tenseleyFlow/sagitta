#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "edit/option.h"
#include "mod/lsp/features.h"
#include "mod/lsp/pickers.h"
#include "mod/lsp/sync.h"
#include "util/arena.h"

static JsonValue *nav_json(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(arena, (const u8 *)json,
                                      (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

static void nav_parse(const char *json, Vec_LspLoc *locs, u32 want)
{
    Arena arena;

    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_locations_parse(nav_json(&arena, json), locs),
                      want);
    arena_free_all(&arena);
}

void test_lsp_locations_null_and_empty_array_return_no_rows(void)
{
    Vec_LspLoc locs = {0};

    YEW_ASSERT_EQ_U64(yew_lsp_locations_parse(NULL, &locs), 0U);
    nav_parse("null", &locs, 0U);
    nav_parse("[]", &locs, 0U);
    YEW_ASSERT_EQ_U64(locs.len, 0U);
    yew_lsp_locations_free(&locs);
}

void test_lsp_locations_single_location_is_owned(void)
{
    static const char json[] =
        "{\"uri\":\"file:///tmp/owned%20location.c\","
        "\"range\":{\"start\":{\"line\":4,\"character\":7},"
        "\"end\":{\"line\":4,\"character\":11}}}";
    Vec_LspLoc locs = {0};

    nav_parse(json, &locs, 1U);
    YEW_ASSERT_EQ_U64(locs.len, 1U);
    YEW_ASSERT_EQ_STR(locs.data[0].path, "/tmp/owned location.c");
    YEW_ASSERT_EQ_U64(locs.data[0].line, 4U);
    YEW_ASSERT_EQ_U64(locs.data[0].chr, 7U);
    YEW_ASSERT_EQ_U64(locs.data[0].end_line, 4U);
    YEW_ASSERT_EQ_U64(locs.data[0].end_chr, 11U);
    yew_lsp_locations_free(&locs);
    YEW_ASSERT_NULL(locs.data);
    YEW_ASSERT_EQ_U64(locs.len, 0U);
    YEW_ASSERT_EQ_U64(locs.cap, 0U);
}

void test_lsp_locations_many_sort_stably_by_path_line_and_column(void)
{
    static const char json[] =
        "["
        "{\"uri\":\"file:///z.c\",\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":3,\"character\":8},"
        "\"end\":{\"line\":3,\"character\":12}}},"
        "{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":1,\"character\":9},"
        "\"end\":{\"line\":1,\"character\":10}}},"
        "{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":3,\"character\":2},"
        "\"end\":{\"line\":3,\"character\":9}}},"
        "{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":3,\"character\":2},"
        "\"end\":{\"line\":3,\"character\":5}}}"
        "]";
    Vec_LspLoc locs = {0};

    nav_parse(json, &locs, 5U);
    YEW_ASSERT_EQ_STR(locs.data[0].path, "/a.c");
    YEW_ASSERT_EQ_U64(locs.data[0].line, 1U);
    YEW_ASSERT_EQ_U64(locs.data[0].chr, 9U);
    YEW_ASSERT_EQ_U64(locs.data[1].line, 3U);
    YEW_ASSERT_EQ_U64(locs.data[1].chr, 2U);
    YEW_ASSERT_EQ_U64(locs.data[1].end_chr, 9U);
    YEW_ASSERT_EQ_U64(locs.data[2].line, 3U);
    YEW_ASSERT_EQ_U64(locs.data[2].chr, 2U);
    YEW_ASSERT_EQ_U64(locs.data[2].end_chr, 5U);
    YEW_ASSERT_EQ_U64(locs.data[3].chr, 8U);
    YEW_ASSERT_EQ_STR(locs.data[4].path, "/z.c");
    yew_lsp_locations_free(&locs);
}

void test_lsp_locations_location_link_prefers_target_selection_range(void)
{
    static const char json[] =
        "[{\"targetUri\":\"file:///link.c\","
        "\"targetRange\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":8,\"character\":9}},"
        "\"targetSelectionRange\":{"
        "\"start\":{\"line\":5,\"character\":6},"
        "\"end\":{\"line\":5,\"character\":10}}}]";
    Vec_LspLoc locs = {0};

    nav_parse(json, &locs, 1U);
    YEW_ASSERT_EQ_STR(locs.data[0].path, "/link.c");
    YEW_ASSERT_EQ_U64(locs.data[0].line, 5U);
    YEW_ASSERT_EQ_U64(locs.data[0].chr, 6U);
    YEW_ASSERT_EQ_U64(locs.data[0].end_line, 5U);
    YEW_ASSERT_EQ_U64(locs.data[0].end_chr, 10U);
    yew_lsp_locations_free(&locs);
}

void test_lsp_locations_ignore_malformed_and_non_file_rows(void)
{
    static const char json[] =
        "["
        "null,"
        "{\"uri\":\"https://example.test/x.c\",\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"uri\":\"file:///missing-range.c\"},"
        "{\"uri\":\"file:///negative.c\",\"range\":{"
        "\"start\":{\"line\":-1,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"uri\":\"file:///backwards.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":4},"
        "\"end\":{\"line\":2,\"character\":3}}},"
        "{\"uri\":\"file:///valid.c\",\"range\":{"
        "\"start\":{\"line\":7,\"character\":1},"
        "\"end\":{\"line\":7,\"character\":2}}}"
        "]";
    Vec_LspLoc locs = {0};

    nav_parse(json, &locs, 1U);
    YEW_ASSERT_EQ_U64(locs.len, 1U);
    YEW_ASSERT_EQ_STR(locs.data[0].path, "/valid.c");
    yew_lsp_locations_free(&locs);
}

static void write_all(int fd, const u8 *bytes, size_t len)
{
    size_t at = 0U;

    while (at < len) {
        ssize_t wrote = write(fd, bytes + at, len - at);

        YEW_ASSERT(wrote > 0);
        at += (size_t)wrote;
    }
}

static void set_open_in_here(Ed *ed)
{
    static const char here[] = "here";
    const char *err = NULL;
    OptVal value = {YEW_OPT_STR,
                    {.str = {here, (u32)(sizeof(here) - 1U)}}};

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, "lsp.open_in", 11U,
                           &value, &err));
    YEW_ASSERT_NULL(err);
}

void test_lsp_location_jump_hydrates_target_and_converts_target_utf16(void)
{
    static const u8 target_text[] = "x\xF0\x9F\x8C\xB2y\n";
    char path[] = "/tmp/yew-lsp-nav-XXXXXX";
    LspLoc loc = {path, 0U, 3U, 0U, 3U};
    Cursor *cursor;
    Buffer *target;
    Ed ed;
    int fd = mkstemp(path);

    YEW_ASSERT(fd >= 0);
    write_all(fd, target_text, sizeof(target_text) - 1U);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"abcdef\n", 7U,
                                  "origin.c"));
    set_open_in_here(&ed);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(2U);
    cursor->anchor = cursor->pos;

    YEW_ASSERT(yew_lsp_location_jump(&ed, ed.win, &loc,
                                     YEW_POSENC_UTF16));
    target = yew_ed_doc(&ed);
    YEW_ASSERT_NOT_NULL(target);
    YEW_ASSERT(yew_buf_resident(target));
    YEW_ASSERT_EQ_STR(target->path, path);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_NOT_NULL(cursor);
    YEW_ASSERT_EQ_U64(cursor->pos.v, 5U);
    YEW_ASSERT_EQ_U64(cursor->anchor.v, 5U);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 1U);

    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
}

void test_lsp_location_jump_does_not_push_when_target_hydration_fails(void)
{
    char path[] = "/tmp/yew-lsp-nav-dir-XXXXXX";
    LspLoc loc = {path, 0U, 0U, 0U, 0U};
    Ed ed;

    YEW_ASSERT_NOT_NULL(mkdtemp(path));
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"origin\n", 7U,
                                  "origin.c"));
    set_open_in_here(&ed);

    YEW_ASSERT(!yew_lsp_location_jump(&ed, ed.win, &loc,
                                      YEW_POSENC_UTF16));
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 0U);
    YEW_ASSERT_EQ_STR(yew_ed_doc(&ed)->name, "origin.c");

    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}
