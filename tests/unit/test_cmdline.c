#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "ui/cmdline.h"
#include "ui/region.h"
#include "util/buf.h"

#define FAMILY                                                              \
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"                \
    "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"                    \
    "\xf0\x9f\x91\xa6"

typedef struct CmdlineFixture {
    Ed ed;
    char state[64];
    char *saved_state;
} CmdlineFixture;

typedef struct InputDoneState {
    u32 calls;
    bool accepted;
    bool closed;
    size_t len;
    u8 text[32];
} InputDoneState;

static void input_done(Ed *ed, bool accepted, const u8 *text, size_t len,
                       void *ctx)
{
    InputDoneState *state = ctx;

    state->calls++;
    state->accepted = accepted;
    state->closed = !ed->cmdline.active && ed->mode == YEW_MODE_L;
    state->len = len;
    YEW_ASSERT(len <= sizeof(state->text));
    if (len != 0U)
        (void)memcpy(state->text, text, len);
}

static char *cmdline_dup(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void cmdline_fixture_init(CmdlineFixture *fixture)
{
    const char *state = getenv("XDG_STATE_HOME");

    fixture->saved_state = state == NULL ? NULL : cmdline_dup(state);
    (void)snprintf(fixture->state, sizeof(fixture->state),
                   "/tmp/yew-cmdline-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fixture->state));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->state, 1), 0);
    yew_ed_init(&fixture->ed);
    YEW_ASSERT(yew_ed_open_scratch(&fixture->ed));
    yew_test_load_runtime(&fixture->ed);
}

static void cmdline_fixture_free(CmdlineFixture *fixture)
{
    static const char *const names[] = {"cmd", "cmd.lock", "input",
                                        "input.lock"};
    char path[160];
    size_t i;

    yew_ed_free(&fixture->ed);
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        (void)snprintf(path, sizeof(path), "%s/yew/history/%s",
                       fixture->state, names[i]);
        if (unlink(path) != 0)
            YEW_ASSERT_EQ_I64(errno, ENOENT);
    }
    (void)snprintf(path, sizeof(path), "%s/yew/history",
                   fixture->state);
    if (rmdir(path) != 0)
        YEW_ASSERT_EQ_I64(errno, ENOENT);
    (void)snprintf(path, sizeof(path), "%s/yew", fixture->state);
    if (rmdir(path) != 0)
        YEW_ASSERT_EQ_I64(errno, ENOENT);
    YEW_ASSERT_EQ_I64(rmdir(fixture->state), 0);
    if (fixture->saved_state != NULL)
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->saved_state, 1),
                          0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    free(fixture->saved_state);
}

