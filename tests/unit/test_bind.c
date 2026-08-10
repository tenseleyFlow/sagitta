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

    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
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
        SagKeymapError error;
        const char *text;
    } cases[] = {
        {bad_seq, SAG_ARRAY_LEN(bad_seq), SAG_KEYMAP_ERR_SEQUENCE,
         "invalid key sequence"},
        {unknown, SAG_ARRAY_LEN(unknown), SAG_KEYMAP_ERR_COMMAND,
         "unknown command"},
        {duplicate, SAG_ARRAY_LEN(duplicate), SAG_KEYMAP_ERR_DUPLICATE,
         "duplicate key sequence"},
        {arity, SAG_ARRAY_LEN(arity), SAG_KEYMAP_ERR_ARITY,
         "command arguments do not match its arity"},
        {escape, SAG_ARRAY_LEN(escape), SAG_KEYMAP_ERR_ESCAPE_PREFIX,
         "<esc> may not be a chord prefix"},
        {long_seq, SAG_ARRAY_LEN(long_seq), SAG_KEYMAP_ERR_TOO_LONG,
         "key sequence exceeds eight keys"},
    };
    u32 i;

    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        Keymap map = {0};
        SagKeymapDiag diag = {99U, SAG_KEYMAP_ERR_NONE};

        SAG_ASSERT(!sag_keymap_build_diag(&map, "invalid", cases[i].rows,
                                           cases[i].n, &diag));
        SAG_ASSERT_EQ_U64(diag.error, cases[i].error);
        SAG_ASSERT_EQ_STR(sag_keymap_error_string(diag.error), cases[i].text);
        SAG_ASSERT_EQ_U64(map.nodes.len, 0U);
        sag_keymap_free(&map);
    }
}

