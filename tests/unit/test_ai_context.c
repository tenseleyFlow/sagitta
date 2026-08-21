#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/ai/context.h"
#include "unicode/utf8.h"

static ShadowReq context_request(const Ed *ed, u64 at)
{
    ShadowReq request = {0};

    request.buf_id = ed->win->buf->id;
    request.buf_gen = ed->win->buf->tb->gen;
    request.pos = BYTEOFF(at);
    request.line = yew_textbuf_line_span(ed->win->buf->tb,
        yew_textbuf_line_of(ed->win->buf->tb, request.pos));
    request.seq = 1U;
    request.prov = YEW_SHADOW_AI;
    return request;
}

void test_ai_context_snaps_budget_at_unicode_graphemes(void)
{
    static const u8 family[] = {
        0xf0U, 0x9fU, 0x91U, 0xa8U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa7U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa6U
    };
    Bytebuf source;
    Ed ed;
    Arena arena;
    AiCtx context;
    AiErr err;
    ShadowReq request;

    bytebuf_init(&source);
    /* cursor 4200 with the default 3072-byte prefix budget cuts at 1128,
     * deliberately through this 25-byte ZWJ family. */
    while (source.len < 1115U)
        bytebuf_push_u8(&source, (u8)'a');
    bytebuf_append(&source, family, sizeof(family));
    while (source.len < 5000U)
        bytebuf_push_u8(&source, (u8)'b');
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, source.data, source.len, "unicode"));
    request = context_request(&ed, 4200U);
    arena_init(&arena);
    YEW_ASSERT(yew_ai_context_build(&ed, ed.win, &request, &arena,
                                    &context, &err));
    YEW_ASSERT(context.plen <= 3072U);
    YEW_ASSERT(context.plen != 0U && context.prefix[0] == (u8)'b');
    YEW_ASSERT(context.slen <= 1024U);
    YEW_ASSERT_EQ_U64(yew_utf8_validate(context.prefix, context.plen),
                      context.plen);
    YEW_ASSERT_EQ_U64(yew_utf8_validate(context.suffix, context.slen),
                      context.slen);
    YEW_ASSERT(context.truncated_head);
    YEW_ASSERT(!context.truncated_tail);
    arena_free_all(&arena);
    yew_ed_free(&ed);
    bytebuf_free(&source);
}

void test_ai_context_reports_relative_path_language_and_edges(void)
{
    static const u8 text[] = "one\ntwo\nthree\n";
    Ed ed;
    Arena arena;
    AiCtx context;
    AiErr err;
    ShadowReq request;
    size_t root_len;
    char *absolute;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U, "nested"));
    root_len = strlen(yew_ws_root(&ed));
    absolute = arena_alloc(&ed.arena, root_len + sizeof("/src/edit/motion.c"),
                           1U);
    (void)memcpy(absolute, yew_ws_root(&ed), root_len);
    (void)memcpy(absolute + root_len, "/src/edit/motion.c",
                 sizeof("/src/edit/motion.c"));
    ed.win->buf->path = absolute;
    ed.win->buf->lang = "c";
    request = context_request(&ed, 4U);
    arena_init(&arena);
    YEW_ASSERT(yew_ai_context_build(&ed, ed.win, &request, &arena,
                                    &context, &err));
    YEW_ASSERT_EQ_STR(context.path, "src/edit/motion.c");
    YEW_ASSERT_EQ_STR(context.lang, "c");
    YEW_ASSERT_EQ_U64(context.line_1based, 2U);
    YEW_ASSERT_EQ_MEM(context.prefix, "one\n", 4U);
    YEW_ASSERT_EQ_MEM(context.suffix, "two\nthree\n", 10U);
    arena_free_all(&arena);

    ed.win->buf->path = "/outside/workspace/private.c";
    ed.win->buf->lang = NULL;
    request = context_request(&ed, 0U);
    arena_init(&arena);
    YEW_ASSERT(yew_ai_context_build(&ed, ed.win, &request, &arena,
                                    &context, &err));
    YEW_ASSERT_EQ_STR(context.path, "");
    YEW_ASSERT_EQ_STR(context.lang, "");
    YEW_ASSERT_EQ_U64(context.plen, 0U);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

static bool context_redact(Ed *ed, const AiCtx *context, RedactHit *hit)
{
    (void)ed;
    (void)context;
    hit->lo = 1U;
    hit->hi = 2U;
    hit->pattern = "fixture";
    return true;
}

void test_ai_context_redaction_is_a_hard_pre_prompt_gate(void)
{
    Ed ed;
    Arena arena;
    AiCtx context;
    AiErr err;
    ShadowReq request;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"secret", 6U, "secret"));
    request = context_request(&ed, 3U);
    arena_init(&arena);
    yew_ai_redact_hook_set(context_redact);
    YEW_ASSERT(!yew_ai_context_build(&ed, ed.win, &request, &arena,
                                     &context, &err));
    YEW_ASSERT_EQ_U64(err.kind, YEW_AI_ERR_CANCELLED);
    yew_ai_redact_hook_set(NULL);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}

void test_ai_context_large_buffer_work_is_budget_bounded(void)
{
    const size_t size = 100U * 1024U * 1024U;
    u8 *bytes = malloc(size);
    Ed ed;
    Arena arena;
    AiCtx context;
    AiErr err;
    ShadowReq request;

    YEW_ASSERT_NOT_NULL(bytes);
    (void)memset(bytes, 'x', size);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, bytes, size, "large"));
    free(bytes);
    request = context_request(&ed, size);
    arena_init(&arena);
    YEW_ASSERT(yew_ai_context_build(&ed, ed.win, &request, &arena,
                                    &context, &err));
    YEW_ASSERT_EQ_U64(context.plen, 3072U);
    YEW_ASSERT_EQ_U64(context.slen, 0U);
    arena_free_all(&arena);
    yew_ed_free(&ed);
}