static Bytebuf cmdline_text(const CmdLine *line)
{
    Bytebuf out;
    TextIter iter;

    bytebuf_init(&out);
    if (yew_textiter_begin(&iter, line->buf, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&iter, line->buf, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (yew_textiter_advance(&iter, line->buf));
    }
    bytebuf_push_u8(&out, 0U);
    return out;
}

static Bytebuf document_text(const Ed *ed)
{
    CmdLine view = {0};

    view.buf = ed->buffer.tb;
    return cmdline_text(&view);
}

static void cmdline_document(CmdlineFixture *fixture, const char *text)
{
    Cursor *cursor;

    yew_undo_free(fixture->ed.buffer.undo);
    yew_textbuf_free(fixture->ed.buffer.tb);
    fixture->ed.buffer.tb =
        yew_textbuf_from_bytes((const u8 *)text, strlen(text));
    fixture->ed.buffer.undo = yew_undo_new(fixture->ed.buffer.tb);
    cursor = yew_ed_cursor(&fixture->ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
}

static Key cmdline_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static CmdStatus cmdline_invoke(Ed *ed, CmdStatus (*command)(CmdCtx *))
{
    CmdCtx context = {0};

    context.ed = ed;
    context.win = yew_cmdline_target(ed);
    context.source = YEW_SRC_TEST;
    return command(&context);
}

static CmdStatus cmdline_noop(CmdCtx *context)
{
    (void)context;
    return YEW_CMD_OK;
}

void test_cmdline_reuses_textbuf_and_grapheme_cursor(void)
{
    CmdlineFixture fixture;
    Win *target;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, FAMILY);
    target = yew_cmdline_target(&fixture.ed);
    YEW_ASSERT_NOT_NULL(target);
    YEW_ASSERT_NOT_NULL(target->buf);
    YEW_ASSERT_EQ_U64(target->buf->tb, fixture.ed.cmdline.buf);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(fixture.ed.cmdline.buf), 1U);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, sizeof(FAMILY) - 1U);

    yew_ed_handle_key(&fixture.ed, cmdline_key(YEW_KEY_LEFT), 1);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 0U);
    YEW_ASSERT_EQ_U64(target->cs.curs.data[target->cs.primary].pos.v, 0U);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_sanitizes_seed_paste_and_register_newlines(void)
{
    CmdlineFixture fixture;
    CmdCtx context = {0};
    RegVal value;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "a\r\n\nb\rc");
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "a b c");
    bytebuf_free(&text);

    yew_cmdline_paste(&fixture.ed, (const u8 *)"x\r\ny\n\nz",
                      sizeof("x\r\ny\n\nz") - 1U);
    yew_regval_init(&value);
    bytebuf_append(&value.bytes, "q\r\n\nr", sizeof("q\r\n\nr") - 1U);
    yew_reg_set(&fixture.ed.regs, (u8)'a', &value);
    yew_regval_free(&value);
    context.ed = &fixture.ed;
    context.win = yew_cmdline_target(&fixture.ed);
    context.sarg = "a";
    context.sarg_len = 1U;
    context.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_cmdline_cmd_insert_register(&context), YEW_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "a b cx y zq r");
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(fixture.ed.cmdline.buf), 1U);
    bytebuf_free(&text);
    yew_regval_init(&value);
    bytebuf_append(&value.bytes, "n\0x", 3U);
    yew_reg_set(&fixture.ed.regs, (u8)'b', &value);
    yew_regval_free(&value);
    context.sarg = "b";
    YEW_ASSERT_EQ_U64(yew_cmdline_cmd_insert_register(&context),
                      YEW_CMD_ERR_ARG);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "a b cx y zq r");
    YEW_ASSERT_EQ_STR(fixture.ed.msg.text,
                      "NUL byte is not valid in a command line");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

typedef struct CmdlineLeafCheck {
    u32 visited;
} CmdlineLeafCheck;

static bool cmdline_check_leaf(const KeyId *keys, u32 key_count,
                               const Binding *binding, void *opaque)
{
    CmdlineLeafCheck *check = opaque;
    const CmdDesc *description = yew_cmd_desc(binding->cmd);

    YEW_ASSERT_NOT_NULL(keys);
    YEW_ASSERT(key_count != 0U);
    YEW_ASSERT_NOT_NULL(description);
    YEW_ASSERT(strncmp(description->name, "ed.", 3U) == 0);
    check->visited++;
    return true;
}

