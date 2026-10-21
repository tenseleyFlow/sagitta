#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/ctxmenu.h"
#include "ui/grouppicker.h"
#include "ui/picker.h"

static u32 callbacks;
static bool was_accepted;
static bool successor;

static void done(Ed *ed, bool accepted, const u8 *text, size_t len, void *ctx)
{
    (void)text;
    (void)len;
    (void)ctx;
    callbacks++;
    was_accepted = accepted;
    if (successor) {
        successor = false;
        yew_cmdline_open_modal(ed, "Successor", "", "next", true, done, NULL);
    }
}

static const PickItem *items(void *ctx, u32 *n)
{
    static const PickItem item = {"Choice", NULL, 1, 0U};

    (void)ctx;
    *n = 1U;
    return &item;
}

static bool accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    (void)ctx;
    (void)payload;
    (void)how;
    yew_cmdline_open_modal(ed, "Child", "", "", true, done, NULL);
    return true;
}

static CmdStatus open_dialog(CmdCtx *cx)
{
    switch (cx->iarg) {
    case YEW_MODAL_PICKER: {
        PickerSpec spec = {0};

        spec.title = "Choices";
        spec.items = items;
        spec.accept = accept;
        yew_picker_open(cx->ed, &spec);
        break;
    }
    case YEW_MODAL_GROUP:
        YEW_ASSERT(yew_gp_show(cx->ed, cx->ed->ws.dir));
        break;
    case YEW_MODAL_CONTEXT:
        YEW_ASSERT(yew_mouse_open_tab_menu(cx->ed,
            cx->ed->tabs.v.data[0].tab_id, 4U, 4U));
        break;
    case YEW_MODAL_PROMPT:
        yew_ed_prompt(cx->ed, YEW_PROMPT_QUIT_DIRTY);
        break;
    default:
        yew_cmdline_open_modal(cx->ed, "Draft", "", "", true, done, NULL);
        break;
    }
    return YEW_CMD_OK;
}

static void setup(Ed *ed, const char *seq, ModalKind kind)
{
    static const CmdEntry command = {
        {"ed.ai.rename", open_dialog, YEW_ARITY_OPT_INT,
         YEW_CMD_PROMPTS, "Modal routing fixture", NULL},
        "", YEW_RP_FORBID, NULL
    };
    BindRow row = {seq, "ed.ai.rename", kind, NULL};

    yew_ed_init(ed);
    ed->clean = true;
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 100U));
    ed->grid_ready = true;
    yew_ed_layout(ed);
    (void)yew_cmd_register_entry(&command);
    YEW_ASSERT(yew_keymap_build(&ed->user_keys, "modal fixture", &row, 1U));
    callbacks = 0U;
    was_accepted = false;
    successor = false;
}

static void cleanup(Ed *ed)
{
    yew_gp_close(ed);
    yew_picker_close(ed, false);
    yew_ctx_close();
    yew_ed_free(ed);
    yew_cmd_shutdown();
    yew_cmd_init();
}

static Key key_from_id(KeyId id)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = (u32)(id.v >> 16U);
    key.mods = (u16)id.v;
    if (key.code >= 32U && key.code < 127U && key.mods == 0U) {
        key.text[0] = (u8)key.code;
        key.ntext = 1U;
    }
    return key;
}

static void keys(Ed *ed, const char *seq)
{
    KeyId parsed[64];
    u32 n = yew_key_parse_seq(seq, parsed, YEW_ARRAY_LEN(parsed));
    u32 i;

    YEW_ASSERT(n != 0U);
    for (i = 0U; i < n; i++)
        yew_ed_handle_key(ed, key_from_id(parsed[i]), ed->now_ms + 1);
}

static void text_is(Ed *ed, const char *expected)
{
    Bytebuf text;

    bytebuf_init(&text);
    yew_cmdline_text(ed, &text);
    YEW_ASSERT_EQ_U64(text.len, strlen(expected));
    YEW_ASSERT_EQ_MEM(text.data, expected, text.len);
    bytebuf_free(&text);
}

