/* Sprint 43: one window timer fans out to providers at their own delays. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "edit/option.h"
#include "edit/shadow.h"

static u32 request_calls[YEW_SHADOW_NPROV];
static u32 cancel_calls[YEW_SHADOW_NPROV];
static ShadowReq last_request[YEW_SHADOW_NPROV];

static bool fake_request(Ed *ed, const ShadowReq *request)
{
    u8 prov;

    (void)ed;
    YEW_ASSERT_NOT_NULL(request);
    prov = request->prov;
    YEW_ASSERT(prov < (u8)YEW_SHADOW_NPROV);
    request_calls[prov]++;
    last_request[prov] = *request;
    return true;
}

static void fake_cancel(Ed *ed, u32 buf_id, u32 up_to)
{
    (void)ed;
    (void)buf_id;
    (void)up_to;
    cancel_calls[YEW_SHADOW_LSP]++;
}

static void fake_cancel_ai(Ed *ed, u32 buf_id, u32 up_to)
{
    (void)ed;
    (void)buf_id;
    (void)up_to;
    cancel_calls[YEW_SHADOW_AI]++;
}

void yew_test_shadow_providers_register(void)
{
    static const ShadowProvider providers[YEW_SHADOW_NPROV] = {
        {"index", YEW_SHADOW_INDEX, 0U, fake_request, NULL},
        {"lsp", YEW_SHADOW_LSP, 120U, fake_request, fake_cancel},
        {"ai", YEW_SHADOW_AI, 350U, fake_request, fake_cancel_ai},
    };
    static bool registered;
    u32 i;

    if (registered)
        return;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        yew_shadow_register(&providers[i]);
    registered = true;
}

static void fake_counts_reset(void)
{
    (void)memset(request_calls, 0, sizeof(request_calls));
    (void)memset(cancel_calls, 0, sizeof(cancel_calls));
    (void)memset(last_request, 0, sizeof(last_request));
}

static void debounce_fixture(Ed *ed, const u8 *bytes, size_t len,
                             u64 cursor)
{
    yew_test_shadow_providers_register();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "shadow-debounce"));
    ed->win->cs.curs.data[ed->win->cs.primary].pos = BYTEOFF(cursor);
    ed->win->cs.curs.data[ed->win->cs.primary].anchor = BYTEOFF(cursor);
    fake_counts_reset();
}

static void fire_at(Ed *ed, i64 now_ms)
{
    ed->now_ms = now_ms;
    yew_timers_fire(&ed->timers, ed, now_ms);
}

static bool set_option(Ed *ed, const char *name, OptVal value,
                       const char **err)
{
    return yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, name,
                       (u32)strlen(name), &value, err);
}

void test_shadow_debounce_fans_one_timer_out_at_provider_deadlines(void)
{
    Ed ed;
    u32 i;

    debounce_fixture(&ed, NULL, 0U, 0U);
    ed.now_ms = 1000;
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, ed.now_ms), 0);

    fire_at(&ed, 1000);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_INDEX], 1U);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_LSP], 0U);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_AI], 0U);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, ed.now_ms), 120);

    fire_at(&ed, 1119);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_LSP], 0U);
    fire_at(&ed, 1120);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_LSP], 1U);
    YEW_ASSERT_EQ_I64(yew_timers_deadline(&ed.timers, ed.now_ms), 230);
    fire_at(&ed, 1350);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_AI], 1U);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.requests, 3U);

    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        YEW_ASSERT_EQ_U64(last_request[i].buf_id, ed.win->buf->id);
        YEW_ASSERT_EQ_U64(last_request[i].buf_gen, ed.win->buf->tb->gen);
        YEW_ASSERT_EQ_U64(last_request[i].pos.v, 0U);
        YEW_ASSERT_EQ_U64(last_request[i].line.lo, 0U);
        YEW_ASSERT_EQ_U64(last_request[i].line.hi, 0U);
        YEW_ASSERT_EQ_U64(last_request[i].seq, 1U);
        YEW_ASSERT_EQ_U64(last_request[i].prov, i);
    }
    yew_ed_free(&ed);
}

void test_shadow_debounce_burst_keeps_one_timer_and_one_request_each(void)
{
    Ed ed;
    u32 i;

    debounce_fixture(&ed, NULL, 0U, 0U);
    for (i = 0U; i < 100U; i++) {
        ed.now_ms = 2000 + (i64)i;
        yew_shadow_arm(&ed, ed.win);
        YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    }
    YEW_ASSERT_EQ_U64(cancel_calls[YEW_SHADOW_LSP], 100U);
    YEW_ASSERT_EQ_U64(cancel_calls[YEW_SHADOW_AI], 100U);
    fire_at(&ed, 2099);
    fire_at(&ed, 2219);
    fire_at(&ed, 2449);
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        YEW_ASSERT_EQ_U64(request_calls[i], 1U);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.requests, 3U);
    YEW_ASSERT_EQ_U64(ed.shadow_stats.dropped_stale, 0U);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_ed_free(&ed);
}

void test_shadow_debounce_policy_gates_and_provider_filter(void)
{
    Ed ed;
    Cursor extra = {BYTEOFF(1U), {0U}, BYTEOFF(1U)};
    OptVal yes = {YEW_OPT_BOOL, {.b = true}};
    OptVal no = {YEW_OPT_BOOL, {.b = false}};
    OptVal index_only = {YEW_OPT_STR, {.str = {"index", 5U}}};
    const char *err = NULL;

    debounce_fixture(&ed, (const u8 *)"abc", 3U, 1U);
    ed.now_ms = 3000;
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    YEW_ASSERT(set_option(&ed, "shadow.midline", yes, &err));
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_shadow_dismiss(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    YEW_ASSERT(set_option(&ed, "shadow.midline", no, &err));
    ed.win->cs.curs.data[0].pos = BYTEOFF(3U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(3U);
    ed.headless = true;
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    ed.headless = false;

    YEW_ASSERT(yew_cset_add(&ed.win->cs, extra));
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    yew_cset_remove_all_but_primary(&ed.win->cs);
    ed.win->cs.curs.data[0].pos = BYTEOFF(2U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(1U);
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);

    ed.win->cs.curs.data[0].pos = BYTEOFF(3U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(3U);
    YEW_ASSERT(set_option(&ed, "shadow.enable", no, &err));
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 0U);
    YEW_ASSERT(set_option(&ed, "shadow.enable", yes, &err));
    YEW_ASSERT(set_option(&ed, "shadow.providers", index_only, &err));
    yew_shadow_arm(&ed, ed.win);
    fire_at(&ed, 3000);
    fire_at(&ed, 5000);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_INDEX], 1U);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_LSP], 0U);
    YEW_ASSERT_EQ_U64(request_calls[YEW_SHADOW_AI], 0U);
    yew_ed_free(&ed);

    debounce_fixture(&ed, (const u8 *)"a b", 3U, 1U);
    ed.now_ms = 6000;
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);
}

void test_shadow_provider_option_rejects_unknown_and_duplicate_names(void)
{
    Ed ed;
    OptVal duplicate = {YEW_OPT_STR, {.str = {"index lsp index", 15U}}};
    OptVal unknown = {YEW_OPT_STR, {.str = {"index nope", 10U}}};
    const char *err = NULL;

    debounce_fixture(&ed, NULL, 0U, 0U);
    YEW_ASSERT(!set_option(&ed, "shadow.providers", duplicate, &err));
    YEW_ASSERT_EQ_STR(err,
                      "shadow.providers contains a duplicate provider");
    err = NULL;
    YEW_ASSERT(!set_option(&ed, "shadow.providers", unknown, &err));
    YEW_ASSERT_EQ_STR(err,
                      "shadow.providers accepts only index, lsp, and ai");
    yew_ed_free(&ed);
}

void test_shadow_pending_timer_is_cancelled_before_window_teardown(void)
{
    Ed ed;

    debounce_fixture(&ed, NULL, 0U, 0U);
    ed.now_ms = 7000;
    yew_shadow_arm(&ed, ed.win);
    YEW_ASSERT_EQ_U64(ed.timers.len, 1U);
    yew_ed_free(&ed);
}
