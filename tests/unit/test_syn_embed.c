#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "syn/defs.h"

typedef struct EmbedDiag {
    FlDiagLevel level[32];
    char msg[32][256];
    u32 n;
} EmbedDiag;

typedef struct EmbedFix {
    Arena arena;
    DiagCtx dc;
    EmbedDiag diag;
} EmbedFix;

static void embed_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    EmbedDiag *diag = ctx;

    (void)sp;
    (void)rendered;
    if (diag->n < YEW_ARRAY_LEN(diag->msg)) {
        diag->level[diag->n] = level;
        (void)snprintf(diag->msg[diag->n], sizeof(diag->msg[diag->n]),
                       "%s", msg);
    }
    diag->n++;
}

static void embed_fix_open(EmbedFix *fix)
{
    (void)memset(fix, 0, sizeof(*fix));
    arena_init(&fix->arena);
    fl_diag_init(&fix->dc, &fix->arena);
    fl_diag_set_sink(&fix->dc, embed_sink, &fix->diag);
}

static SynDef *embed_compile(EmbedFix *fix, const char *src, u32 *errors)
{
    u32 warnings;
    u32 file = fl_diag_add_file(&fix->dc, "embed.fl", src, strlen(src));

    return yew_syn_def_compile(&fix->arena, &fix->dc, (const u8 *)src,
                               strlen(src), file, errors, &warnings);
}

static void embed_fix_close(EmbedFix *fix, SynDef *def)
{
    if (def != NULL)
        yew_syn_def_dispose(def);
    arena_free_all(&fix->arena);
}

static bool embed_diag_has(const EmbedFix *fix, const char *want)
{
    u32 i;

    for (i = 0U; i < fix->diag.n && i < YEW_ARRAY_LEN(fix->diag.msg); i++) {
        if (fix->diag.level[i] == FL_DIAG_ERROR &&
            strcmp(fix->diag.msg[i], want) == 0)
            return true;
    }
    return false;
}

static void expect_embed_error(const char *src, const char *message)
{
    EmbedFix fix;
    SynDef *def;
    u32 errors;

    embed_fix_open(&fix);
    def = embed_compile(&fix, src, &errors);
    YEW_ASSERT_NULL(def);
    YEW_ASSERT(errors > 0U);
    YEW_ASSERT(embed_diag_has(&fix, message));
    embed_fix_close(&fix, def);
}

