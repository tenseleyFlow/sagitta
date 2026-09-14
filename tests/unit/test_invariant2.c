#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "edit/motion.h"
#include "edit/sel_actions.h"
#include "edit/shell.h"
#include "fl/record.h"
#include "search/replace.h"
#include "search/searchui.h"
#include "unicode/coords.h"
#include "util/arena.h"

enum {
    INV2_RI_COUNT = 65,
    INV2_MARK_COUNT = 300,
    INV2_BOM_PAD = 66
};

typedef struct Inv2Line {
    u64 lo;
    u64 content_hi;
    u64 chars;
    u64 graphemes;
    u64 cells;
} Inv2Line;

/*
 * Sprint 58 invariant 2's hand-computed coordinate oracle.  These offsets
 * describe the final buffer, after the CR byte is inserted immediately
 * before an LF that remains in the original store.  Do not derive this table
 * from the implementation under test.
 */
static const Inv2Line inv2_lines[] = {
    {0U, 25U, 7U, 1U, 2U},
    {26U, 286U, 65U, 33U, 66U},
    {287U, 888U, 301U, 1U, 1U},
    {889U, 892U, 3U, 3U, 12U},
    {893U, 902U, 9U, 9U, 36U},
    {903U, 907U, 4U, 4U, 4U},
    {909U, 981U, 68U, 68U, 66U},
};

static const u8 inv2_family[] = {
    0xF0U, 0x9FU, 0x91U, 0xA8U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA9U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA7U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA6U,
};

static const u8 inv2_ri[] = {0xF0U, 0x9FU, 0x87U, 0xA6U};
static const u8 inv2_mark[] = {0xCCU, 0x81U};
static const u8 inv2_surrogate[] = {0xEDU, 0xA0U, 0x80U};
static const u8 inv2_overlong[] = {
    0xC0U, 0x80U,
    0xE0U, 0x80U, 0x80U,
    0xF0U, 0x80U, 0x80U, 0x80U,
};
static const u8 inv2_bom_pair[] = {
    0xEFU, 0xBBU, 0xBFU, 0xEFU, 0xBBU, 0xBFU,
};

static TextBuf *inv2_textbuf(Bytebuf *want)
{
    Bytebuf source;
    TextBuf *tb;
    u32 i;

    bytebuf_init(&source);
    bytebuf_append(&source, inv2_family, sizeof(inv2_family));
    bytebuf_push_u8(&source, (u8)'\n');
    for (i = 0U; i < INV2_RI_COUNT; i++)
        bytebuf_append(&source, inv2_ri, sizeof(inv2_ri));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_push_u8(&source, (u8)'e');
    for (i = 0U; i < INV2_MARK_COUNT; i++)
        bytebuf_append(&source, inv2_mark, sizeof(inv2_mark));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, inv2_surrogate, sizeof(inv2_surrogate));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, inv2_overlong, sizeof(inv2_overlong));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, "crlf\n", 5U);
    bytebuf_append(&source, inv2_bom_pair, sizeof(inv2_bom_pair));
    for (i = 0U; i < INV2_BOM_PAD; i++)
        bytebuf_push_u8(&source, (u8)'z');

    tb = yew_textbuf_from_bytes(source.data, source.len);
    YEW_ASSERT_NOT_NULL(tb);
    /* The LF was copied into the original store; CR comes from add storage. */
    yew_textbuf_insert(tb, BYTEOFF(907U), (const u8 *)"\r", 1U);
    YEW_ASSERT(yew_textbuf_piece_count(tb) >= 3U);

    bytebuf_init(want);
    bytebuf_append(want, source.data, 907U);
    bytebuf_push_u8(want, (u8)'\r');
    bytebuf_append(want, source.data + 907U, source.len - 907U);
    bytebuf_free(&source);
    return tb;
}

