#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/shadow_ai.h"
#include "text/edit.h"

static AiCall *frame_call(Ed *ed, u32 seq)
{
    AiCall *call;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    call = &ed->ai->call;
    (void)memset(call, 0, sizeof(*call));
    call->ed = ed;
    call->active = true;
    call->live = true;
    call->seq = seq;
    call->buf_id = ed->win->buf->id;
    call->buf_gen = ed->win->buf->tb->gen;
    call->pos = BYTEOFF(0U);
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
    ed->win->shadow.seq_next[YEW_SHADOW_AI] = seq + 1U;
    return call;
}

static void frame_option(Ed *ed, i64 value)
{
    OptVal option = {YEW_OPT_INT, {.i = value}};
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.frame_ms", 11U,
                           &option, &error));
    YEW_ASSERT_NULL(error);
}

void test_ai_frame_batches_all_ready_tokens_once(void)
{
    Ed ed;
    AiCall *call = frame_call(&ed, 7U);
    u8 tokens[500];

    (void)memset(tokens, 'x', sizeof(tokens));
    bytebuf_append(&call->raw, tokens, sizeof(tokens));
    call->dirty = true;
    yew_ai_shadow_pump(&ed);

    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.prov, YEW_SHADOW_AI);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.len, sizeof(tokens));
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, tokens, sizeof(tokens));
    YEW_ASSERT_EQ_U64(call->delivered, sizeof(tokens));
    YEW_ASSERT(!call->dirty);
    yew_ed_free(&ed);
}

void test_ai_frame_first_token_bypasses_the_floor(void)
{
    Ed ed;
    AiCall *call = frame_call(&ed, 8U);

    frame_option(&ed, 200);
    ed.ai->last_deliver_ms = yew_now_ms();
    bytebuf_push_u8(&call->raw, (u8)'x');
    call->dirty = true;
    yew_ai_shadow_pump(&ed);

    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    YEW_ASSERT_EQ_U64(call->delivered, 1U);
    YEW_ASSERT_EQ_I64(yew_ai_shadow_deadline(&ed, yew_now_ms()), -1);
    yew_ed_free(&ed);
}

void test_ai_frame_enforces_the_growth_only_rule(void)
{
    static const char initial[] = "abcdef";
    static const char shorter[] = "```c\nx\n```";
    Ed ed;
    AiCall *call = frame_call(&ed, 9U);

    frame_option(&ed, 0);
    bytebuf_append(&call->raw, initial, sizeof(initial) - 1U);
    call->dirty = true;
    yew_ai_shadow_pump(&ed);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.len, sizeof(initial) - 1U);

    call->raw.len = 0U;
    bytebuf_append(&call->raw, shorter, sizeof(shorter) - 1U);
    call->dirty = true;
    yew_ai_shadow_pump(&ed);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.delivered, 1U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.len, sizeof(initial) - 1U);
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, initial,
                      sizeof(initial) - 1U);
    YEW_ASSERT_EQ_U64(call->delivered, sizeof(initial) - 1U);
    YEW_ASSERT(!call->dirty);
    yew_ed_free(&ed);
}

void test_ai_frame_deadline_wakes_a_dirty_final_batch(void)
{
    Ed ed;
    AiCall *call = frame_call(&ed, 10U);
    i64 now;

    frame_option(&ed, 200);
    bytebuf_push_u8(&call->raw, (u8)'a');
    call->dirty = true;
    yew_ai_shadow_pump(&ed);
    bytebuf_push_u8(&call->raw, (u8)'b');
    call->dirty = true;
    call->transport_done = true;
    now = yew_now_ms();

    YEW_ASSERT(yew_ai_shadow_deadline(&ed, now) >= 0);
    YEW_ASSERT(yew_ai_shadow_deadline(&ed, now) <= 200);
    yew_ed_free(&ed);
}

void test_ai_frame_continues_after_a_matching_typed_prefix(void)
{
    Ed ed;
    AiCall *call = frame_call(&ed, 11U);
    EditCtx edit;

    frame_option(&ed, 0);
    bytebuf_append(&call->raw, "hello", 5U);
    call->dirty = true;
    yew_ai_shadow_pump(&ed);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.seq, 11U);

    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"h", 1U));
    yew_ed_finish_edit(&ed, &edit);
    ed.win->cs.curs.data[0].pos = BYTEOFF(1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(1U);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT(call->active);

    bytebuf_append(&call->raw, " world", 6U);
    call->dirty = true;
    yew_ai_shadow_pump(&ed);
    YEW_ASSERT(ed.win->shadow.live);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.seq, 12U);
    YEW_ASSERT_EQ_U64(ed.win->shadow.sug.buf_gen, ed.win->buf->tb->gen);
    YEW_ASSERT_EQ_I64(yew_shadow_revalidate(ed.win->buf->tb,
                                            &ed.win->shadow.sug,
                                            BYTEOFF(1U)), 1);
    YEW_ASSERT_EQ_MEM(ed.win->shadow.sug.text, "hello world", 11U);
    yew_ed_free(&ed);
}