void test_modalkeys_input_toggle_uses_actual_chord(void)
{
    static const char *const seqs[] = {"A-m", "C-x C-m", "t g"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(seqs); i++) {
        Ed ed;
        Win *doc;
        u64 dispatched;

        setup(&ed, seqs[i], YEW_MODAL_INPUT);
        doc = ed.win;
        keys(&ed, seqs[i]);
        YEW_ASSERT(ed.cmdline.modal);
        keys(&ed, "d r a f t <cr> b o d y");
        text_is(&ed, "draft\nbody");
        dispatched = ed.dispatch_count;
        keys(&ed, seqs[i]);
        YEW_ASSERT(!ed.cmdline.active);
        YEW_ASSERT_EQ_U64(callbacks, 1U);
        YEW_ASSERT(!was_accepted);
        YEW_ASSERT_EQ_U64(ed.dispatch_count, dispatched);
        YEW_ASSERT_EQ_U64(ed.mode, YEW_MODE_L);
        YEW_ASSERT(ed.win == doc);
        YEW_ASSERT_EQ_U64(yew_textbuf_len(doc->buf->tb), 0U);
        keys(&ed, seqs[i]);
        text_is(&ed, "");
        keys(&ed, "<esc>");
        YEW_ASSERT_EQ_U64(callbacks, 2U);
        cleanup(&ed);
    }
}

void test_modalkeys_chord_mismatch_timeout_and_replacement(void)
{
    Ed ed;
    i64 deadline;

    setup(&ed, "t g", YEW_MODAL_INPUT);
    keys(&ed, "t g");
    keys(&ed, "t x");
    text_is(&ed, "tx");
    keys(&ed, "t");
    deadline = yew_modal_keys_deadline(&ed);
    YEW_ASSERT(deadline > ed.now_ms);
    yew_ed_modal_tick(&ed, deadline - 1);
    text_is(&ed, "tx");
    yew_ed_modal_tick(&ed, deadline);
    text_is(&ed, "txt");
    YEW_ASSERT_EQ_I64(yew_modal_keys_deadline(&ed), -1);
    keys(&ed, "t");
    yew_ed_handle_paste(&ed, NULL, 0U, false);
    yew_ed_handle_paste(&ed, (const u8 *)"g", 1U, false);
    yew_ed_handle_paste(&ed, NULL, 0U, true);
    text_is(&ed, "txttg");
    /* A different modal replacing one with a held prefix gets no stale
     * text and does not acquire the old opener. */
    keys(&ed, "t");
    yew_cmdline_open_modal(&ed, "Replacement", "", "new", true, done, NULL);
    keys(&ed, "g");
    text_is(&ed, "newg");
    yew_ed_modal_tick(&ed, INT64_MAX);
    text_is(&ed, "newg");
    YEW_ASSERT_EQ_U64(ed.modal_keys.owner[YEW_MODAL_INPUT].n, 0U);
    keys(&ed, "<esc>");
    cleanup(&ed);
}

void test_modalkeys_held_key_and_callback_replacement(void)
{
    Ed ed;
    KeyId id;
    Key repeat;

    setup(&ed, "A-m", YEW_MODAL_INPUT);
    keys(&ed, "A-m");
    YEW_ASSERT_EQ_U64(yew_key_parse_seq("A-m", &id, 1U), 1U);
    repeat = key_from_id(id);
    repeat.ev = YEW_KEY_REPEAT;
    yew_ed_handle_key(&ed, repeat, 2);
    repeat.ev = YEW_KEY_RELEASE;
    yew_ed_handle_key(&ed, repeat, 3);
    YEW_ASSERT(ed.cmdline.modal);
    YEW_ASSERT_EQ_U64(callbacks, 0U);
    /* Ordinary m is still text. Another Alt action cannot run behind us. */
    keys(&ed, "m A-n");
    text_is(&ed, "m");
    successor = true;
    keys(&ed, "A-m");
    YEW_ASSERT_EQ_U64(callbacks, 1U);
    YEW_ASSERT(!was_accepted);
    YEW_ASSERT(ed.cmdline.modal);
    text_is(&ed, "next");
    keys(&ed, "A-m");
    YEW_ASSERT(!ed.cmdline.active);
    YEW_ASSERT_EQ_U64(callbacks, 2U);
    cleanup(&ed);
}

