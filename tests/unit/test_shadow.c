/* Sprint 43: accepting a ghost is the only path that makes it text. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "text/edit.h"

static void shadow_accept_fixture(Ed *ed, const u8 *bytes, size_t len)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "shadow-accept"));
    ed->win->cs.curs.data[ed->win->cs.primary].pos = BYTEOFF(len);
    ed->win->cs.curs.data[ed->win->cs.primary].anchor = BYTEOFF(len);
}

static void deliver_at_cursor(Ed *ed, const u8 *text, u32 len)
{
    ShadowSug suggestion = {0};
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];

    suggestion.seq = ed->win->shadow.seq_next[YEW_SHADOW_INDEX]++;
    suggestion.prov = YEW_SHADOW_INDEX;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = cursor->pos;
    suggestion.text = text;
    suggestion.len = len;
    yew_shadow_deliver(ed, &suggestion);
}

static bool accept_text_eq(const TextBuf *tb, const u8 *want, size_t len)
{
    TextIter iter;
    u64 done = 0U;

    if (yew_textbuf_len(tb) != len)
        return false;
    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return false;
    while (done < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available))
            return false;
        take = available < len - done ? available : len - done;
        if (take == 0U || memcmp(bytes, want + done, (size_t)take) != 0)
            return false;
        done += take;
        if (done < len && !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

void test_shadow_accept_all_is_one_owned_undo_transaction(void)
{
    Ed ed;
    EditCtx edit;
    u32 before;

    shadow_accept_fixture(&ed, (const u8 *)"let ", 4U);
    deliver_at_cursor(&ed, (const u8 *)"value", 5U);
    before = yew_undo_current(ed.win->buf->undo);
    YEW_ASSERT(yew_shadow_accept_all(&ed, ed.win));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_all, 1U);
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"let value",
                              9U));
    YEW_ASSERT(yew_undo_current(ed.win->buf->undo) != before);

    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&edit));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"let ", 4U));
    yew_ed_free(&ed);
}

void test_shadow_accept_line_shortens_then_consumes(void)
{
    Ed ed;

    shadow_accept_fixture(&ed, NULL, 0U);
    deliver_at_cursor(&ed, (const u8 *)"one\ntwo", 7U);
    YEW_ASSERT(yew_shadow_accept_line(&ed, ed.win));
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.consumed, 4U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.buf_gen, ed.win->buf->tb->gen);
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"one\n", 4U));
    YEW_ASSERT(yew_shadow_accept_line(&ed, ed.win));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_line, 2U);
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"one\ntwo", 7U));
    yew_ed_free(&ed);
}

void test_shadow_accept_after_typed_prefix_keeps_owned_bytes_alive(void)
{
    Ed ed;
    EditCtx edit;

    shadow_accept_fixture(&ed, NULL, 0U);
    deliver_at_cursor(&ed, (const u8 *)"hello", 5U);
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"he", 2U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.consumed, 2U);
    YEW_ASSERT(yew_shadow_accept_all(&ed, ed.win));
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"hello", 5U));
    YEW_ASSERT(!ed.win->shadow.live);
    yew_ed_free(&ed);
}

void test_shadow_accept_revalidation_failure_never_inserts(void)
{
    Ed ed;
    u32 undo_before;

    shadow_accept_fixture(&ed, NULL, 0U);
    deliver_at_cursor(&ed, (const u8 *)"ghost", 5U);
    undo_before = yew_undo_current(ed.win->buf->undo);
    yew_textbuf_insert(ed.win->buf->tb, BYTEOFF(0U), (const u8 *)"x", 1U);
    ed.win->cs.curs.data[0].pos = BYTEOFF(1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(1U);
    YEW_ASSERT(!yew_shadow_accept_all(&ed, ed.win));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.revalidate_fail, 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.win->buf->undo), undo_before);
    YEW_ASSERT(accept_text_eq(ed.win->buf->tb, (const u8 *)"x", 1U));
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_STR(ed.msg.text, "suggestion is stale");
    yew_ed_free(&ed);
}

static u64 expected_word_len(Mode mode, const u8 *text, u32 len, bool alt)
{
    TextBuf *scratch = yew_textbuf_from_bytes(text, len);
    const UnitOps *unit = (mode == YEW_MODE_I || mode == YEW_MODE_E)
                              ? &yew_unit_word
                              : yew_unit_of_mode(mode);
    UnitCtx ctx = {scratch, NULL, NULL};
    ByteOff next;

    YEW_ASSERT_NOT_NULL(unit);
    next = unit->next(&ctx, BYTEOFF(0U), alt);
    yew_textbuf_free(scratch);
    return next.v;
}

void test_shadow_accept_word_matches_mode_units(void)
{
    static const u8 ascii[] = "fooBarBaz tail";
    static const u8 cjk[] = "漢字テスト 次";
    static const u8 emoji[] = "👨‍👩‍👧‍👦x tail";
    static const struct {
        const u8 *text;
        u32 len;
    } cases[] = {
        {ascii, (u32)(sizeof(ascii) - 1U)},
        {cjk, (u32)(sizeof(cjk) - 1U)},
        {emoji, (u32)(sizeof(emoji) - 1U)},
    };
    static const Mode modes[] = {
        YEW_MODE_L, YEW_MODE_W, YEW_MODE_B, YEW_MODE_I, YEW_MODE_E,
    };
    u32 i;
    u32 j;
    u32 alt;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        for (j = 0U; j < YEW_ARRAY_LEN(modes); j++) {
            for (alt = 0U; alt < 2U; alt++) {
                Ed ed;
                u64 want = expected_word_len(modes[j], cases[i].text,
                                             cases[i].len, alt != 0U);

                shadow_accept_fixture(&ed, NULL, 0U);
                ed.mode = modes[j];
                deliver_at_cursor(&ed, cases[i].text, cases[i].len);
                YEW_ASSERT(yew_shadow_accept_word(&ed, ed.win,
                                                   alt != 0U));
                YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), want);
                YEW_ASSERT(accept_text_eq(ed.win->buf->tb, cases[i].text,
                                          (size_t)want));
                YEW_ASSERT_EQ_U64(ed.shadow_stats.accepted_word, 1U);
                yew_ed_free(&ed);
            }
        }
    }
}