static bool trace_has(Ed *ed, const char *needle)
{
    Bytebuf trace;
    bool found;

    bytebuf_init(&trace);
    fl_trace_render(sag_fl_vm(ed), sag_fl_vm(ed)->err, &trace);
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

    sag_ed_init(&ed);
    for (i = 0U; i < SAG_ARRAY_LEN(source); i++) {
        SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, source[i],
                                      (u32)strlen(source[i])),
                          SAG_CMD_ERR_STATE);
        SAG_ASSERT(trace_has(&ed, "at <repl> (<E>:1:"));
        SAG_ASSERT(trace_has(&ed, "^"));
    }
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, duplicate, sizeof(duplicate) - 1U),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, duplicate, sizeof(duplicate) - 1U),
                      SAG_CMD_ERR_STATE);
    SAG_ASSERT(trace_has(&ed, "duplicate key sequence"));
    SAG_ASSERT(trace_has(&ed, "at <repl> (<E>:1:"));
    sag_ed_free(&ed);
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

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT(!ed.errorbells);
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, script, sizeof(script) - 1U),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&ed), 1U);
    SAG_ASSERT_EQ_U64(ed.keys.n, 3U);
    SAG_ASSERT_EQ_U64(sag_key_parse_seq("Z", &id, 1U), 1U);
    SAG_ASSERT_EQ_I64(sag_keymap_lookup(&ed.bind_keys[SAG_MODE_L], &id, 1U,
                                        NULL, &binding), SAG_MATCH_FULL);
    SAG_ASSERT_NOT_NULL(binding);
    SAG_ASSERT_EQ_STR(sag_cmd_desc(binding->cmd)->name, "ed.fl.closure");
    map.ed = &ed;
    SAG_ASSERT_EQ_I64(sag_bind_cmd_map(&map), SAG_CMD_OK);
    shown = ed.msg.full == NULL ? ed.msg.text : ed.msg.full;
    SAG_ASSERT_NOT_NULL(strstr(shown, "Z  ed.fl.closure"));
    sag_ed_handle_key(&ed, bind_key('Z'), 10);
    SAG_ASSERT(ed.errorbells);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, 1U);
    sag_ed_free(&ed);
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

    sag_ed_init(&ed);
    nop = sag_cmd_lookup("ed.nop", 6U);
    enter = sag_cmd_lookup("ed.mode.enter", 13U);
    plugin = fl_origin_register(&ed, FL_ORIGIN_PLUGIN, "plug:test", 0U);
    before = sag_bind_rebuild_count(&ed);
    sag_bind_batch_begin(&ed);
    first = sag_bind_add(&ed, plugin, SAG_MODE_L, seq, enter, 0, sarg,
                         FL_NIL_V);
    second = sag_bind_add(&ed, plugin, SAG_MODE_W, "Y", nop, 0, NULL,
                          FL_NIL_V);
    SAG_ASSERT(first != 0U);
    SAG_ASSERT(second != 0U);
    SAG_ASSERT_EQ_U64(sag_bind_rebuild_count(&ed), before);
    seq[0] = 'Q';
    sarg[0] = 'x';
    sag_bind_batch_end(&ed);
    SAG_ASSERT_EQ_U64(sag_bind_rebuild_count(&ed), before + 1U);
    SAG_ASSERT_EQ_U64(sag_key_parse_seq("X", &key, 1U), 1U);
    SAG_ASSERT_EQ_I64(sag_keymap_lookup(&ed.bind_keys[SAG_MODE_L], &key, 1U,
                                        NULL, &binding), SAG_MATCH_FULL);
    SAG_ASSERT_NOT_NULL(binding);
    SAG_ASSERT_EQ_STR(binding->sarg, "owned");
    SAG_ASSERT_EQ_I64(sag_fl_eval(&ed, unbind_src,
                                  sizeof(unbind_src) - 1U),
                      SAG_CMD_ERR_STATE);
    SAG_ASSERT(trace_has(&ed, "owned by plug:test"));
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&ed), 2U);
    SAG_ASSERT(sag_bind_remove(&ed, first));
    SAG_ASSERT(!sag_bind_remove(&ed, first));
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&ed), 1U);
    sag_ed_free(&ed);
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

    sag_ed_init(&ed);
    nop = sag_cmd_lookup("ed.nop", 6U);
    enter = sag_cmd_lookup("ed.mode.enter", 13U);
    builtin = fl_origin_register(&ed, FL_ORIGIN_BUILTIN,
                                 "/runtime/init.fl", 0U);
    workspace = fl_origin_register(&ed, FL_ORIGIN_WORKSPACE,
                                   "/work/.sagitta.fl", 0U);
    sag_bind_batch_begin(&ed);
    lower = sag_bind_add(&ed, builtin, SAG_MODE_L, "Z", nop, 0, NULL,
                         FL_NIL_V);
    upper = sag_bind_add(&ed, workspace, SAG_MODE_L, "Z", enter, 0, "W",
                         FL_NIL_V);
    sag_bind_batch_end(&ed);
    SAG_ASSERT(lower != 0U);
    SAG_ASSERT(upper != 0U);
    SAG_ASSERT_EQ_U64(sag_bind_active_count(&ed), 2U);
    SAG_ASSERT_EQ_U64(sag_key_parse_seq("Z", &key, 1U), 1U);
    SAG_ASSERT_EQ_I64(sag_keymap_lookup(&ed.bind_keys[SAG_MODE_L], &key,
                                        1U, NULL, &binding),
                      SAG_MATCH_FULL);
    SAG_ASSERT_NOT_NULL(binding);
    SAG_ASSERT_EQ_STR(sag_cmd_desc(binding->cmd)->name, "ed.mode.enter");
    SAG_ASSERT_EQ_STR(binding->sarg, "W");
    SAG_ASSERT(sag_bind_remove(&ed, upper));
    binding = NULL;
    SAG_ASSERT_EQ_I64(sag_keymap_lookup(&ed.bind_keys[SAG_MODE_L], &key,
                                        1U, NULL, &binding),
                      SAG_MATCH_FULL);
    SAG_ASSERT_NOT_NULL(binding);
    SAG_ASSERT_EQ_STR(sag_cmd_desc(binding->cmd)->name, "ed.nop");
    SAG_ASSERT(sag_bind_remove(&ed, lower));
    sag_ed_free(&ed);
}
