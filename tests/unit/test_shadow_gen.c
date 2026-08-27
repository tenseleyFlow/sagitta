/* Sprint 43: generation and byte revalidation are the corruption guard. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "text/edit.h"

static void shadow_fixture(Ed *ed, const u8 *bytes, size_t len)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "shadow-test"));
}

static ShadowSug suggestion_for(Ed *ed, u32 seq, ByteOff pos,
                                const u8 *text, u32 len)
{
    ShadowSug suggestion = {0};

    suggestion.seq = seq;
    suggestion.prov = YEW_SHADOW_INDEX;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = pos;
    suggestion.text = text;
    suggestion.len = len;
    ed->win->shadow.seq_next[YEW_SHADOW_INDEX] = seq + 1U;
    return suggestion;
}

static bool textbuf_eq(const TextBuf *tb, const u8 *want, size_t len)
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

void test_shadow_revalidate_exact_and_divergent_prefixes(void)
{
    static const u8 text[] = "abcdefghijklmnop";
    u8 changed[sizeof(text) - 1U];
    u32 i;

    for (i = 0U; i < sizeof(text) - 1U; i++) {
        TextBuf *tb = yew_textbuf_from_bytes(text, sizeof(text) - 1U);
        ShadowSug suggestion = {0};

        suggestion.text = text;
        suggestion.len = (u32)(sizeof(text) - 1U);
        suggestion.pos = BYTEOFF(0U);
        YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion,
                                                BYTEOFF(i)), i);
        yew_textbuf_free(tb);
    }
    (void)memcpy(changed, text, sizeof(changed));
    for (i = 0U; i < sizeof(changed); i++) {
        TextBuf *tb;
        ShadowSug suggestion = {0};

        changed[i] ^= 0x20U;
        tb = yew_textbuf_from_bytes(changed, sizeof(changed));
        suggestion.text = text;
        suggestion.len = (u32)sizeof(changed);
        suggestion.pos = BYTEOFF(0U);
        YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion,
                                                BYTEOFF(i + 1U)), -1);
        yew_textbuf_free(tb);
        changed[i] ^= 0x20U;
    }
}

void test_shadow_revalidate_boundaries_pieces_and_binary(void)
{
    static const u8 family[] = {
        0xf0U, 0x9fU, 0x91U, 0xa8U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa7U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa6U, (u8)'x'
    };
    static const u8 binary[] = {'a', 0xffU, 'b'};
    TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"xx", 2U);
    ShadowSug suggestion = {0};
    u32 i;

    yew_textbuf_insert(tb, BYTEOFF(2U), (const u8 *)"ab", 2U);
    yew_textbuf_insert(tb, BYTEOFF(4U), (const u8 *)"cd", 2U);
    yew_textbuf_insert(tb, BYTEOFF(6U), (const u8 *)"ef", 2U);
    suggestion.text = (const u8 *)"abcdef";
    suggestion.len = 6U;
    suggestion.pos = BYTEOFF(2U);
    YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion, BYTEOFF(1U)),
                      -1);
    YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion, BYTEOFF(8U)),
                      6);
    yew_textbuf_insert(tb, BYTEOFF(8U), (const u8 *)"z", 1U);
    YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion, BYTEOFF(9U)),
                      -1);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(binary, sizeof(binary));
    suggestion.text = binary;
    suggestion.len = (u32)sizeof(binary);
    suggestion.pos = BYTEOFF(0U);
    YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion, BYTEOFF(3U)),
                      3);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(family, sizeof(family));
    suggestion.text = family;
    suggestion.len = (u32)sizeof(family);
    suggestion.pos = BYTEOFF(0U);
    for (i = 0U; i <= sizeof(family); i++)
        YEW_ASSERT_EQ_I64(yew_shadow_revalidate(tb, &suggestion,
                                                BYTEOFF(i)), i);
    yew_textbuf_free(tb);
}

void test_shadow_delivery_owns_bytes_and_rejects_stale_results(void)
{
    Ed ed;
    u8 bytes[] = "owned";
    ShadowSug suggestion;

    shadow_fixture(&ed, NULL, 0U);
    suggestion = suggestion_for(&ed, 4U, BYTEOFF(0U), bytes, 5U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    bytes[0] = 'x';
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, "owned", 5U);

    yew_shadow_dismiss(&ed, ed.win);
    ed.win->shadow.seq_min[YEW_SHADOW_INDEX] = 5U;
    suggestion.seq = 4U;
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_stale, 1U);

    suggestion.seq = 5U;
    suggestion.buf_id++;
    ed.win->shadow.seq_next[YEW_SHADOW_INDEX] = 6U;
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_stale, 2U);
    YEW_ASSERT(!ed.win->shadow.live);
    yew_ed_free(&ed);
}

void test_shadow_generation_drop_preserves_buffer_bytes(void)
{
    Ed ed;
    static const u8 original[] = "abc";
    ShadowSug suggestion;

    shadow_fixture(&ed, original, sizeof(original) - 1U);
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(3U),
                                (const u8 *)"def", 3U);
    suggestion.buf_gen--;
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_gen, 1U);
    YEW_ASSERT(textbuf_eq(ed.buffer.tb, original, sizeof(original) - 1U));
    yew_ed_free(&ed);
}

void test_shadow_delivery_eligibility_uses_suffix_and_line_shape(void)
{
    static const u8 indented[] = "    code";
    static const u8 whitespace[] = " \t  ";
    static const u8 multiline[] = "one\ntwo";
    OptVal yes = {YEW_OPT_BOOL, {.b = true}};
    const char *err = NULL;
    ShadowSug suggestion;
    Ed ed;

    shadow_fixture(&ed, indented, sizeof(indented) - 1U);
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(0U),
                                (const u8 *)"ghost", 5U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    ed.win->cs.curs.data[0].pos = BYTEOFF(4U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(4U);
    suggestion = suggestion_for(&ed, 2U, BYTEOFF(4U),
                                (const u8 *)"ghost", 5U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 2U);
    yew_ed_free(&ed);

    shadow_fixture(&ed, whitespace, sizeof(whitespace) - 1U);
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(0U),
                                (const u8 *)"ghost", 5U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(ed.win->shadow.live);
    yew_ed_free(&ed);

    shadow_fixture(&ed, indented, sizeof(indented) - 1U);
    YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_SCOPE_DECLARED, "shadow.midline",
                           14U, &yes, &err));
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(0U),
                                (const u8 *)"ghost", 5U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(ed.win->shadow.live);
    yew_shadow_dismiss(&ed, ed.win);
    suggestion = suggestion_for(&ed, 2U, BYTEOFF(0U), multiline,
                                sizeof(multiline) - 1U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 2U);
    ed.win->cs.curs.data[0].pos = BYTEOFF(sizeof(indented) - 1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(sizeof(indented) - 1U);
    suggestion = suggestion_for(&ed, 3U,
                                BYTEOFF(sizeof(indented) - 1U), multiline,
                                sizeof(multiline) - 1U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT(textbuf_eq(ed.win->buf->tb, indented,
                          sizeof(indented) - 1U));
    yew_ed_free(&ed);
}

void test_shadow_edit_keeps_only_a_matching_typed_prefix(void)
{
    Ed ed;
    EditCtx edit;
    ShadowSug suggestion;

    shadow_fixture(&ed, NULL, 0U);
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(0U),
                                (const u8 *)"hello", 5U);
    yew_shadow_deliver(&ed, &suggestion);
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"he", 2U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.consumed, 2U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.buf_gen, ed.buffer.tb->gen);
    YEW_ASSERT_EQ_U64(ed.win->shadow.seq_min[YEW_SHADOW_INDEX],
                      ed.win->shadow.seq_next[YEW_SHADOW_INDEX]);

    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(2U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT(textbuf_eq(ed.buffer.tb, (const u8 *)"hex", 3U));
    yew_ed_free(&ed);
}

void test_shadow_delete_always_dismisses(void)
{
    Ed ed;
    EditCtx edit;
    ShadowSug suggestion;

    shadow_fixture(&ed, (const u8 *)"abc", 3U);
    ed.win->cs.curs.data[0].pos = BYTEOFF(3U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(3U);
    suggestion = suggestion_for(&ed, 1U, BYTEOFF(3U),
                                (const u8 *)"def", 3U);
    yew_shadow_deliver(&ed, &suggestion);
    YEW_ASSERT(ed.win->shadow.live);
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_delete(&edit, (Span){2U, 3U}));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT(textbuf_eq(ed.buffer.tb, (const u8 *)"ab", 2U));
    yew_ed_free(&ed);
}
