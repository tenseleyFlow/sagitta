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

static char *cmdline_dup(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = sag_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void cmdline_fixture_init(CmdlineFixture *fixture)
{
    const char *state = getenv("XDG_STATE_HOME");

    fixture->saved_state = state == NULL ? NULL : cmdline_dup(state);
    (void)snprintf(fixture->state, sizeof(fixture->state),
                   "/tmp/sag-cmdline-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(fixture->state));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->state, 1), 0);
    sag_ed_init(&fixture->ed);
    SAG_ASSERT(sag_ed_open_scratch(&fixture->ed));
}

static void cmdline_fixture_free(CmdlineFixture *fixture)
{
    static const char *const names[] = {"cmd", "cmd.lock"};
    char path[160];
    size_t i;

    sag_ed_free(&fixture->ed);
    for (i = 0U; i < SAG_ARRAY_LEN(names); i++) {
        (void)snprintf(path, sizeof(path), "%s/sagitta/history/%s",
                       fixture->state, names[i]);
        if (unlink(path) != 0)
            SAG_ASSERT_EQ_I64(errno, ENOENT);
    }
    (void)snprintf(path, sizeof(path), "%s/sagitta/history",
                   fixture->state);
    if (rmdir(path) != 0)
        SAG_ASSERT_EQ_I64(errno, ENOENT);
    (void)snprintf(path, sizeof(path), "%s/sagitta", fixture->state);
    if (rmdir(path) != 0)
        SAG_ASSERT_EQ_I64(errno, ENOENT);
    SAG_ASSERT_EQ_I64(rmdir(fixture->state), 0);
    if (fixture->saved_state != NULL)
        SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->saved_state, 1),
                          0);
    else
        SAG_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    free(fixture->saved_state);
}

static Bytebuf cmdline_text(const CmdLine *line)
{
    Bytebuf out;
    TextIter iter;

    bytebuf_init(&out);
    if (sag_textiter_begin(&iter, line->buf, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&iter, line->buf, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&iter, line->buf));
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

    sag_undo_free(fixture->ed.buffer.undo);
    sag_textbuf_free(fixture->ed.buffer.tb);
    fixture->ed.buffer.tb =
        sag_textbuf_from_bytes((const u8 *)text, strlen(text));
    fixture->ed.buffer.undo = sag_undo_new(fixture->ed.buffer.tb);
    cursor = sag_ed_cursor(&fixture->ed);
    SAG_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
}

static Key cmdline_key(u32 code)
{
    Key key = {0};

    key.code = code;
    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
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
    context.win = sag_cmdline_target(ed);
    context.source = SAG_SRC_TEST;
    return command(&context);
}

void test_cmdline_reuses_textbuf_and_grapheme_cursor(void)
{
    CmdlineFixture fixture;
    Win *target;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, FAMILY);
    target = sag_cmdline_target(&fixture.ed);
    SAG_ASSERT_NOT_NULL(target);
    SAG_ASSERT_NOT_NULL(target->buf);
    SAG_ASSERT_EQ_U64(target->buf->tb, fixture.ed.cmdline.buf);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(fixture.ed.cmdline.buf), 1U);
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, sizeof(FAMILY) - 1U);

    sag_ed_handle_key(&fixture.ed, cmdline_key(SAG_KEY_LEFT), 1);
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 0U);
    SAG_ASSERT_EQ_U64(target->cs.curs.data[target->cs.primary].pos.v, 0U);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_sanitizes_seed_paste_and_register_newlines(void)
{
    CmdlineFixture fixture;
    CmdCtx context = {0};
    RegVal value;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "a\r\n\nb\rc");
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "a b c");
    bytebuf_free(&text);

    sag_cmdline_paste(&fixture.ed, (const u8 *)"x\r\ny\n\nz",
                      sizeof("x\r\ny\n\nz") - 1U);
    sag_regval_init(&value);
    bytebuf_append(&value.bytes, "q\r\n\nr", sizeof("q\r\n\nr") - 1U);
    sag_reg_set(&fixture.ed.regs, (u8)'a', &value);
    sag_regval_free(&value);
    context.ed = &fixture.ed;
    context.win = sag_cmdline_target(&fixture.ed);
    context.sarg = "a";
    context.sarg_len = 1U;
    context.source = SAG_SRC_TEST;
    SAG_ASSERT_EQ_U64(sag_cmdline_cmd_insert_register(&context), SAG_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "a b cx y zq r");
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(fixture.ed.cmdline.buf), 1U);
    bytebuf_free(&text);
    sag_regval_init(&value);
    bytebuf_append(&value.bytes, "n\0x", 3U);
    sag_reg_set(&fixture.ed.regs, (u8)'b', &value);
    sag_regval_free(&value);
    context.sarg = "b";
    SAG_ASSERT_EQ_U64(sag_cmdline_cmd_insert_register(&context),
                      SAG_CMD_ERR_ARG);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "a b cx y zq r");
    SAG_ASSERT_EQ_STR(fixture.ed.msg.text,
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
    const CmdDesc *description = sag_cmd_desc(binding->cmd);

    SAG_ASSERT_NOT_NULL(keys);
    SAG_ASSERT(key_count != 0U);
    SAG_ASSERT_NOT_NULL(description);
    SAG_ASSERT(strncmp(description->name, "ed.", 3U) == 0);
    check->visited++;
    return true;
}

