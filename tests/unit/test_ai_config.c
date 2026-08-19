#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "fl/parse.h"
#include "mod/ai/config.h"

typedef struct AiConfigFix {
    Arena arena;
    DiagCtx dc;
    Interner in;
    u32 ndiag;
    FlDiagLevel level;
    FlSpan span;
    char message[256];
    char rendered[1024];
} AiConfigFix;

static void config_sink(void *ctx, FlDiagLevel level, FlSpan span,
                        const char *msg, const char *rendered)
{
    AiConfigFix *f = ctx;

    if (f->ndiag++ != 0U)
        return;
    f->level = level;
    f->span = span;
    (void)snprintf(f->message, sizeof(f->message), "%s", msg);
    (void)snprintf(f->rendered, sizeof(f->rendered), "%s", rendered);
}

static void config_open(AiConfigFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, config_sink, f);
}

static void config_close(AiConfigFix *f)
{
    interner_free(&f->in);
    arena_free_all(&f->arena);
}

static FlNode *config_parse(AiConfigFix *f, const char *src)
{
    u32 file_id = fl_diag_add_file(&f->dc, "init.fl", src, strlen(src));

    return fl_parse_literal(&f->arena, &f->dc, &f->in, src, strlen(src),
                            file_id);
}

static void assert_forbidden(const char *key, bool quoted, u32 line, u32 col)
{
    AiConfigFix f;
    char src[192];
    char want[128];
    FlNode *map;
    HttpUrl url;

    config_open(&f);
    if (quoted)
        (void)snprintf(src, sizeof(src), "{\n  \"%s\": [\"not\", \"relevant\"],\n}\n",
                       key);
    else
        (void)snprintf(src, sizeof(src), "{\n  %s: [\"not\", \"relevant\"],\n}\n",
                       key);
    map = config_parse(&f, src);
    YEW_ASSERT_NOT_NULL(map);
    YEW_ASSERT(!yew_ai_config_validate_backend(&f.arena, &f.dc, &f.in,
                                                map, &url));
    (void)snprintf(want, sizeof(want),
                   "'%s' may not hold a literal API key", key);
    YEW_ASSERT_EQ_U64(f.ndiag, 1U);
    YEW_ASSERT_EQ_U64(f.level, FL_DIAG_ERROR);
    YEW_ASSERT_EQ_U64(f.span.line, line);
    YEW_ASSERT_EQ_U64(f.span.col, col);
    YEW_ASSERT_EQ_U64(f.span.len, strlen(key) + (quoted ? 2U : 0U));
    YEW_ASSERT_EQ_STR(f.message, want);
    YEW_ASSERT(strstr(f.rendered,
        "      = init.fl is pasted into bug reports and committed to "
        "dotfile repos.\n") != NULL);
    YEW_ASSERT(strstr(f.rendered,
        "      = use key_env: \"ANTHROPIC_API_KEY\", or key_cmd: "
        "[\"pass\",\"show\",\"…\"]\n") != NULL);
    {
        const char *caret = strstr(f.rendered, "^" );
        const char *note = strstr(f.rendered, "      = init.fl");

        YEW_ASSERT_NOT_NULL(caret);
        YEW_ASSERT_NOT_NULL(note);
        YEW_ASSERT(caret < note);
    }
    config_close(&f);
}

void test_ai_config_rejects_literal_credential_fields(void)
{
    static const char *const aliases[] = {
        "key", "api_key", "token", "password", "secret"
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(aliases); i++) {
        assert_forbidden(aliases[i], false, 2U, 3U);
        assert_forbidden(aliases[i], true, 2U, 3U);
    }
}

void test_ai_config_accepts_indirect_keys_and_ignores_nested_maps(void)
{
    static const char src[] =
        "{\n"
        "  url: \"http://127.0.0.1:11434/api/generate\",\n"
        "  key_env: \"ANTHROPIC_API_KEY\",\n"
        "  key_cmd: [\"pass\", \"show\", \"api/anthropic\"],\n"
        "  options: {secret: \"belongs to another schema\"},\n"
        "}\n";
    AiConfigFix f;
    FlNode *map;
    HttpUrl url;

    config_open(&f);
    map = config_parse(&f, src);
    YEW_ASSERT_NOT_NULL(map);
    YEW_ASSERT(yew_ai_config_validate_backend(&f.arena, &f.dc, &f.in,
                                               map, &url));
    YEW_ASSERT_EQ_U64(f.ndiag, 0U);
    YEW_ASSERT_EQ_STR(url.host, "127.0.0.1");
    YEW_ASSERT_EQ_U64(url.port, 11434U);
    YEW_ASSERT_EQ_STR(url.path, "/api/generate");
    YEW_ASSERT(yew_ai_config_validate_backend(&f.arena, &f.dc, &f.in,
                                               map, NULL));
    YEW_ASSERT_EQ_U64(f.ndiag, 0U);
    config_close(&f);
}

void test_ai_config_rejects_url_userinfo_at_value_span(void)
{
    static const char src[] =
        "{\n"
        "  url: \"http://user:password@example.test/v1\",\n"
        "}\n";
    AiConfigFix f;
    FlNode *map;
    HttpUrl url;

    config_open(&f);
    map = config_parse(&f, src);
    YEW_ASSERT_NOT_NULL(map);
    YEW_ASSERT(!yew_ai_config_validate_backend(&f.arena, &f.dc, &f.in,
                                                map, &url));
    YEW_ASSERT_EQ_U64(f.ndiag, 1U);
    YEW_ASSERT_EQ_U64(f.level, FL_DIAG_ERROR);
    YEW_ASSERT_EQ_U64(f.span.line, 2U);
    YEW_ASSERT_EQ_U64(f.span.col, 8U);
    YEW_ASSERT_EQ_U64(f.span.len,
                      strlen("\"http://user:password@example.test/v1\""));
    YEW_ASSERT_EQ_STR(f.message, "bad url: userinfo is not allowed");
    config_close(&f);
}