static void inv2_assert_bytes(const TextBuf *tb, const Bytebuf *want)
{
    TextIter it;
    size_t done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want->len);
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want->len) {
        const u8 *bytes;
        u64 len;
        size_t take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < (u64)(want->len - done) ? (size_t)len
                                             : want->len - done;
        YEW_ASSERT_EQ_MEM(bytes, want->data + done, take);
        done += take;
        if (done < want->len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

static void inv2_assert_span(const TextBuf *tb, u64 off,
                             const u8 *want, size_t want_len)
{
    TextIter it;
    size_t done = 0U;

    YEW_ASSERT(off + want_len <= yew_textbuf_len(tb));
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(off)));
    while (done < want_len) {
        const u8 *bytes;
        u64 len;
        size_t take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < (u64)(want_len - done) ? (size_t)len
                                            : want_len - done;
        YEW_ASSERT_EQ_MEM(bytes, want + done, take);
        done += take;
        if (done < want_len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

void test_invariant2_adversarial_columns_and_all_units(void)
{
    static const UnitOps *const units[] = {
        &yew_unit_line,
        &yew_unit_word,
        &yew_unit_block,
        &yew_unit_char,
    };
    Bytebuf want;
    TextBuf *tb = inv2_textbuf(&want);
    Buffer buffer = {0};
    Win win = {0};
    UnitCtx unit = {0};
    size_t i;

    buffer.tb = tb;
    buffer.tabwidth = 4U;
    win.buf = &buffer;
    win.vp.rows = 24U;
    win.vp.cols = 80U;
    unit.tb = tb;
    unit.buf = &buffer;
    unit.win = &win;

    YEW_ASSERT_EQ_U64(want.len, 981U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb),
                      YEW_ARRAY_LEN(inv2_lines));
    inv2_assert_bytes(tb, &want);

    for (i = 0U; i < YEW_ARRAY_LEN(inv2_lines); i++) {
        const Inv2Line *oracle = &inv2_lines[i];
        Span line = yew_textbuf_line_span(tb, LINENO(i));
        ByteOff at = BYTEOFF(line.lo);
        u64 span_hi = i + 1U < YEW_ARRAY_LEN(inv2_lines)
                          ? inv2_lines[i + 1U].lo
                          : want.len;
        u64 graphemes = 0U;
        u64 cells = 0U;

        YEW_ASSERT_EQ_U64(line.lo, oracle->lo);
        YEW_ASSERT_EQ_U64(line.hi, span_hi);
        while (at.v < oracle->content_hi) {
            YewTextCluster cluster;

            YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, at).v,
                              graphemes);
            YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, at, 4U).v,
                              cells);
            YEW_ASSERT(yew_text_cluster_next(tb, line, at, &cluster));
            YEW_ASSERT_EQ_U64(cluster.bytes.lo, at.v);
            YEW_ASSERT(cluster.bytes.hi > cluster.bytes.lo);
            cells += cluster.cells;
            graphemes++;
            at = BYTEOFF(cluster.bytes.hi);
        }
        YEW_ASSERT_EQ_U64(at.v, oracle->content_hi);
        YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, line, at).v,
                          oracle->chars);
        YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, at).v,
                          oracle->graphemes);
        YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, at, 4U).v,
                          oracle->cells);
        YEW_ASSERT_EQ_U64(graphemes, oracle->graphemes);
        YEW_ASSERT_EQ_U64(cells, oracle->cells);
        YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                          (GCol){oracle->graphemes}).v,
                          oracle->content_hi);
    }

    for (i = 0U; i < YEW_ARRAY_LEN(units); i++) {
        const UnitOps *ops = units[i];
        u8 alt;

        for (alt = 0U; alt < 2U; alt++) {
            ByteOff at = BYTEOFF(0U);
            u64 steps = 0U;

            while (at.v < want.len) {
                ByteOff next = ops->next(&unit, at, alt != 0U);

                YEW_ASSERT(next.v > at.v);
                YEW_ASSERT(next.v <= want.len);
                YEW_ASSERT(yew_is_grapheme_boundary(tb, next));
                at = next;
                YEW_ASSERT(++steps <= want.len + 1U);
            }
            while (at.v != 0U) {
                ByteOff prev = ops->prev(&unit, at, alt != 0U);

                YEW_ASSERT(prev.v < at.v);
                YEW_ASSERT(yew_is_grapheme_boundary(tb, prev));
                at = prev;
                YEW_ASSERT(++steps <= 2U * want.len + 2U);
            }
        }
    }

    inv2_assert_bytes(tb, &want);
    bytebuf_free(&want);
    yew_textbuf_free(tb);
}