void test_cmdline_e_keymap_leaves_are_registered_editor_commands(void)
{
    CmdlineFixture fixture;
    CmdlineLeafCheck check = {0};
    const Keymap *map;

    cmdline_fixture_init(&fixture);
    map = &fixture.ed.mode_keys[SAG_MODE_E];
    SAG_ASSERT(sag_keymap_visit(map, cmdline_check_leaf, &check));
    SAG_ASSERT_EQ_U64(check.visited, sag_keymap_binding_count(map));
    SAG_ASSERT(check.visited >= 20U);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_highlight_selection_seeds_range(void)
{
    CmdlineFixture fixture;
    Cursor *cursor;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    sag_textbuf_insert(fixture.ed.buffer.tb, BYTEOFF(0U),
                       (const u8 *)"alpha", 5U);
    cursor = sag_ed_cursor(&fixture.ed);
    SAG_ASSERT_NOT_NULL(cursor);
    cursor->anchor = BYTEOFF(1U);
    cursor->pos = BYTEOFF(4U);
    SAG_ASSERT_EQ_U64(sag_mode_enter_highlight(&fixture.ed, SAG_MODE_L,
                                               false), SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_mode_enter(&fixture.ed, SAG_MODE_E), SAG_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "'<,'>");
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v, 5U);
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.return_mode, SAG_MODE_H);
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_escape_restores_completion_stem_before_closing(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "f");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed,
                                     sag_cmdline_cmd_complete_next),
                      SAG_CMD_OK);
    SAG_ASSERT(fixture.ed.cmdline.menu.items.len > 1U);
    SAG_ASSERT(fixture.ed.cmdline.active);

    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_cancel),
                      SAG_CMD_OK);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "f");
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.menu.items.len, 0U);
    SAG_ASSERT(fixture.ed.cmdline.active);
    bytebuf_free(&text);

    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_cancel),
                      SAG_CMD_OK);
    SAG_ASSERT(!fixture.ed.cmdline.active);
    SAG_ASSERT_EQ_U64(fixture.ed.mode, SAG_MODE_L);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_accepts_registered_command_and_closes(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "redraw");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_accept),
                      SAG_CMD_OK);
    SAG_ASSERT(!fixture.ed.cmdline.active);
    SAG_ASSERT_EQ_U64(fixture.ed.mode, SAG_MODE_L);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_range_executes_once_with_multiple_cursors(void)
{
    CmdlineFixture fixture;
    Cursor second = {BYTEOFF(8U), {0U}, BYTEOFF(8U)};
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    cmdline_document(&fixture, "one\ntwo\nthree\n");
    SAG_ASSERT(sag_cset_add(&fixture.ed.win->cs, second));
    SAG_ASSERT_EQ_U64(fixture.ed.win->cs.curs.len, 2U);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "1,2d");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_accept),
                      SAG_CMD_OK);
    text = document_text(&fixture.ed);
    SAG_ASSERT_EQ_STR((const char *)text.data, "three\n");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_bang_quit_bypasses_durability_failure(void)
{
    CmdlineFixture fixture;

    cmdline_fixture_init(&fixture);
    cmdline_document(&fixture, "dirty");
    fixture.ed.durability_failed = true;
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "q!");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_accept),
                      SAG_CMD_OK);
    SAG_ASSERT(fixture.ed.quit);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_parse_error_preserves_text_and_points_at_token(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "definitely_missing");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_accept),
                      SAG_CMD_ERR_ARG);
    SAG_ASSERT(fixture.ed.cmdline.active);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "definitely_missing");
    SAG_ASSERT_EQ_U64(fixture.ed.cmdline.cur.pos.v,
                      fixture.ed.cmdline.err.tok_lo);
    SAG_ASSERT(fixture.ed.cmdline.err.tok_hi >
               fixture.ed.cmdline.err.tok_lo);
    SAG_ASSERT_EQ_STR(fixture.ed.cmdline.err.msg,
                      "unknown command 'definitely_missing' (try Tab)");
    SAG_ASSERT(fixture.ed.msg.active);
    SAG_ASSERT(strncmp(fixture.ed.msg.text, "E: ", 3U) == 0);
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}

void test_cmdline_printable_edit_resets_history_walk_to_new_draft(void)
{
    CmdlineFixture fixture;
    Bytebuf text;

    cmdline_fixture_init(&fixture);
    sag_cmdline_open(&fixture.ed, SAG_PROMPT_CMD, "wr");
    sag_hist_add(fixture.ed.cmdline.history, "write");
    SAG_ASSERT_EQ_U64(cmdline_invoke(&fixture.ed, sag_cmdline_cmd_hist_prev),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(fixture.ed.cmdline.hist.idx, 0);
    SAG_ASSERT_EQ_STR(fixture.ed.cmdline.hist.stem, "wr");

    sag_ed_handle_key(&fixture.ed, cmdline_key((u32)'!'), 1);
    text = cmdline_text(&fixture.ed.cmdline);
    SAG_ASSERT_EQ_STR((const char *)text.data, "write!");
    SAG_ASSERT_EQ_I64(fixture.ed.cmdline.hist.idx, -1);
    SAG_ASSERT_NULL(fixture.ed.cmdline.hist.stem);
    SAG_ASSERT_EQ_STR(fixture.ed.cmdline.hist.draft, "write!");
    bytebuf_free(&text);
    cmdline_fixture_free(&fixture);
}