void test_cmdline_e_keymap_leaves_are_registered_editor_commands(void)
{
    CmdlineFixture fixture;
    CmdlineLeafCheck check = {0};
    const Keymap *map;

    cmdline_fixture_init(&fixture);
    map = &fixture.ed.bind_keys[YEW_MODE_E];
    YEW_ASSERT(yew_keymap_visit(map, cmdline_check_leaf, &check));
    YEW_ASSERT_EQ_U64(check.visited, yew_keymap_binding_count(map));
    YEW_ASSERT(check.visited >= 20U);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_highlight_selection_seeds_range(void)
{
    CmdlineFixture fixture;
    Cursor *cursor;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_textbuf_insert(fixture.ed.buffer.tb, BYTEOFF(0U),
                       (const u8 *)"alpha", 5U);
    cursor = yew_ed_cursor(&fixture.ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->anchor = BYTEOFF(1U);
    cursor->pos = BYTEOFF(4U);
    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&fixture.ed, YEW_MODE_L,
                                               false), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_mode_enter(&fixture.ed, YEW_MODE_E), YEW_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "'<,'>");
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 5U);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.return_mode, YEW_MODE_H);
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_escape_restores_completion_stem_before_closing(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "f");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 1U);
    YEW_ASSERT_NOT_NULL(fixture.ed.cmdline.comp_arena.head);
    YEW_ASSERT(fixture.ed.cmdline.active);

    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_cancel),
                      YEW_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "f");
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);
    YEW_ASSERT_NULL(fixture.ed.cmdline.comp_arena.head);
    YEW_ASSERT(fixture.ed.cmdline.active);
    bytebuf_free(&text);

    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_cancel),
                      YEW_CMD_OK);
    YEW_ASSERT(!fixture.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(fixture.ed.mode, YEW_MODE_L);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_accepts_registered_command_and_closes(void)
{
    CmdlineFixture fixture;
    InputDoneState state = {0};

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "redraw");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT(!fixture.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(fixture.ed.mode, YEW_MODE_L);

    yew_cmdline_open_input(&fixture.ed, "wolf name", input_done, &state);
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(state.calls, 1U);
    YEW_ASSERT(state.accepted);
    YEW_ASSERT(state.closed);
    YEW_ASSERT_EQ_U64(state.len, 9U);
    YEW_ASSERT_EQ_MEM(state.text, "wolf name", 9U);

    (void)memset(&state, 0, sizeof(state));
    yew_cmdline_open_input(&fixture.ed, "old", input_done, &state);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_INPUT, "picker");
    YEW_ASSERT_EQ_U64(state.calls, 1U);
    YEW_ASSERT(!state.accepted);
    YEW_ASSERT(state.closed);
    YEW_ASSERT_EQ_U64(state.len, 3U);
    YEW_ASSERT_EQ_MEM(state.text, "old", 3U);
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(state.calls, 1U);
    YEW_ASSERT(!fixture.ed.cmdline.active);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_range_executes_once_with_multiple_cursors(void)
{
    CmdlineFixture fixture;
    Cursor second = {BYTEOFF(8U), {0U}, BYTEOFF(8U)};
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    cmdline_document(&fixture, "one\ntwo\nthree\n");
    YEW_ASSERT(yew_cset_add(&fixture.ed.win->cs, second));
    YEW_ASSERT_EQ_U64(fixture.ed.win->cs.curs.len, 2U);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "1,2d");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    text = document_text(&fixture.ed);
    YEW_ASSERT_EQ_STR((const char *)text.data, "three\n");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_bang_quit_bypasses_durability_failure(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    cmdline_document(&fixture, "dirty");
    fixture.ed.durability_failed = true;
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "q!");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT(fixture.ed.quit);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_parse_error_preserves_text_and_points_at_token(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "definitely_missing");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT(fixture.ed.cmdline.active);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "definitely_missing");
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v,
                      fixture.ed.cmdline.err.tok_lo);
    YEW_ASSERT(fixture.ed.cmdline.err.tok_hi >
               fixture.ed.cmdline.err.tok_lo);
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.err.msg,
                      "unknown command 'definitely_missing' (try Tab)");
    YEW_ASSERT(fixture.ed.msg.active);
    YEW_ASSERT(strncmp(fixture.ed.msg.text, "E: ", 3U) == 0);
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_catalogue_errors_preserve_live_prompt(void)
{
    static const struct {
        const char *line;
        const char *message;
        u32 lo;
        u32 hi;
    } rows[] = {
        {"not_a_command", "unknown command 'not_a_command' (try Tab)",
         0U, 13U},
        {"file.w", "ambiguous: file.write, file.write_quit", 0U, 6U},
        {"file.open", ":file.open requires 1 argument", 0U, 9U},
        {"quit extra", ":quit takes no arguments", 5U, 10U},
        {"1quit", ":quit takes no range", 0U, 1U},
        {"ui.grow", ":ui.grow requires a range", 0U, 7U},
        {"5,3d", "backwards range (5,3)", 0U, 3U},
        {"900d", "line 900 past end of buffer (412 lines)", 0U, 3U},
        {"w \"abc", "unterminated \"", 2U, 3U},
        {"w \"\\q\"", "unknown escape '\\q'", 3U, 5U},
        {"w %z", "unknown expansion '%z'", 2U, 4U},
        {"w %", "buffer has no file name", 2U, 3U},
        {"w %s", "%s needs a selection", 2U, 4U},
        {"!", "ed.shell.run needs a command", 0U, 1U},
        {"mode.enter I", "unknown command 'mode.enter' (try Tab)",
         0U, 10U},
    };
    CmdlineFixture fixture;
    CmdEntry required = {
        {"ed.ui.grow", cmdline_noop, YEW_ARITY_NONE, 0U,
         "Command-line error test command", NULL},
        "", YEW_RP_REQUIRED, NULL};
    char document[412];
    size_t i;

    cmdline_fixture_init(&fixture);
    (void)memset(document, '\n', sizeof(document) - 1U);
    document[sizeof(document) - 1U] = '\0';
    cmdline_document(&fixture, document);
    (void)yew_cmd_register_entry(&required);

    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        Bytebuf text;
        char expected[sizeof(fixture.ed.msg.text)];

        yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, rows[i].line);
        YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                         yew_cmdline_cmd_accept),
                          YEW_CMD_ERR_ARG);
        YEW_ASSERT(fixture.ed.cmdline.active);
        text = cmdline_text(&fixture.ed.cmdline);
        YEW_ASSERT_EQ_STR((const char *)text.data, rows[i].line);
        YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, rows[i].lo);
        YEW_ASSERT_EQ_U64(fixture.ed.cmdline.err.tok_lo, rows[i].lo);
        YEW_ASSERT_EQ_U64(fixture.ed.cmdline.err.tok_hi, rows[i].hi);
        YEW_ASSERT_EQ_STR(fixture.ed.cmdline.err.msg, rows[i].message);
        YEW_ASSERT(fixture.ed.msg.active);
        (void)snprintf(expected, sizeof(expected), "E: %s",
                       rows[i].message);
        YEW_ASSERT_EQ_STR(fixture.ed.msg.text, expected);
        bytebuf_free(&text);
        yew_cmdline_close(&fixture.ed, false);
    }
    cmdline_fixture_free(&fixture);
    yew_cmd_shutdown();
    yew_cmd_init();
}

