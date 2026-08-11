#include "syn_toy.h"

#include <string.h>

#include "util/log.h"

static void first_add(u8 first[32], u8 byte)
{
    first[byte >> 3U] |= (u8)(1U << (byte & 7U));
}

static void rule_init(SynToy *toy, u32 i, const char *pattern, u8 attr,
                      u8 op, u16 target)
{
    SynRule *r = &toy->rules[i];
    YewReErr err = {0U, NULL};

    (void)memset(r, 0, sizeof(*r));
    (void)memset(r->caps, 0xff, sizeof(r->caps));
    r->re = yew_re_compile(&toy->arena, pattern, strlen(pattern), 0U, &err);
    if (r->re == NULL)
        YEW_BUG("toy syntax regex /%s/: %s", pattern,
                err.msg == NULL ? "compile failed" : err.msg);
    r->attr = attr;
    r->op = op;
    r->target = target;
}

static void add_first(SynToy *toy, u32 rule, u32 ctx, const char *bytes)
{
    const u8 *p = (const u8 *)bytes;

    while (*p != 0U) {
        first_add(toy->rules[rule].first, *p);
        first_add(toy->ctxs[ctx].first, *p++);
    }
}

void syn_toy_init(SynToy *toy)
{
    (void)memset(toy, 0, sizeof(*toy));
    arena_init(&toy->arena);
    arena_init(&toy->aux_arena);
    interner_init(&toy->aux, &toy->aux_arena);

    toy->ctxs[SYN_TOY_MAIN] = (SynCtx){0U, 7U, YEW_ATTR_TEXT,
                                       SYN_OP_STAY, 0U, 0U, {0}, 0U};
    toy->ctxs[SYN_TOY_STRING] = (SynCtx){7U, 2U, YEW_ATTR_STRING,
                                         SYN_OP_POP, 1U, 0U, {0}, 0U};
    toy->ctxs[SYN_TOY_COMMENT_BLOCK] =
        (SynCtx){9U, 1U, YEW_ATTR_COMMENT, SYN_OP_STAY, 0U, 0U, {0}, 0U};
    toy->ctxs[SYN_TOY_COMMENT_LINE] =
        (SynCtx){10U, 0U, YEW_ATTR_COMMENT, SYN_OP_POP, 1U, 0U, {0}, 0U};

    rule_init(toy, 0U, "//", YEW_ATTR_COMMENT, SYN_OP_PUSH,
              SYN_TOY_COMMENT_LINE);
    add_first(toy, 0U, SYN_TOY_MAIN, "/");
    rule_init(toy, 1U, "/\\*", YEW_ATTR_COMMENT, SYN_OP_PUSH,
              SYN_TOY_COMMENT_BLOCK);
    add_first(toy, 1U, SYN_TOY_MAIN, "/");
    rule_init(toy, 2U, "\"", YEW_ATTR_STRING, SYN_OP_PUSH,
              SYN_TOY_STRING);
    add_first(toy, 2U, SYN_TOY_MAIN, "\"");
    rule_init(toy, 3U, "\\b(if|else|while|return)\\b",
              YEW_ATTR_KEYWORD_CONTROL, SYN_OP_STAY, 0U);
    add_first(toy, 3U, SYN_TOY_MAIN, "iewr");
    rule_init(toy, 4U, "[0-9]+", YEW_ATTR_NUMBER, SYN_OP_STAY, 0U);
    add_first(toy, 4U, SYN_TOY_MAIN, "0123456789");
    rule_init(toy, 5U, "\\b(true|false)\\b", YEW_ATTR_BOOLEAN,
              SYN_OP_STAY, 0U);
    add_first(toy, 5U, SYN_TOY_MAIN, "tf");
    rule_init(toy, 6U, "[+*/=-]+", YEW_ATTR_OPERATOR, SYN_OP_STAY, 0U);
    add_first(toy, 6U, SYN_TOY_MAIN, "+*/=-");

    rule_init(toy, 7U, "\\\\.", YEW_ATTR_STRING_ESCAPE, SYN_OP_STAY, 0U);
    add_first(toy, 7U, SYN_TOY_STRING, "\\");
    rule_init(toy, 8U, "\"", YEW_ATTR_STRING, SYN_OP_POP, 0U);
    toy->rules[8U].nop = 1U;
    add_first(toy, 8U, SYN_TOY_STRING, "\"");

    rule_init(toy, 9U, "\\*/", YEW_ATTR_COMMENT, SYN_OP_POP, 0U);
    toy->rules[9U].nop = 1U;
    add_first(toy, 9U, SYN_TOY_COMMENT_BLOCK, "*");

    toy->def = (SynDef){"toy", SYN_TOY_MAIN, SYN_TOY_NCTXS, 10U,
                        toy->ctxs, toy->rules, &toy->aux};
    toy->engine = yew_syn_engine_new(&toy->def);
    if (toy->engine == NULL)
        YEW_BUG("toy syntax engine allocation failed");
}

void syn_toy_free(SynToy *toy)
{
    yew_syn_engine_free(toy->engine);
    interner_free(&toy->aux);
    arena_free_all(&toy->aux_arena);
    arena_free_all(&toy->arena);
    (void)memset(toy, 0, sizeof(*toy));
}

u32 syn_toy_state(SynToy *toy, u16 ctx)
{
    SynState state;

    (void)memset(&state, 0, sizeof(state));
    state.f[0].ctx = SYN_TOY_MAIN;
    state.depth = 1U;
    state.ndef = 1U;
    if (ctx != SYN_TOY_MAIN) {
        state.f[1].ctx = ctx;
        state.depth = 2U;
    }
    return yew_syn_state_intern(yew_syn_engine_states(toy->engine), &state);
}

u32 syn_toy_line(SynToy *toy, u32 entry, const char *line,
                 SynSpan *spans, u32 cap, SynLineOut *out)
{
    *out = (SynLineOut){spans, 0U, cap, YEW_SYN_STATE_UNKNOWN,
                        YEW_SYN_STOP_OK};
    yew_syn_line(toy->engine, entry, (const u8 *)line, (u32)strlen(line), out);
    return out->exit_state;
}