void test_modalkeys_picker_group_context_and_confirmation(void)
{
    static const ModalKind kinds[] = {YEW_MODAL_PICKER, YEW_MODAL_GROUP,
                                      YEW_MODAL_CONTEXT, YEW_MODAL_PROMPT};
    char dir[] = "/tmp/yew-modalkeys-XXXXXX";
    size_t i;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    for (i = 0U; i < YEW_ARRAY_LEN(kinds); i++) {
        Ed ed;

        setup(&ed, "A-g", kinds[i]);
        ed.ws.dir = arena_strdup(&ed.arena, dir);
        keys(&ed, "A-g");
        YEW_ASSERT_EQ_U64(ed.modal_keys.owner[kinds[i]].n, 1U);
        YEW_ASSERT(yew_gp_active() || yew_picker_active(&ed) ||
                   yew_ctx_active() || ed.prompt != YEW_PROMPT_NONE);
        if (kinds[i] == YEW_MODAL_PICKER) {
            YEW_ASSERT(yew_mouse_open_tab_menu(&ed,
                ed.tabs.v.data[0].tab_id, 4U, 4U));
            keys(&ed, "<esc>");
            YEW_ASSERT(!yew_ctx_active());
            YEW_ASSERT(yew_picker_active(&ed));
        }
        keys(&ed, "A-g");
        YEW_ASSERT(!yew_gp_active());
        YEW_ASSERT(!yew_picker_active(&ed));
        YEW_ASSERT(!yew_ctx_active());
        YEW_ASSERT_EQ_U64(ed.prompt, YEW_PROMPT_NONE);
        YEW_ASSERT(!ed.quit);
        YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), 0U);
        cleanup(&ed);
    }
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}

void test_modalkeys_picker_child_inherits_opener(void)
{
    Ed ed;

    setup(&ed, "A-g", YEW_MODAL_PICKER);
    keys(&ed, "A-g");
    keys(&ed, "<cr>");
    YEW_ASSERT(ed.cmdline.modal);
    YEW_ASSERT(!yew_picker_active(&ed));
    keys(&ed, "<cr>");
    text_is(&ed, "\n");
    keys(&ed, "A-g");
    YEW_ASSERT(!ed.cmdline.active);
    YEW_ASSERT_EQ_U64(callbacks, 1U);
    YEW_ASSERT(!was_accepted);
    keys(&ed, "A-g");
    /* Mouse/API accept must carry the opener just like keyboard Enter. */
    YEW_ASSERT(yew_picker_accept(&ed));
    YEW_ASSERT(ed.cmdline.modal);
    keys(&ed, "A-g");
    YEW_ASSERT(!ed.cmdline.active);
    YEW_ASSERT_EQ_U64(callbacks, 2U);
    cleanup(&ed);
}

void test_modalkeys_command_submission_is_not_an_opener(void)
{
    Ed ed;

    setup(&ed, "A-m", YEW_MODAL_INPUT);
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, "ai.rename");
    keys(&ed, "<cr>");
    YEW_ASSERT(ed.cmdline.modal);
    YEW_ASSERT_EQ_U64(ed.modal_keys.owner[YEW_MODAL_INPUT].n, 0U);
    keys(&ed, "<cr> m");
    text_is(&ed, "\nm");
    keys(&ed, "<esc>");
    YEW_ASSERT(!ed.cmdline.active);
    cleanup(&ed);
}