static void cmdline_type(CmdlineFixture *fixture, const char *text)
{
    const char *p;

    for (p = text; *p != '\0'; p++)
        yew_ed_handle_key(&fixture->ed, cmdline_key((u32)(u8)*p), 1);
}

void test_cmdline_menu_filters_live_while_typing(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    /* A bare `:` has no token to filter on, so no list -- "every command
     * in the registry" is noise, not an answer. */
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);

    /* Sprint 18 needed a Tab to see anything here. */
    cmdline_type(&fixture, "fil");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 1U);
    /* Filtering ranked a row first but the user has chosen nothing. */
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, -1);
    YEW_ASSERT(!fixture.ed.cmdline.menu.explicit_sel);

    /* Typing narrows rather than dismissing, which is the inversion of
     * s18's "any printable key closes the menu". */
    cmdline_type(&fixture, "e.wri");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len >= 1U);

    /* And a token that matches nothing closes it -- silently. */
    cmdline_type(&fixture, "zzzz");
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);
    YEW_ASSERT(!(fixture.ed.msg.active &&
                 fixture.ed.msg.sev == YEW_MSG_ERROR));
    cmdline_fixture_free(&fixture);
}

void test_cmdline_enter_executes_while_filtering(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "redraw");
    /* The menu is open -- s18's rule taken literally would refuse to
     * execute, and the prompt could never be used with one Enter. */
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len != 0U);
    YEW_ASSERT(!fixture.ed.cmdline.menu.explicit_sel);

    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT(!fixture.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(fixture.ed.mode, YEW_MODE_L);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_enter_accepts_a_chosen_row_without_executing(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    /* A stem with SEVERAL matches: a single match is inserted outright
     * and never becomes a choice to accept. */
    cmdline_type(&fixture, "fil");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 1U);

    /*
     * The first Tab inserts the prefix every tiered candidate shares
     * (`file.`) and chooses nothing -- there is still an unambiguous
     * completion to offer, so it offers it.
     */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    YEW_ASSERT(!fixture.ed.cmdline.menu.explicit_sel);

    /* With nothing left to insert unambiguously, the next Tab IS a
     * choice.  Only Tab, S-Tab, C-n, C-p or a click get here. */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    YEW_ASSERT(fixture.ed.cmdline.menu.explicit_sel);

    /*
     * Now Enter accepts instead of executing -- the property s18 pinned
     * so `:w /etc/pas` cannot run when the user meant to pick `passwd`.
     */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_accept),
                      YEW_CMD_OK);
    YEW_ASSERT(fixture.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);
    /* The chosen row landed in the line, followed by the separating
     * space that says the argument position is next. */
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT(strncmp((const char *)text.data, "file.", 5U) == 0);
    YEW_ASSERT_EQ_STR((const char *)text.data +
                          strlen((const char *)text.data) - 1U, " ");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_escape_leaves_a_live_menu_and_closes_the_prompt(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "fil");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len != 0U);

    /*
     * The user never opened this menu -- it filtered itself open while
     * they typed.  Esc goes past it and closes the prompt, or leaving
     * would take two presses for a list nobody asked for.
     */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_cancel),
                      YEW_CMD_OK);
    YEW_ASSERT(!fixture.ed.cmdline.active);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_hint_reports_what_the_parser_understands(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    cmdline_document(&fixture, "one\ntwo\nthree\nfour\n");

    /* An abbreviation is the one case where the user cannot see what
     * will actually run, so it is spelled out -- with the argument the
     * command is waiting for. */
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "w");
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint,
                      "w \xE2\x86\x92 file.write \xC2\xB7 <file>");
    yew_cmdline_close(&fixture.ed, false);

    /* A full name needs no arrow. */
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "redraw");
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint, "redraw");
    yew_cmdline_close(&fixture.ed, false);

    /*
     * A range reports how many LINES it covers.  The user typed the
     * numbers, so echoing them back says nothing; what they cannot see
     * is what those numbers resolve to.
     */
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "1,3d");
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint,
                      "d \xE2\x86\x92 edit.line.delete \xC2\xB7 3 lines");
    yew_cmdline_close(&fixture.ed, false);

    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "%d");
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint,
                      "d \xE2\x86\x92 edit.line.delete \xC2\xB7 whole buffer");
    yew_cmdline_close(&fixture.ed, false);

    cmdline_fixture_free(&fixture);
}

