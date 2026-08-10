/* Sprint 36 §7: the generated-state differential for the frozen v1 bytes. */
#include "harness.h"

#include <string.h>

#include "fl/data.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "ws/fllit.h"
#include "ws/state.h"

static u64 diff_rng(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void diff_emit_pane(FlEmit *e, const char *key, u32 depth)
{
    sag_fl_map_open(e, key);
    if (depth == 0U) {
        sag_fl_int(e, "win", 0);
    } else {
        sag_fl_str(e, "split", (depth & 1U) != 0U ? "h" : "v", 1U);
        sag_fl_int(e, "ratio_permille", 500);
        diff_emit_pane(e, "a", 0U);
        diff_emit_pane(e, "b", depth - 1U);
    }
    sag_fl_map_close(e);
}

static void diff_generated_doc(Bytebuf *raw, u32 serial, u64 *rng)
{
    static const char cjk[] = "/tmp/\xe7\x9f\xa2.txt";
    static const char odd[] = "/tmp/a b\"c\n.txt";
    static const char invalid[] = "/tmp/\xff.txt";
    const char *paths[] = {"/tmp/a.c", cjk, odd, invalid};
    FlEmit e;
    u32 ntabs = (u32)(diff_rng(rng) % 201U);
    u32 i;

    sag_fl_emit_init(&e, raw);
    sag_fl_map_open(&e, NULL);
    sag_fl_int(&e, "version", 1);
    sag_fl_str(&e, "writer", "sagitta", 7U);
    sag_fl_map_open(&e, "workspace");
    sag_fl_str(&e, "path", "/generated", 10U);
    sag_fl_int(&e, "saved_at", (i64)serial);
    sag_fl_map_close(&e);
    sag_fl_map_open(&e, "options");
    sag_fl_map_close(&e);
    sag_fl_list_open(&e, "groups");
    sag_fl_map_open(&e, NULL);
    sag_fl_int(&e, "id", 7);
    sag_fl_str(&e, "label", "generated", 9U);
    sag_fl_str(&e, "dir_path", "/generated", 10U);
    sag_fl_nil(&e, "last_active_member");
    sag_fl_map_close(&e);
    sag_fl_list_close(&e);
    sag_fl_list_open(&e, "tabs");
    for (i = 0U; i < ntabs; i++) {
        const char *path = paths[diff_rng(rng) % SAG_ARRAY_LEN(paths)];
        u32 ncursors = 1U + (u32)(diff_rng(rng) % 8U);
        u32 c;

        sag_fl_map_open(&e, NULL);
        sag_fl_int(&e, "id", (i64)i + 1);
        sag_fl_str(&e, "path", path, (u64)strlen(path));
        sag_fl_int(&e, "group", (i % 3U) == 0U ? 7 : 0);
        sag_fl_int(&e, "group_ordinal", (i64)i);
        sag_fl_bool(&e, "deferred", i != 0U);
        sag_fl_int(&e, "focus", 0);
        diff_emit_pane(&e, "panes", (u32)(diff_rng(rng) % 9U));
        sag_fl_list_open(&e, "wins");
        sag_fl_map_open(&e, NULL);
        sag_fl_list_open(&e, "cursors");
        for (c = 0U; c < ncursors; c++) {
            sag_fl_map_open(&e, NULL);
            sag_fl_int(&e, "pos", (i64)(diff_rng(rng) % 100000U));
            sag_fl_int(&e, "anchor", (i64)(diff_rng(rng) % 100000U));
            sag_fl_int(&e, "goal", c == 0U ? -1 : (i64)c);
            sag_fl_map_close(&e);
        }
        sag_fl_list_close(&e);
        sag_fl_int(&e, "primary", (i64)(ncursors - 1U));
        sag_fl_map_close(&e);
        sag_fl_list_close(&e);
        sag_fl_map_close(&e);
    }
    sag_fl_list_close(&e);
    sag_fl_int(&e, "active_tab", ntabs == 0U ? 0 : 1);
    sag_fl_list_open(&e, "files");
    sag_fl_list_close(&e);
    sag_fl_map_close(&e);
    sag_fl_emit_done(&e);
}

static void diff_reemit_lit(const FlLit *lit, Bytebuf *out)
{
    FlEmit e;

    sag_fl_emit_init(&e, out);
    sag_fl_emit_lit(&e, NULL, lit);
    sag_fl_emit_done(&e);
}

void test_state_diff_generated_500_matrix(void)
{
    u64 rng = 0x36f1e7c5a91b204dULL;
    u32 i;

    for (i = 0U; i < 500U; i++) {
        Arena old_arena;
        Arena new_arena;
        Arena value_arena;
        Interner in;
        DiagCtx dc;
        FlVm vm;
        FlParseErr old_err;
        FlParseErr new_err;
        FlLit *old_lit;
        FlLit *new_lit;
        FlValue value;
        Bytebuf raw;
        Bytebuf old_out;
        Bytebuf new_out;
        Bytebuf value_out;

        arena_init(&old_arena);
        arena_init(&new_arena);
        arena_init(&value_arena);
        bytebuf_init(&raw);
        bytebuf_init(&old_out);
        bytebuf_init(&new_out);
        bytebuf_init(&value_out);
        diff_generated_doc(&raw, i, &rng);
        old_lit = sag_fl_parse(&old_arena, raw.data, raw.len, &old_err);
        new_lit = sag_fl_parse_fletch(&new_arena, raw.data, raw.len,
                                      &new_err);
        SAG_ASSERT_NOT_NULL(old_lit);
        SAG_ASSERT_NOT_NULL(new_lit);
        diff_reemit_lit(old_lit, &old_out);
        diff_reemit_lit(new_lit, &new_out);
        SAG_ASSERT_EQ_U64(old_out.len, raw.len);
        SAG_ASSERT_EQ_MEM(old_out.data, raw.data, raw.len);
        SAG_ASSERT_EQ_U64(new_out.len, raw.len);
        SAG_ASSERT_EQ_MEM(new_out.data, raw.data, raw.len);

        interner_init(&in, &value_arena);
        fl_diag_init(&dc, &value_arena);
        (void)fl_vm_init(&vm, &value_arena, &in, &dc);
        value = fl_data_read(&vm, (const char *)raw.data, raw.len, &dc);
        SAG_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);
        fl_data_write(&value_out, value, 0U);
        SAG_ASSERT_EQ_U64(value_out.len, raw.len);
        SAG_ASSERT_EQ_MEM(value_out.data, raw.data, raw.len);
        fl_vm_free(&vm);
        interner_free(&in);
        arena_free_all(&value_arena);
        arena_free_all(&new_arena);
        arena_free_all(&old_arena);
        bytebuf_free(&value_out);
        bytebuf_free(&new_out);
        bytebuf_free(&old_out);
        bytebuf_free(&raw);
    }
}

