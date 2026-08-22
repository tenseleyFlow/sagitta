/* Retained workspace booleans overlay the immutable schema-v1 options map. */
#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"

static void so_ed(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
}

static void so_apply_options(Ed *ed)
{
    static const char doc[] =
        "{ version: 1, options: { \"from.the.future\": 42, "
        "\"git.tree.all_files\": \"legacy\", }, groups: [], tabs: [], "
        "active_tab: 0, files: [], }";

    YEW_ASSERT_EQ_I64(yew_state_apply(ed, (const u8 *)doc,
                                      sizeof(doc) - 1U),
                      YEW_WS_FRESH);
}

void test_state_options_bool_overlay_preserves_unknown_literals(void)
{
    Ed ed;
    Arena a;
    Bytebuf out;
    FlParseErr err;
    FlLit *root;
    const FlLit *options;

    so_ed(&ed);
    so_apply_options(&ed);
    ed.state.ready = true;
    ed.state.writer = true;
    YEW_ASSERT(yew_state_option_bool_set(&ed, "git.tree.all_files", true));
    YEW_ASSERT(yew_state_option_bool_set(&ed, "git.tree.show_hidden", false));
    YEW_ASSERT(ed.state.dirty);
    YEW_ASSERT(ed.state.timer != YEW_TIMER_NONE);
    YEW_ASSERT(yew_state_option_bool(&ed, "git.tree.all_files", false));
    YEW_ASSERT(!yew_state_option_bool(&ed, "git.tree.show_hidden", true));

    arena_init(&a);
    bytebuf_init(&out);
    yew_state_emit(&ed, &out);
    root = yew_fl_parse(&a, out.data, out.len, &err);
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_I64(yew_fl_int_or(yew_fl_get(root, "version"), 0), 1);
    options = yew_fl_get(root, "options");
    YEW_ASSERT_EQ_I64(yew_fl_int_or(yew_fl_get(options,
                                               "from.the.future"), 0),
                      42);
    YEW_ASSERT(yew_fl_bool_or(yew_fl_get(options,
                                         "git.tree.all_files"), false));
    YEW_ASSERT(!yew_fl_bool_or(yew_fl_get(options,
                                          "git.tree.show_hidden"), true));
    bytebuf_free(&out);
    arena_free_all(&a);
    yew_ed_free(&ed);
}

void test_state_options_bool_overlay_is_sorted_and_idempotent(void)
{
    Ed ed;
    Bytebuf first;
    const u8 *all_at;
    const u8 *hidden_at;

    so_ed(&ed);
    ed.state.ready = true;
    ed.state.writer = true;
    YEW_ASSERT(yew_state_option_bool_set(&ed, "git.tree.show_hidden", true));
    YEW_ASSERT(yew_state_option_bool_set(&ed, "git.tree.all_files", false));
    YEW_ASSERT(!yew_state_option_bool_set(&ed, "git.tree.all_files", false));
    bytebuf_init(&first);
    yew_state_emit(&ed, &first);
    bytebuf_push_u8(&first, 0U);
    first.len--;
    all_at = (const u8 *)strstr((const char *)first.data,
                               "git.tree.all_files");
    hidden_at = (const u8 *)strstr((const char *)first.data,
                                  "git.tree.show_hidden");
    YEW_ASSERT_NOT_NULL(all_at);
    YEW_ASSERT_NOT_NULL(hidden_at);
    YEW_ASSERT(all_at < hidden_at);
    bytebuf_free(&first);
    yew_ed_free(&ed);
}