void test_cmdline_hint_says_nothing_it_does_not_know(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);

    /* Nothing typed: nothing understood, nothing said. */
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint, "");

    /*
     * An unknown command produces NO hint rather than an error line.
     * While the user is still typing, the empty menu is already the
     * signal, and styling a half-typed line as a failure is the
     * flashing message line Sprint 21's doctrine forbids.
     */
    cmdline_type(&fixture, "zzzzz");
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hint, "");
    YEW_ASSERT(!(fixture.ed.msg.active &&
                 fixture.ed.msg.sev == YEW_MSG_ERROR));
    cmdline_fixture_free(&fixture);
}

void test_cmdline_menu_commands_are_registered_and_internal(void)
{
    static const char *const names[] = {
        "ed.cmdline.menu.next",      "ed.cmdline.menu.prev",
        "ed.cmdline.menu.page_next", "ed.cmdline.menu.page_prev",
        "ed.cmdline.menu.accept",    "ed.cmdline.menu.dismiss",
        "ed.cmdline.ghost.accept",
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        CmdId id = yew_cmd_lookup(names[i], (u32)strlen(names[i]));
        const CmdDesc *desc = yew_cmd_desc(id);

        YEW_ASSERT(id.v != 0U);
        YEW_ASSERT_NOT_NULL(desc);
        /* Keymap plumbing, not commands a user types: YEW_CMD_INTERNAL
         * is what keeps them out of the `:` menu and out of the
         * E-mode resolver. */
        YEW_ASSERT((desc->flags & YEW_CMD_INTERNAL) != 0U);
    }
}

