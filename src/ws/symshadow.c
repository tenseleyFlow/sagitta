#include "ws/symshadow.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "ws/symidx.h"

static bool stem_copy(const TextBuf *tb, Span span, u8 *out)
{
    TextIter iter;
    u64 copied = 0U;
    u64 len = span.hi - span.lo;

    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(span.lo)))
        return false;
    while (copied < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available) ||
            available == 0U)
            return false;
        take = available < len - copied ? available : len - copied;
        (void)memcpy(out + copied, bytes, (size_t)take);
        copied += take;
        if (copied < len && !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

static bool index_request(Ed *ed, const ShadowReq *request)
{
    u8 stem[YEW_SYM_MAX_LEN];
    SymQuery query;
    SymHit hit;
    ShadowSug suggestion = {0};
    UnitCtx unit;
    ByteOff home;
    const char *label;
    size_t label_len;
    u64 stem_len;

    if (ed == NULL || request == NULL || ed->win == NULL ||
        ed->win->buf == NULL || ed->win->buf->tb == NULL ||
        request->prov != (u8)YEW_SHADOW_INDEX ||
        request->buf_id != ed->win->buf->id ||
        request->pos.v > yew_textbuf_len(ed->win->buf->tb))
        return false;

    unit = (UnitCtx){ed->win->buf->tb, ed->win->buf, ed->win};
    home = yew_unit_word.home(&unit, request->pos, false);
    if (home.v > request->pos.v)
        return false;
    stem_len = request->pos.v - home.v;
    if (stem_len < YEW_SYM_GHOST_MIN_STEM ||
        stem_len > YEW_SYM_MAX_LEN ||
        !stem_copy(ed->win->buf->tb,
                   (Span){home.v, request->pos.v}, stem))
        return false;

    query = (SymQuery){(const char *)stem, (u32)stem_len,
                       request->buf_id, request->pos, 1U, true};
    if (yew_symidx_query(&ed->ws, &query, &hit, 1U) != 1U ||
        hit.rank < YEW_SYM_GHOST_MIN)
        return false;
    label = yew_intern_str(&ed->interner, hit.name);
    label_len = yew_intern_len(&ed->interner, hit.name);
    if (label == NULL || label_len <= stem_len || label_len > UINT32_MAX ||
        memcmp(label, stem, (size_t)stem_len) != 0)
        return false;

    suggestion.seq = request->seq;
    suggestion.prov = (u8)YEW_SHADOW_INDEX;
    suggestion.buf_id = request->buf_id;
    suggestion.buf_gen = request->buf_gen;
    suggestion.pos = request->pos;
    suggestion.text = (const u8 *)label + stem_len;
    suggestion.len = (u32)(label_len - stem_len);
    yew_shadow_deliver(ed, &suggestion);
    return true;
}

static const ShadowProvider yew_shadow_index = {
    "index", YEW_SHADOW_INDEX, 0U, index_request, NULL,
};

const ShadowProvider *yew_symshadow_provider(void)
{
    return &yew_shadow_index;
}

void yew_symshadow_install(void)
{
    static bool installed;
    const char *test_provider = getenv("YEW_SHADOW_TEST");

    if (installed ||
        (test_provider != NULL && strcmp(test_provider, "1") == 0))
        return;
    yew_shadow_register(&yew_shadow_index);
    installed = true;
}
