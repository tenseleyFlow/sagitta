#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "fl/flruntime.h"
#include "fl/origin.h"
#include "fl/trace.h"
#include "fl/vm.h"
#include "util/buf.h"

static Key bind_key(char c)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = (u32)(u8)c;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

void test_bind_keymap_diagnostics_classify_all_six_failures(void)
{
    static const BindRow bad_seq[] = {{"<nope>", "ed.nop", 0, NULL}};
    static const BindRow unknown[] = {{"a", "ed.missing", 0, NULL}};
    static const BindRow duplicate[] = {
        {"a", "ed.nop", 0, NULL}, {"a", "ed.nop", 0, NULL}
    };
    static const BindRow arity[] = {{"a", "ed.mode.enter", 0, NULL}};
    static const BindRow escape[] = {{"<esc> a", "ed.nop", 0, NULL}};
    static const BindRow long_seq[] = {
        {"a b c d e f g h i", "ed.nop", 0, NULL}
    };
    static const struct {
        const BindRow *rows;
        u32 n;
        YewKeymapError error;
        const char *text;
    } cases[] = {
        {bad_seq, YEW_ARRAY_LEN(bad_seq), YEW_KEYMAP_ERR_SEQUENCE,
         "invalid key sequence"},
        {unknown, YEW_ARRAY_LEN(unknown), YEW_KEYMAP_ERR_COMMAND,
         "unknown command"},
        {duplicate, YEW_ARRAY_LEN(duplicate), YEW_KEYMAP_ERR_DUPLICATE,
         "duplicate key sequence"},
        {arity, YEW_ARRAY_LEN(arity), YEW_KEYMAP_ERR_ARITY,
         "command arguments do not match its arity"},
        {escape, YEW_ARRAY_LEN(escape), YEW_KEYMAP_ERR_ESCAPE_PREFIX,
         "<esc> may not be a chord prefix"},
        {long_seq, YEW_ARRAY_LEN(long_seq), YEW_KEYMAP_ERR_TOO_LONG,
         "key sequence exceeds eight keys"},
    };
    u32 i;

    yew_cmd_init();
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&bad_seq[0]),
                      YEW_KEYMAP_ERR_SEQUENCE);
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&unknown[0]),
                      YEW_KEYMAP_ERR_COMMAND);
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&duplicate[0]),
                      YEW_KEYMAP_ERR_NONE);
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&arity[0]),
                      YEW_KEYMAP_ERR_ARITY);
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&escape[0]),
                      YEW_KEYMAP_ERR_ESCAPE_PREFIX);
    YEW_ASSERT_EQ_U64(yew_keymap_validate_row(&long_seq[0]),
                      YEW_KEYMAP_ERR_TOO_LONG);
    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        Keymap map = {0};
        YewKeymapDiag diag = {99U, YEW_KEYMAP_ERR_NONE};

        YEW_ASSERT(!yew_keymap_build_diag(&map, "invalid", cases[i].rows,
                                           cases[i].n, &diag));
        YEW_ASSERT_EQ_U64(diag.error, cases[i].error);
        YEW_ASSERT_EQ_STR(yew_keymap_error_string(diag.error), cases[i].text);
        YEW_ASSERT_EQ_U64(map.nodes.len, 0U);
        yew_keymap_free(&map);
    }
}

static bool trace_has(Ed *ed, const char *needle)
{
    Bytebuf trace;
    bool found;

    bytebuf_init(&trace);
    fl_trace_render(yew_fl_vm(ed), yew_fl_vm(ed)->err, &trace);
    bytebuf_push_u8(&trace, 0U);
    found = strstr((const char *)trace.data, needle) != NULL;
    bytebuf_free(&trace);
    return found;
}

void test_bind_fletch_validation_errors_carry_source_positions(void)
{
    static const char *const source[] = {
        "bind(\"L\", \"<nope>\", \"ed.nop\")",
        "bind(\"L\", \"a\", \"ed.missing\")",
        "bind(\"L\", \"a\", \"ed.mode.enter\")",
        "bind(\"L\", \"<esc> a\", \"ed.nop\")",
        "bind(\"L\", \"a b c d e f g h i\", \"ed.nop\")",
    };
    Ed ed;
    u32 i;
    static const char duplicate[] = "bind(\"L\", \"a\", \"ed.nop\")";

    yew_ed_init(&ed);
    for (i = 0U; i < YEW_ARRAY_LEN(source); i++) {
        YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, source[i],
                                      (u32)strlen(source[i])),
                          YEW_CMD_ERR_STATE);
        YEW_ASSERT(trace_has(&ed, "at <repl> (<E>:1:"));
        YEW_ASSERT(trace_has(&ed, "^"));
    }
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, duplicate, sizeof(duplicate) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, duplicate, sizeof(duplicate) - 1U),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT(trace_has(&ed, "duplicate key sequence"));
    YEW_ASSERT(trace_has(&ed, "at <repl> (<E>:1:"));
    yew_ed_free(&ed);
}