void test_cmdline_menu_page_moves_by_a_page(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "e");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 5U);

    /* Entering the list lands on row 0, the same as Tab does -- a page
     * key should not skip past the best match on its way in. */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_menu_page_next),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 0);
    YEW_ASSERT(fixture.ed.cmdline.menu.explicit_sel);

    /* Now it pages: five visible rows, so row 0 -> row 5. */
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_menu_page_next),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 5);

    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_menu_page_prev),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 0);
    cmdline_fixture_free(&fixture);
}

static Key cmdline_mouse(u8 button, u8 ev, u16 col, u16 row)
{
    Key key = {0};

    key.kind = YEW_EV_MOUSE;
    key.ev = ev;
    key.button = button;
    key.col = col;
    key.row = row;
    return key;
}

/* Stands in for a draw: §5 registers exactly these from the same Rect it
 * draws each row with, but a unit fixture has no grid to draw into. */
static void cmdline_fake_menu_regions(u32 rows)
{
    u32 i;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_BLOCK, (Rect){0U, 10U, 80U, (u16)rows}, 0);
    for (i = 0U; i < rows; i++)
        yew_region_add(YEW_REGION_MENU_ROW,
                       (Rect){0U, (u16)(10U + i), 80U, 1U}, (i32)i);
}

void test_cmdline_click_selects_then_accepts_the_same_row(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "fil");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 2U);
    cmdline_fake_menu_regions(5U);

    /* First click chooses the row -- explicitly, exactly as Tab does,
     * which is what §6's Enter rule keys on. */
    {
        Key not_mouse = {0}; /* a key event must not reach the menu */

        YEW_ASSERT(!yew_mouse_claimed_by_menu(&fixture.ed, not_mouse));
    }
    {
        Key press = cmdline_mouse((u8)YEW_MB_LEFT, YEW_KEY_PRESS, 4U, 12U);

        yew_mouse_event(&fixture.ed, &press);
        YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 2);
        YEW_ASSERT(fixture.ed.cmdline.menu.explicit_sel);
        YEW_ASSERT(fixture.ed.cmdline.active);

        /* Second click on the SAME row accepts it and closes the menu. */
        yew_mouse_event(&fixture.ed, &press);
        YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);
        YEW_ASSERT(fixture.ed.cmdline.active);
    }
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT(strncmp((const char *)text.data, "file.", 5U) == 0);
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_click_and_keyboard_reach_the_same_state(void)
{
    CmdlineFixture fixture;
    Bytebuf clicked;
    Bytebuf typed;

    /* Invariant 9: the mouse is an accelerator, never the only way. */
    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "fil");
    cmdline_fake_menu_regions(5U);
    {
        Key press = cmdline_mouse((u8)YEW_MB_LEFT, YEW_KEY_PRESS, 4U, 10U);

        yew_mouse_event(&fixture.ed, &press);
    }
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 0);
    clicked = cmdline_text(&fixture.ed.cmdline);
    cmdline_fixture_free(&fixture);

    /* The same row, reached with C-n's command instead. */
    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "fil");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, 0);
    typed = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)clicked.data, (const char *)typed.data);
    bytebuf_free(&typed);
    bytebuf_free(&clicked);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_wheel_scrolls_without_choosing(void)
{
    CmdlineFixture fixture;
    Key wheel;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "e");
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len > 8U);
    cmdline_fake_menu_regions(5U);
    /* The prompt row, so the menu has rows above it to scroll within. */
    fixture.ed.footer_rect = (Rect){0U, 23U, 80U, 1U};

    wheel = cmdline_mouse((u8)YEW_MB_WHEEL_DOWN, YEW_KEY_PRESS, 4U, 12U);
    yew_mouse_event(&fixture.ed, &wheel);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.top, 3U);

    /*
     * Looking is not choosing: the wheel moved the window and left the
     * selection alone, so Enter still EXECUTES rather than accepting a
     * row the user only scrolled past.
     */
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, -1);
    YEW_ASSERT(!fixture.ed.cmdline.menu.explicit_sel);

    wheel = cmdline_mouse((u8)YEW_MB_WHEEL_UP, YEW_KEY_PRESS, 4U, 12U);
    yew_mouse_event(&fixture.ed, &wheel);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.menu.top, 0U);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_click_on_a_menu_gap_is_swallowed(void)
{
    CmdlineFixture fixture;
    Key press;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "fil");
    yew_region_frame_begin();
    /* Only the inert block, as if the row rects did not cover it. */
    yew_region_add(YEW_REGION_BLOCK, (Rect){0U, 10U, 80U, 5U}, 0);

    /* Claimed, so it cannot fall through to the pane underneath -- and
     * claiming it is all that happens: no row was chosen. */
    press = cmdline_mouse((u8)YEW_MB_LEFT, YEW_KEY_PRESS, 4U, 12U);
    YEW_ASSERT(yew_mouse_claimed_by_menu(&fixture.ed, press));
    yew_mouse_event(&fixture.ed, &press);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.menu.sel, -1);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_ghost_is_never_in_the_buffer(void)
{
    CmdlineFixture fixture;
    Bytebuf typed;
    Bytebuf via_api;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "redr");
    /* A suggestion is showing: `redraw` extends `redr`. */
    YEW_ASSERT(fixture.ed.cmdline.menu.items.len != 0U);

    /*
     * The prompt still holds exactly what was typed.  A ghost in the
     * TextBuf would poison the history draft, hand the parser text the
     * user never wrote, and make yew_cmdline_text() -- which Sprint 21's
     * search reads on every keystroke -- return a pattern with a
     * suggestion glued onto it.
     */
    typed = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)typed.data, "redr");
    bytebuf_free(&typed);

    bytebuf_init(&via_api);
    yew_cmdline_text(&fixture.ed, &via_api);
    bytebuf_push_u8(&via_api, 0U);
    YEW_ASSERT_EQ_STR((const char *)via_api.data, "redr");
    bytebuf_free(&via_api);

    /* And the history draft is the typed text, not the suggestion. */
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hist.draft, "redr");
    cmdline_fixture_free(&fixture);
}

