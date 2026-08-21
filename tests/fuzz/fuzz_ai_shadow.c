/*
 * Sprint 49 fuzz: the AI call and Sprint 43 ghost under adversarial event
 * ordering.  Protocol framing has its own fuzz target; this starts after an
 * adapter produced completion bytes and mixes frames, ordinary edits,
 * cursor motion, accepts, cancellation, EOF, invalid UTF-8 and a maximum
 * sized token.  Passive events must never edit the document, and every
 * successful accept must insert a prefix of the currently revalidated
 * ghost.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/shadow_ai.h"
#include "text/edit.h"

enum {
    AI_SHADOW_MAX_OPS = 64,
    AI_SHADOW_MAX_BUFFER = 4096,
    AI_SHADOW_HUGE_TOKEN = 1024 * 1024
};

typedef struct Model {
    u32 started;
    u32 terminal;
} Model;

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static bool materialize(const TextBuf *tb, Bytebuf *out)
{
    TextIter iter;

    out->len = 0U;
    if (yew_textbuf_len(tb) == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return false;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            return false;
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&iter, tb));
    return out->len == yew_textbuf_len(tb);
}

static bool bytes_equal(const Bytebuf *left, const Bytebuf *right)
{
    return left->len == right->len &&
           (left->len == 0U ||
            memcmp(left->data, right->data, left->len) == 0);
}

static void call_begin(Ed *ed, Model *model)
{
    AiCall *call = &ed->ai->call;
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];

    if (call->active)
        return;
    (void)memset(call, 0, sizeof(*call));
    call->ed = ed;
    call->backend.name = "fuzz";
    call->active = true;
    call->live = true;
    call->seq = ed->win->shadow.seq_next[YEW_SHADOW_AI]++;
    call->buf_id = ed->win->buf->id;
    call->buf_gen = ed->win->buf->tb->gen;
    call->pos = cursor->pos;
    call->t_sent = -1;
    call->t_first_token = -1;
    call->t_done = -1;
    call->retry_after_ms = -1;
    arena_init(&call->arena);
    bytebuf_init(&call->raw);
    bytebuf_init(&call->text);
    bytebuf_init(&call->body);
    bytebuf_init(&call->response);
    bytebuf_init(&call->curl_config);
    bytebuf_init(&call->curl_err);
    yew_ai_adapter_state_init(&call->adapter);
    model->started++;
}

static void call_cancel(Ed *ed, Model *model)
{
    if (!ed->ai->call.active)
        return;
    yew_ai_call_abort(ed, &ed->ai->call, YEW_AI_ERR_CANCELLED);
    model->terminal++;
}

static bool append_token(AiCall *call, const u8 *bytes, size_t len)
{
    if (!call->active || len > YEW_HTTP_MAX_BODY - call->raw.len)
        return true;
    bytebuf_append(&call->raw, bytes, len);
    call->dirty = true;
    return true;
}

static bool append_huge(AiCall *call)
{
    u8 *bytes;

    if (!call->active || call->raw.len != 0U)
        return true;
    bytes = malloc(AI_SHADOW_HUGE_TOKEN);
    if (bytes == NULL)
        return false;
    (void)memset(bytes, 'x', AI_SHADOW_HUGE_TOKEN);
    bytebuf_append(&call->raw, bytes, AI_SHADOW_HUGE_TOKEN);
    call->dirty = true;
    free(bytes);
    return true;
}

static bool explicit_insert(Ed *ed, u8 byte)
{
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];
    EditCtx edit = yew_ed_edit_ctx(ed);
    bool ok = yew_edit_insert(&edit, cursor->pos, &byte, 1U);

    yew_ed_finish_edit(ed, &edit);
    return ok;
}

static bool accepted_prefix(const Bytebuf *before, const Bytebuf *after,
                            u64 at, const u8 *ghost, u64 ghost_len,
                            u64 consumed, char *why, size_t why_cap)
{
    size_t inserted;

    if (after->len <= before->len)
        return fail(why, why_cap, "AI accept inserted no bytes");
    inserted = after->len - before->len;
    if (at > before->len || consumed > ghost_len ||
        inserted > ghost_len - consumed)
        return fail(why, why_cap, "AI accept exceeded the ghost");
    if ((at != 0U && memcmp(before->data, after->data, (size_t)at) != 0) ||
        (before->len != at &&
         memcmp(before->data + at, after->data + at + inserted,
                before->len - (size_t)at) != 0) ||
        memcmp(after->data + at, ghost + consumed, inserted) != 0)
        return fail(why, why_cap, "AI accept changed non-ghost bytes");
    return true;
}

static bool accept_one(Ed *ed, u8 selector, const Bytebuf *before,
                       Bytebuf *after, char *why, size_t why_cap)
{
    Shadow *shadow = &ed->win->shadow;
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];
    u8 *ghost;
    u64 ghost_len;
    u64 consumed;
    u64 at;
    i64 validated;
    bool accepted;

    if (!shadow->live || before->len >= AI_SHADOW_MAX_BUFFER)
        return true;
    ghost_len = shadow->sug.len;
    ghost = malloc(ghost_len == 0U ? 1U : (size_t)ghost_len);
    if (ghost == NULL)
        return fail(why, why_cap, "copying AI ghost failed");
    if (ghost_len != 0U)
        (void)memcpy(ghost, shadow->sug.text, (size_t)ghost_len);
    at = cursor->pos.v;
    validated = yew_shadow_revalidate(ed->win->buf->tb, &shadow->sug,
                                      cursor->pos);
    if (validated < 0) {
        free(ghost);
        return fail(why, why_cap, "AI accept reached a stale ghost");
    }
    consumed = (u64)validated;
    if (selector % 3U == 0U)
        accepted = yew_shadow_accept_word(ed, ed->win, false);
    else if (selector % 3U == 1U)
        accepted = yew_shadow_accept_line(ed, ed->win);
    else
        accepted = yew_shadow_accept_all(ed, ed->win);
    if (!materialize(ed->win->buf->tb, after)) {
        free(ghost);
        return fail(why, why_cap, "materializing AI accept failed");
    }
    if (accepted && !accepted_prefix(before, after, at, ghost, ghost_len,
                                     consumed, why, why_cap)) {
        free(ghost);
        return false;
    }
    if (!accepted && !bytes_equal(before, after)) {
        free(ghost);
        return fail(why, why_cap, "refused AI accept changed the buffer");
    }
    free(ghost);
    return true;
}

static bool check_ai_shadow(const u8 *data, size_t len, char *why,
                            size_t why_cap)
{
    static const u8 seed[] = "prefix ";
    Ed ed;
    Model model = {0U, 0U};
    Bytebuf before;
    Bytebuf after;
    size_t ops;
    size_t i;
    bool ok = false;

    bytebuf_init(&before);
    bytebuf_init(&after);
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, seed, sizeof(seed) - 1U, "ai-fuzz"))
        goto done;
    ed.win->cs.curs.data[0].pos = BYTEOFF(sizeof(seed) - 1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(sizeof(seed) - 1U);
    call_begin(&ed, &model);
    ops = len < AI_SHADOW_MAX_OPS ? len : AI_SHADOW_MAX_OPS;
    for (i = 0U; i < ops; i++) {
        AiCall *call = &ed.ai->call;
        u8 action = (u8)(data[i] % 10U);
        bool may_edit = false;

        if (!materialize(ed.win->buf->tb, &before))
            goto done;
        switch (action) {
        case 0:
        case 1:
            if (call->active) {
                u8 token[4] = {data[i], (u8)'x', (u8)'\n', 0xffU};
                size_t token_len = 1U + (size_t)(data[i] % 4U);

                if (!append_token(call, token, token_len))
                    goto done;
            }
            break;
        case 2:
            if (!append_huge(call))
                goto done;
            break;
        case 3:
            yew_ai_shadow_pump(&ed);
            break;
        case 4:
            if (!accept_one(&ed, data[i], &before, &after, why, why_cap))
                goto done;
            may_edit = true;
            break;
        case 5:
            if (before.len < AI_SHADOW_MAX_BUFFER) {
                may_edit = true;
                if (!explicit_insert(&ed, (u8)('a' + data[i] % 26U)))
                    goto done;
            }
            break;
        case 6: {
            u64 pos = before.len == 0U ? 0U : data[i] % (before.len + 1U);

            ed.win->cs.curs.data[0].pos = BYTEOFF(pos);
            ed.win->cs.curs.data[0].anchor = BYTEOFF(pos);
            yew_shadow_dismiss(&ed, ed.win);
            break;
        }
        case 7:
            yew_shadow_dismiss(&ed, ed.win);
            break;
        case 8:
            call_cancel(&ed, &model);
            break;
        default:
            if (!call->active)
                call_begin(&ed, &model);
            break;
        }
        if (!materialize(ed.win->buf->tb, &after))
            goto done;
        if (!may_edit && !bytes_equal(&before, &after)) {
            (void)snprintf(why, why_cap,
                           "passive AI shadow event changed the buffer");
            goto done;
        }
        if (ed.ai->call.active && !ed.ai->call.live) {
            (void)snprintf(why, why_cap, "inactive AI call remained live");
            goto done;
        }
        if (yew_job_running_count(&ed) != 0U || ed.jobs.len != 0U) {
            (void)snprintf(why, why_cap, "AI shadow fuzz leaked a child");
            goto done;
        }
        yew_textbuf_check(ed.win->buf->tb);
    }
    call_cancel(&ed, &model);
    if (model.started == 0U || model.started != model.terminal ||
        ed.ai->call.active || ed.ai->call.live) {
        (void)snprintf(why, why_cap,
                       "AI call did not reach exactly one terminal state");
        goto done;
    }
    ok = true;
done:
    if (ed.ai != NULL && ed.ai->call.active)
        call_cancel(&ed, &model);
    yew_ed_free(&ed);
    bytebuf_free(&before);
    bytebuf_free(&after);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_ai_shadow", NULL,
                         check_ai_shadow);
}
