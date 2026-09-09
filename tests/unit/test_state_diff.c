/*
 * Sprint 36 §7 retained controls for the shipping Fletch data codec.
 *
 * The generated legacy-vs-Fletch matrix was retired in Sprint 58 after its
 * final 2x2 differential proof. These tests keep the writer's key spelling
 * and pure-literal boundary explicit without retaining the legacy codec.
 */
#include "harness.h"

#include <string.h>

#include "fl/data.h"
#include "fl/diag.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

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
    YEW_ASSERT_EQ_U64(out.len, strlen(expected));
    YEW_ASSERT_EQ_MEM(out.data, expected, strlen(expected));
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
    YEW_ASSERT(fl_diag_errors(&dc) > 0U);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
}