void test_cmdline_ghost_accept_matches_a_menu_accept(void)
{
    CmdlineFixture fixture;
    Bytebuf ghosted;
    Bytebuf chosen;

    /* Accept the suggestion with Right. */
    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "redr");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_ghost_accept),
                      YEW_CMD_OK);
    ghosted = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)ghosted.data, "redraw ");
    cmdline_fixture_free(&fixture);

    /* Accept the same candidate from the menu instead.  One accept path
     * means the two land byte-identical text. */
    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "redr");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     yew_cmdline_cmd_complete_next),
                      YEW_CMD_OK);
    chosen = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)chosen.data,
                      (const char *)ghosted.data);
    bytebuf_free(&chosen);
    bytebuf_free(&ghosted);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_ghost_accept_is_a_motion_when_nothing_is_suggested(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, NULL);
    cmdline_type(&fixture, "quit");
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 4U);

    /* Step off the end: no suggestion can be shown mid-line, so Right
     * has to be an ordinary motion.  Driven through the keymap, so the
     * binding is under test too. */
    yew_ed_handle_key(&fixture.ed, cmdline_key(YEW_KEY_LEFT), 1);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 3U);
    yew_ed_handle_key(&fixture.ed, cmdline_key(YEW_KEY_RIGHT), 1);
    YEW_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 4U);

    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "quit");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_printable_edit_resets_history_walk_to_new_draft(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    yew_cmdline_open(&fixture.ed, YEW_PROMPT_CMD, "wr");
    yew_hist_add(fixture.ed.cmdline.history, "write");
    YEW_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, yew_cmdline_cmd_hist_prev),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.hist.idx, 0);
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hist.stem, "wr");

    yew_ed_handle_key(&fixture.ed, cmdline_key((u32)'!'), 1);
    text = cmdline_text(&fixture.ed.cmdline);
    YEW_ASSERT_EQ_STR((const char *)text.data, "write!");
    YEW_ASSERT_EQ_I64(fixture.ed.cmdline.hist.idx, -1);
    YEW_ASSERT_NULL(fixture.ed.cmdline.hist.stem);
    YEW_ASSERT_EQ_STR(fixture.ed.cmdline.hist.draft, "write!");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}