void test_syn_embed_all_keys_compile_to_opener_and_bridge(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"embed-ok\"},contexts:{\n"
        "main:{rules:[\n"
        " {match:\"(js)\",push:\"bridge\",embed:{lang:\"@1\",ctx:\"tag\",end:\"inline\",defer:true,fallback:\"code\"}},\n"
        " {match:\"script\",push:\"bridge\",embed:{lang:\"javascript\",ctx:\"tag\",end:\"inline\",fallback:\"code\"}},\n"
        " {match:\"self\",push:\"selfbridge\",embed:{lang:\"@self\",end:\"line\"}}]},\n"
        "bridge:{default:\"comment\",rules:[{match:\"close\",pop:1,end:true}]},\n"
        "selfbridge:{default:\"string\",at_eol:\"pop\",rules:[]}\n"
        "}}";
    EmbedFix fix;
    SynDef *def;
    const SynRule *dynamic;
    const SynRule *literal;
    const SynRule *self;
    const SynCtx *bridge;
    u32 errors;

    embed_fix_open(&fix);
    def = embed_compile(&fix, src, &errors);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(errors, 0U);
    dynamic = &def->rules[0];
    literal = &def->rules[1];
    self = &def->rules[2];
    bridge = &def->ctxs[1];

    YEW_ASSERT_EQ_U64(dynamic->op, SYN_OP_EMBED);
    YEW_ASSERT_EQ_U64(dynamic->target, 1U);
    YEW_ASSERT_EQ_U64(dynamic->embed.lang_kind, SYN_EMBED_LANG_CAPTURE);
    YEW_ASSERT_EQ_U64(dynamic->embed.lang_group, 1U);
    YEW_ASSERT_EQ_U64(dynamic->embed.end, SYN_EMBED_END_INLINE);
    YEW_ASSERT(dynamic->embed.flags & YEW_SYN_EMBED_DEFER);
    YEW_ASSERT_EQ_U64(dynamic->embed.fallback, YEW_ATTR_CODE);
    YEW_ASSERT_EQ_STR(yew_intern_str(def->aux, dynamic->embed.ctx), "tag");

    YEW_ASSERT_EQ_U64(literal->op, SYN_OP_EMBED);
    YEW_ASSERT_EQ_U64(literal->embed.lang_kind, SYN_EMBED_LANG_LITERAL);
    YEW_ASSERT_EQ_STR(yew_intern_str(def->aux, literal->embed.lang),
                      "javascript");
    YEW_ASSERT_EQ_U64(literal->embed.flags, 0U);

    YEW_ASSERT_EQ_U64(self->embed.lang_kind, SYN_EMBED_LANG_SELF);
    YEW_ASSERT_EQ_U64(self->embed.end, SYN_EMBED_END_LINE);
    YEW_ASSERT_EQ_U64(self->embed.fallback, YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(self->embed.ctx, 0U);

    YEW_ASSERT(bridge->flags & YEW_SYN_CTX_EMBED_BRIDGE);
    YEW_ASSERT_EQ_U64(bridge->embed.lang_kind, SYN_EMBED_LANG_NONE);
    YEW_ASSERT_EQ_STR(yew_intern_str(def->aux, bridge->embed.ctx), "tag");
    YEW_ASSERT_EQ_U64(bridge->embed.end, SYN_EMBED_END_INLINE);
    YEW_ASSERT_EQ_U64(bridge->embed.fallback, YEW_ATTR_CODE);
    YEW_ASSERT_EQ_U64(def->rules[3].end, 1U);
    YEW_ASSERT(def->ctxs[2].flags & YEW_SYN_CTX_EMBED_BRIDGE);
    embed_fix_close(&fix, def);
}

void test_syn_embed_requires_lang_end_and_string_push(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",embed:{lang:\"js\",end:\"line\"}}]}}}",
        "embed requires exactly one string 'push' bridge target");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:[\"b\"],embed:{lang:\"js\",end:\"line\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed bridge 'push' must be one context name, not a list");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{end:\"line\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.lang must be a string");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.end must be a string");
}

void test_syn_embed_rejects_incompatible_state_operations(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",pop:1,embed:{lang:\"js\",end:\"line\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed cannot be combined with 'pop' or 'set'; it may only accompany its string 'push' bridge target");
}

void test_syn_embed_validates_capture_end_and_field_types(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"(x)\",push:\"b\",embed:{lang:\"@2\",end:\"line\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.lang: @2 but the pattern has 1 capture groups");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\",end:true}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.end must be a string");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\",end:\"line\",defer:\"yes\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.defer must be a boolean");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\",ctx:false,end:\"line\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "embed.ctx must be a string");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\",end:\"line\",fallback:\"wat\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "unknown attr 'wat' (did you mean 'tag'?)");
}

void test_syn_embed_end_is_scoped_to_bridge_context(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",pop:1,end:true}]}}}",
        "'end' marks a rule that returns from an embedded language");
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"x\",push:\"b\",embed:{lang:\"js\",end:\"inline\"}}]},b:{rules:[{match:\"y\"}]}}}",
        "context 'b' embeds a language it can never leave");
}