void test_bind_closure_dispatches_from_frozen_mode_layer(void)
{
    static const char script[] =
        "bind(\"L\", \"Z\", fn() set({ errorbells: true }))";
    Ed ed;
    KeyId id;
    const Binding *binding = NULL;
    CmdCtx map = {0};
    const char *shown;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT(!ed.errorbells);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, script, sizeof(script) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 1U);
    YEW_ASSERT_EQ_U64(ed.keys.n, 3U);
    YEW_ASSERT_EQ_U64(yew_key_parse_seq("Z", &id, 1U), 1U);
    YEW_ASSERT_EQ_I64(yew_keymap_lookup(&ed.bind_keys[YEW_MODE_L], &id, 1U,
                                        NULL, &binding), YEW_MATCH_FULL);
    YEW_ASSERT_NOT_NULL(binding);
    YEW_ASSERT_EQ_STR(yew_cmd_desc(binding->cmd)->name, "ed.fl.closure");
    map.ed = &ed;
    YEW_ASSERT_EQ_I64(yew_bind_cmd_map(&map), YEW_CMD_OK);
    shown = ed.msg.full == NULL ? ed.msg.text : ed.msg.full;
    YEW_ASSERT_NOT_NULL(strstr(shown, "Z  ed.fl.closure"));
    yew_ed_handle_key(&ed, bind_key('Z'), 10);
    YEW_ASSERT(ed.errorbells);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    yew_ed_free(&ed);
}

void test_bind_origin_ownership_copies_rows_and_batches_one_rebuild(void)
{
    Ed ed;
    CmdId nop;
    CmdId enter;
    u32 plugin;
    u32 first;
    u32 second;
    u32 before;
    char seq[] = "X";
    char sarg[] = "owned";
    KeyId key;
    const Binding *binding = NULL;
    static const char unbind_src[] = "unbind(\"L\", \"X\")";

    yew_ed_init(&ed);
    nop = yew_cmd_lookup("ed.nop", 6U);
    enter = yew_cmd_lookup("ed.mode.enter", 13U);
    plugin = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "plug:test", 0U);
    before = yew_bind_rebuild_count(&ed);
    yew_bind_batch_begin(&ed);
    first = yew_bind_add(&ed, plugin, YEW_MODE_L, seq, enter, 0, sarg,
                         FL_NIL_V);
    second = yew_bind_add(&ed, plugin, YEW_MODE_W, "Y", nop, 0, NULL,
                          FL_NIL_V);
    YEW_ASSERT(first != 0U);
    YEW_ASSERT(second != 0U);
    YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&ed), before);
    seq[0] = 'Q';
    sarg[0] = 'x';
    yew_bind_batch_end(&ed);
    YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&ed), before + 1U);
    YEW_ASSERT_EQ_U64(yew_key_parse_seq("X", &key, 1U), 1U);
    YEW_ASSERT_EQ_I64(yew_keymap_lookup(&ed.bind_keys[YEW_MODE_L], &key, 1U,
                                        NULL, &binding), YEW_MATCH_FULL);
    YEW_ASSERT_NOT_NULL(binding);
    YEW_ASSERT_EQ_STR(binding->sarg, "owned");
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, unbind_src,
                                  sizeof(unbind_src) - 1U),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT(trace_has(&ed, "owned by plug:test"));
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 2U);
    YEW_ASSERT(yew_bind_remove(&ed, first));
    YEW_ASSERT(!yew_bind_remove(&ed, first));
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 1U);
    yew_ed_free(&ed);
}

void test_bind_later_origin_shadows_then_teardown_reveals_prior(void)
{
    Ed ed;
    CmdId nop;
    CmdId enter;
    u32 builtin;
    u32 workspace;
    u32 lower;
    u32 upper;
    KeyId key;
    const Binding *binding = NULL;

    yew_ed_init(&ed);
    nop = yew_cmd_lookup("ed.nop", 6U);
    enter = yew_cmd_lookup("ed.mode.enter", 13U);
    builtin = fl_origin_register(&ed, FL_ORIGIN_BUILTIN,
                                 "/runtime/init.fl", 0U);
    workspace = fl_origin_register(&ed, FL_ORIGIN_WORKSPACE,
                                   "/work/.yew.fl", 0U);
    yew_bind_batch_begin(&ed);
    lower = yew_bind_add(&ed, builtin, YEW_MODE_L, "Z", nop, 0, NULL,
                         FL_NIL_V);
    upper = yew_bind_add(&ed, workspace, YEW_MODE_L, "Z", enter, 0, "W",
                         FL_NIL_V);
    yew_bind_batch_end(&ed);
    YEW_ASSERT(lower != 0U);
    YEW_ASSERT(upper != 0U);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 2U);
    YEW_ASSERT_EQ_U64(yew_key_parse_seq("Z", &key, 1U), 1U);
    YEW_ASSERT_EQ_I64(yew_keymap_lookup(&ed.bind_keys[YEW_MODE_L], &key,
                                        1U, NULL, &binding),
                      YEW_MATCH_FULL);
    YEW_ASSERT_NOT_NULL(binding);
    YEW_ASSERT_EQ_STR(yew_cmd_desc(binding->cmd)->name, "ed.mode.enter");
    YEW_ASSERT_EQ_STR(binding->sarg, "W");
    YEW_ASSERT(yew_bind_remove(&ed, upper));
    binding = NULL;
    YEW_ASSERT_EQ_I64(yew_keymap_lookup(&ed.bind_keys[YEW_MODE_L], &key,
                                        1U, NULL, &binding),
                      YEW_MATCH_FULL);
    YEW_ASSERT_NOT_NULL(binding);
    YEW_ASSERT_EQ_STR(yew_cmd_desc(binding->cmd)->name, "ed.nop");
    YEW_ASSERT(yew_bind_remove(&ed, lower));
    yew_ed_free(&ed);
}