static void inv2_write_file(const char *path, const u8 *bytes, size_t len)
{
    FILE *file = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void inv2_assert_file(const char *path, const Bytebuf *want)
{
    FILE *file = fopen(path, "rb");
    Bytebuf got;
    u8 chunk[257];
    size_t n;

    YEW_ASSERT_NOT_NULL(file);
    bytebuf_init(&got);
    while ((n = fread(chunk, 1U, sizeof(chunk), file)) != 0U)
        bytebuf_append(&got, chunk, n);
    YEW_ASSERT(!ferror(file));
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    YEW_ASSERT_EQ_U64(got.len, want->len);
    YEW_ASSERT_EQ_MEM(got.data, want->data, want->len);
    bytebuf_free(&got);
}

static void inv2_remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        YEW_ASSERT_NOT_NULL(dir);
        while ((entry = readdir(dir)) != NULL) {
            char child[256];
            int n;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            n = snprintf(child, sizeof(child), "%s/%s", path,
                         entry->d_name);
            YEW_ASSERT(n > 0 && (size_t)n < sizeof(child));
            inv2_remove_tree(child);
        }
        YEW_ASSERT_EQ_I64(closedir(dir), 0);
        YEW_ASSERT_EQ_I64(rmdir(path), 0);
    } else {
        YEW_ASSERT_EQ_I64(unlink(path), 0);
    }
}

static void inv2_place(Ed *ed, u64 pos, u64 anchor)
{
    Cursor *cursor = yew_ed_cursor(ed);
    Span line;

    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(pos);
    cursor->anchor = BYTEOFF(anchor);
    line = yew_textbuf_line_span(ed->buffer.tb,
                                 yew_textbuf_line_of(ed->buffer.tb,
                                                     cursor->pos));
    cursor->goal_col = yew_off_to_ccol(ed->buffer.tb, line, cursor->pos,
                                       YEW_VP_TABWIDTH);
    yew_cset_normalize(ed->buffer.tb, &ed->win->cs);
}

static u32 inv2_replace_ascii(Ed *ed, const char *pat, const char *replacement)
{
    Arena arena;
    SearchOpts opts;
    YewRe *re;
    YewReplPlan plan;
    YewReplErr err = {0};
    EditCtx ec;
    u64 lines = yew_textbuf_line_count(ed->buffer.tb);
    u32 changed;

    arena_init(&arena);
    yew_search_opts_init(&opts);
    re = yew_search_compile(&arena, pat, strlen(pat), &opts, NULL);
    YEW_ASSERT_NOT_NULL(re);
    yew_repl_plan_init(&plan);
    YEW_ASSERT(yew_repl_plan_build(&plan, re, ed->buffer.tb, LINENO(0U),
                                   LINENO(lines - 1U), replacement,
                                   strlen(replacement), YEW_SUB_GLOBAL,
                                   &err));
    ec = yew_ed_edit_ctx(ed);
    changed = yew_repl_plan_apply(&plan, &ec);
    yew_ed_finish_edit(ed, &ec);
    yew_repl_plan_free(&plan);
    arena_free_all(&arena);
    return changed;
}