void test_syn_embed_bridge_policy_must_be_compatible(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{rules:[{match:\"a\",push:\"b\",embed:{lang:\"js\",end:\"line\"}},{match:\"z\",push:\"b\",embed:{lang:\"css\",end:\"inline\"}}]},b:{at_eol:\"pop\",rules:[]}}}",
        "context 'b' has incompatible embed bridge descriptors");
}

void test_syn_embed_context_source_key_remains_invalid(void)
{
    expect_embed_error(
        "{syntax:1,language:{name:\"x\"},contexts:{main:{embed:{lang:\"js\",end:\"line\"},rules:[]}}}",
        "'embed' is only valid on a rule");
}

void test_syn_embed_warm_cache_preserves_rule_and_bridge_descriptors(void)
{
    static const char src[] =
        "{syntax:1,language:{name:\"embed-cache\"},contexts:{"
        "main:{rules:[{match:\"(js)\",push:\"b\",embed:{lang:\"@1\",ctx:\"tag\",end:\"inline\",defer:true,fallback:\"code\"}}]},"
        "b:{rules:[{match:\"close\",pop:1,end:true}]}}}";
    char root[] = "/tmp/yew-syn-embed-XXXXXX";
    char source[160];
    char dir[160];
    char *cache;
    char *saved_xdg = getenv("XDG_CACHE_HOME") == NULL ? NULL :
                      strdup(getenv("XDG_CACHE_HOME"));
    char *saved_no_cache = getenv("YEW_NO_SYN_CACHE") == NULL ? NULL :
                           strdup(getenv("YEW_NO_SYN_CACHE"));
    FILE *file;
    Arena arena;
    DiagCtx dc;
    SynDef *def;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_EQ_I64(setenv("XDG_CACHE_HOME", root, 1), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_NO_SYN_CACHE"), 0);
    yew_syn_cache_set_bypass(false);
    (void)snprintf(source, sizeof(source), "%s/embed.fl", root);
    file = fopen(source, "wb");
    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(src, 1U, strlen(src), file), strlen(src));
    YEW_ASSERT_EQ_I64(fclose(file), 0);

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    yew_syn_compile_count_reset();
    def = yew_syn_def_load(&arena, &dc, source);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 1U);
    yew_syn_def_dispose(def);
    arena_free_all(&arena);

    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    yew_syn_compile_count_reset();
    def = yew_syn_def_load(&arena, &dc, source);
    YEW_ASSERT_NOT_NULL(def);
    YEW_ASSERT_EQ_U64(yew_syn_compile_count(), 0U);
    YEW_ASSERT_EQ_U64(def->rules[0].op, SYN_OP_EMBED);
    YEW_ASSERT_EQ_U64(def->rules[0].embed.lang_kind,
                      SYN_EMBED_LANG_CAPTURE);
    YEW_ASSERT_EQ_U64(def->rules[0].embed.lang_group, 1U);
    YEW_ASSERT(def->rules[0].embed.flags & YEW_SYN_EMBED_DEFER);
    YEW_ASSERT_EQ_STR(yew_intern_str(def->aux, def->rules[0].embed.ctx),
                      "tag");
    YEW_ASSERT(def->ctxs[1].flags & YEW_SYN_CTX_EMBED_BRIDGE);
    YEW_ASSERT_EQ_U64(def->ctxs[1].embed.lang_kind, SYN_EMBED_LANG_NONE);
    YEW_ASSERT_EQ_U64(def->ctxs[1].embed.end, SYN_EMBED_END_INLINE);
    YEW_ASSERT_EQ_STR(yew_intern_str(def->aux, def->ctxs[1].embed.ctx),
                      "tag");
    yew_syn_def_dispose(def);
    arena_free_all(&arena);

    cache = yew_syn_cache_path("embed-cache");
    YEW_ASSERT_NOT_NULL(cache);
    YEW_ASSERT_EQ_I64(unlink(cache), 0);
    free(cache);
    YEW_ASSERT_EQ_I64(unlink(source), 0);
    (void)snprintf(dir, sizeof(dir), "%s/yew/syn", root);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    (void)snprintf(dir, sizeof(dir), "%s/yew", root);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(root), 0);
    if (saved_xdg != NULL) {
        YEW_ASSERT_EQ_I64(setenv("XDG_CACHE_HOME", saved_xdg, 1), 0);
        free(saved_xdg);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("XDG_CACHE_HOME"), 0);
    }
    if (saved_no_cache != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_NO_SYN_CACHE", saved_no_cache, 1), 0);
        free(saved_no_cache);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_NO_SYN_CACHE"), 0);
    }
}
