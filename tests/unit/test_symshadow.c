#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "ws/symidx.h"
#include "ws/symshadow.h"

static void symshadow_fixture(Ed *ed, const u8 *bytes, size_t len)
{
    SymIndex *idx;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "symshadow"));
    ed->win->cs.curs.data[ed->win->cs.primary].pos = BYTEOFF(len);
    ed->win->cs.curs.data[ed->win->cs.primary].anchor = BYTEOFF(len);
    idx = yew_symidx_buffer(&ed->ws, ed->win->buf->id, true);
    YEW_ASSERT_NOT_NULL(idx);
    YEW_ASSERT(yew_symidx_scan(idx, ed->win->buf, (Span){0U, len}) != 0U);
}

static bool symshadow_request(Ed *ed)
{
    const ShadowProvider *provider = yew_symshadow_provider();
    ShadowReq request;

    request.buf_id = ed->win->buf->id;
    request.buf_gen = ed->win->buf->tb->gen;
    request.pos = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    request.line = yew_textbuf_line_span(
        ed->win->buf->tb,
        yew_textbuf_line_of(ed->win->buf->tb, request.pos));
    request.seq = ed->win->shadow.seq_next[YEW_SHADOW_INDEX]++;
    request.prov = (u8)YEW_SHADOW_INDEX;
    return provider->request(ed, &request);
}

void test_symshadow_delivers_only_the_top_label_remainder(void)
{
    static const u8 fixture[] = "alphabet\nalp";
    Ed ed;
    const ShadowProvider *provider = yew_symshadow_provider();

    symshadow_fixture(&ed, fixture, sizeof(fixture) - 1U);
    YEW_ASSERT_EQ_STR(provider->name, "index");
    YEW_ASSERT_EQ_U64(provider->prov, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(provider->debounce_ms, 0U);
    YEW_ASSERT(provider->cancel == NULL);
    YEW_ASSERT(symshadow_request(&ed));
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.selected, YEW_SHADOW_INDEX);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.pos.v, sizeof(fixture) - 1U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.len, 5U);
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, "habet", 5U);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    yew_ed_free(&ed);
}

void test_symshadow_rejects_short_and_exhausted_stems(void)
{
    static const u8 short_fixture[] = "alphabet\nal";
    static const u8 exact_fixture[] = "alphabet\nalphabet";
    static const u8 fuzzy_fixture[] = "alphabet\naht";
    static const u8 weak_fixture[] =
        "axbxc_abc_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    Ed ed;

    symshadow_fixture(&ed, short_fixture, sizeof(short_fixture) - 1U);
    YEW_ASSERT(!symshadow_request(&ed));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 0U);
    yew_ed_free(&ed);

    symshadow_fixture(&ed, exact_fixture, sizeof(exact_fixture) - 1U);
    YEW_ASSERT(!symshadow_request(&ed));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 0U);
    yew_ed_free(&ed);

    symshadow_fixture(&ed, (const u8 *)"abc", 3U);
    {
        Buffer other = {0};
        SymHit hit;
        SymQuery query = {"abc", 3U, ed.win->buf->id, BYTEOFF(3U),
                          1U, true};

        other.owner = &ed;
        other.tb = yew_textbuf_from_bytes(weak_fixture,
                                           sizeof(weak_fixture) - 1U);
        other.path = "/elsewhere/weak.c";
        yew_syn_buf_init(&other.syn);
        yew_syn_attach(&other.syn, YEW_LANG_NONE, other.tb);
        YEW_ASSERT_EQ_U64(yew_symidx_scan(
            &ed.ws.sym_ws, &other,
            (Span){0U, sizeof(weak_fixture) - 1U}), 1U);
        ed.ws.sym_ws.tick = ed.ws.sym_ws.updated.data[0] + 64U;
        YEW_ASSERT_EQ_U64(yew_symidx_query(&ed.ws, &query, &hit, 1U), 1U);
        YEW_ASSERT(hit.rank < YEW_SYM_GHOST_MIN);
        yew_syn_detach(&other.syn);
        yew_textbuf_free(other.tb);
    }
    YEW_ASSERT(!symshadow_request(&ed));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 0U);
    yew_ed_free(&ed);

    /* A fuzzy menu candidate is not valid ghost suffix unless its label
     * begins byte-exactly with the typed stem. */
    symshadow_fixture(&ed, fuzzy_fixture, sizeof(fuzzy_fixture) - 1U);
    YEW_ASSERT(!symshadow_request(&ed));
    YEW_ASSERT(!ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 0U);
    yew_ed_free(&ed);
}