void test_invariant2_end_to_end_byte_pipeline(void)
{
    static const char macro[] = "@[ i\"Q\" ]\n";
    static const u64 source_row_bytes[] = {
        25U, 260U, 601U, 3U, 9U, 4U, 72U,
    };
    static const u64 stored_row_bytes[] = {
        89U, 260U, 666U, 57U, 39U, 66U, 72U,
    };
    char path[] = "/tmp/yew-invariant2-XXXXXX";
    char state[] = "/tmp/yew-invariant2-state-XXXXXX";
    const char *state_env = getenv("XDG_STATE_HOME");
    char *saved_state = state_env == NULL ? NULL : strdup(state_env);
    Bytebuf want;
    TextBuf *seed = inv2_textbuf(&want);
    Ed ed;
    CmdCtx cx = {0};
    EditCtx ec;
    RegVal *reg;
    u64 before_len;
    int fd;
    size_t i;

    yew_textbuf_free(seed);
    YEW_ASSERT(state_env == NULL || saved_state != NULL);
    YEW_ASSERT_NOT_NULL(mkdtemp(state));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state, 1), 0);
    fd = mkstemp(path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    inv2_write_file(path, want.data, want.len);

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_open(&ed, path), YEW_LOAD_OK);
    ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    inv2_assert_bytes(ed.buffer.tb, &want);
    YEW_ASSERT(ed.buffer.meta.had_invalid_utf8);
    YEW_ASSERT_EQ_U64(ed.buffer.meta.eol, YEW_EOL_MIXED);

    /* Reinsert CR from add storage while LF stays in the original store. */
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(ec.tb == ed.buffer.tb);
    YEW_ASSERT(ec.buffer == &ed.buffer);
    YEW_ASSERT_NOT_NULL(ec.meta);
    YEW_ASSERT_NOT_NULL(ec.undo);
    YEW_ASSERT_NOT_NULL(ec.cset);
    yew_undo_begin(&ec, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_delete(&ec, (Span){907U, 908U}));
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(907U), (const u8 *)"\r", 1U));
    yew_undo_end(&ec);
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_textbuf_piece_count(ed.buffer.tb) >= 3U);
    inv2_assert_bytes(ed.buffer.tb, &want);

    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&ed, YEW_MODE_L, false),
                      YEW_CMD_OK);
    ed.win->h.kind = YEW_SEL_RECT;
    inv2_place(&ed, want.len, 0U);
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_sel_cmd_yank(&cx), YEW_CMD_OK);
    reg = yew_reg_get(&ed.regs, '"');
    YEW_ASSERT_NOT_NULL(reg);
    YEW_ASSERT_EQ_U64(reg->type, YEW_REG_BLOCKWISE);
    YEW_ASSERT_EQ_U64(reg->width, 66U);
    YEW_ASSERT_EQ_U64(reg->rows.len, YEW_ARRAY_LEN(source_row_bytes));
    YEW_ASSERT_EQ_U64(reg->bytes.len, 1249U);
    for (i = 0U; i < YEW_ARRAY_LEN(source_row_bytes); i++) {
        u64 j;

        YEW_ASSERT_EQ_U64(reg->rows.data[i].hi - reg->rows.data[i].lo,
                          stored_row_bytes[i]);
        YEW_ASSERT_EQ_MEM(reg->bytes.data + reg->rows.data[i].lo,
                          want.data + inv2_lines[i].lo,
                          source_row_bytes[i]);
        for (j = source_row_bytes[i]; j < stored_row_bytes[i]; j++)
            YEW_ASSERT_EQ_U64(reg->bytes.data[reg->rows.data[i].lo + j],
                              (u8)' ');
    }
    inv2_assert_bytes(ed.buffer.tb, &want);

    YEW_ASSERT_EQ_U64(yew_mode_enter(&ed, YEW_MODE_L), YEW_CMD_OK);
    inv2_place(&ed, 0U, 0U);
    before_len = yew_textbuf_len(ed.buffer.tb);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_reg_paste(&ed.regs, &ec, '"', true, 4U));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_textbuf_len(ed.buffer.tb) > before_len);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    inv2_assert_bytes(ed.buffer.tb, &want);

    YEW_ASSERT_EQ_U64(yew_shell_filter(&ed, ed.win,
                                       (Span){0U, want.len}, "cat", NULL),
                      YEW_FILT_OK);
    inv2_assert_bytes(ed.buffer.tb, &want);

    YEW_ASSERT_EQ_U64(inv2_replace_ascii(&ed, "crlf", "CRLF"), 1U);
    inv2_assert_span(ed.buffer.tb, 903U, (const u8 *)"CRLF", 4U);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    inv2_assert_bytes(ed.buffer.tb, &want);

    inv2_place(&ed, want.len, want.len);
    YEW_ASSERT_EQ_I64(yew_flapi_reg_write(&ed, (u8)'a', (const u8 *)macro,
                                          sizeof(macro) - 1U, false),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_macro_replay(&ed, (u8)'a', 1U), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.buffer.tb), want.len + 1U);
    inv2_assert_span(ed.buffer.tb, want.len, (const u8 *)"Q", 1U);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    inv2_assert_bytes(ed.buffer.tb, &want);

    YEW_ASSERT_EQ_I64(yew_ed_file_save(&ed, false), YEW_CMD_OK);
    inv2_assert_file(path, &want);
    yew_ed_free(&ed);

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_U64(yew_ed_open(&ed, path), YEW_LOAD_OK);
    inv2_assert_bytes(ed.buffer.tb, &want);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(unlink(path), 0);
    if (saved_state == NULL)
        YEW_ASSERT_EQ_I64(unsetenv("XDG_STATE_HOME"), 0);
    else {
        YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", saved_state, 1), 0);
        free(saved_state);
    }
    inv2_remove_tree(state);
    bytebuf_free(&want);
}