void test_state_diff_data_writer_quotes_non_identifier_keys(void)
{
    static const char expected[] =
        "{\n  plain_key: 1,\n  \"history.scope\": 2,\n}\n";
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlMap *map;
    Bytebuf out;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_vm_init(&vm, &arena, &in, &dc);
    map = fl_map_new(&vm);
    (void)fl_map_set(&vm, map,
                     FL_OBJ_V(FL_STR, fl_str_new(&vm, "plain_key", 9U)),
                     FL_INT_V(1));
    (void)fl_map_set(&vm, map,
                     FL_OBJ_V(FL_STR, fl_str_new(&vm, "history.scope", 13U)),
                     FL_INT_V(2));
    bytebuf_init(&out);
    fl_data_write(&out, FL_OBJ_V(FL_MAP, map), 0U);
    SAG_ASSERT_EQ_U64(out.len, strlen(expected));
    SAG_ASSERT_EQ_MEM(out.data, expected, strlen(expected));
    bytebuf_free(&out);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
}

void test_state_diff_pure_literal_runs_nothing(void)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_vm_init(&vm, &arena, &in, &dc);
    {
        static const char source[] = "io.write(\"x\", \"bad\")";

        (void)fl_data_read(&vm, source, strlen(source), &dc);
    }
    SAG_ASSERT(fl_diag_errors(&dc) > 0U);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
}
