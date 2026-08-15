#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/lsp/client.h"
#include "util/arena.h"

static JsonValue *parse(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *v = yew_json_parse(arena, (const u8 *)json,
                                  (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(v);
    return v;
}

void test_lsp_caps_accept_boolean_object_and_reject_false_null_absent(void)
{
    static const struct { const char *key; u32 bit; } caps[] = {
        {"completionProvider", YEW_LSPC_COMPLETION},
        {"hoverProvider", YEW_LSPC_HOVER},
        {"signatureHelpProvider", YEW_LSPC_SIGNATURE},
        {"definitionProvider", YEW_LSPC_DEFINITION},
        {"declarationProvider", YEW_LSPC_DECLARATION},
        {"typeDefinitionProvider", YEW_LSPC_TYPE_DEFINITION},
        {"implementationProvider", YEW_LSPC_IMPLEMENTATION},
        {"referencesProvider", YEW_LSPC_REFERENCES},
        {"documentHighlightProvider", YEW_LSPC_DOCUMENT_HIGHLIGHT},
        {"documentSymbolProvider", YEW_LSPC_DOCUMENT_SYMBOL},
        {"renameProvider", YEW_LSPC_RENAME},
        {"workspaceSymbolProvider", YEW_LSPC_WORKSPACE_SYMBOL}
    };
    static const struct { const char *value; bool supported; } shapes[] = {
        {"true", true}, {"{}", true}, {"false", false}, {"null", false}
    };
    size_t i;
    size_t j;

    for (i = 0U; i < YEW_ARRAY_LEN(caps); i++) {
        for (j = 0U; j < YEW_ARRAY_LEN(shapes); j++) {
            char json[192];
            Arena arena;
            LspCaps got;
            LspServer server;
            u8 enc;
            bool unknown;

            (void)snprintf(json, sizeof(json),
                "{\"capabilities\":{\"textDocumentSync\":2,\"%s\":%s}}",
                caps[i].key, shapes[j].value);
            arena_init(&arena);
            yew_lsp_caps_parse(&got, parse(&arena, json), &enc, &unknown);
            (void)memset(&server, 0, sizeof(server));
            server.caps = got;
            YEW_ASSERT_EQ_U64(yew_lsp_has(&server, caps[i].bit),
                              shapes[j].supported);
            arena_free_all(&arena);
        }
        {
            Arena arena;
            LspCaps got;
            LspServer server;
            u8 enc;
            bool unknown;

            arena_init(&arena);
            yew_lsp_caps_parse(&got, parse(&arena,
                "{\"capabilities\":{\"textDocumentSync\":2}}"),
                &enc, &unknown);
            (void)memset(&server, 0, sizeof(server));
            server.caps = got;
            YEW_ASSERT(!yew_lsp_has(&server, caps[i].bit));
            arena_free_all(&arena);
        }
    }
}

void test_lsp_caps_parse_sync_details_and_triggers(void)
{
    static const char json[] =
        "{\"capabilities\":{"
        "\"textDocumentSync\":{\"change\":1,\"save\":{\"includeText\":true}},"
        "\"completionProvider\":{\"resolveProvider\":true,"
        "\"triggerCharacters\":[\".\",\":\"]},"
        "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
        "\"positionEncoding\":\"utf-8\"}}";
    Arena arena;
    LspCaps got;
    u8 enc;
    bool unknown;

    arena_init(&arena);
    yew_lsp_caps_parse(&got, parse(&arena, json), &enc, &unknown);
    YEW_ASSERT_EQ_U64(got.sync_kind, 1U);
    YEW_ASSERT(got.save_include_text);
    YEW_ASSERT(got.resolve_completion);
    YEW_ASSERT_EQ_STR(got.trigger_chars, ".:");
    YEW_ASSERT_EQ_STR(got.sig_trigger, "(,");
    YEW_ASSERT_EQ_U64(enc, YEW_POSENC_UTF8);
    YEW_ASSERT(!unknown);
    arena_free_all(&arena);
}

void test_lsp_caps_position_encoding_fallbacks_to_utf16(void)
{
    static const struct { const char *member; bool unknown; } cases[] = {
        {"", false}, {",\"positionEncoding\":\"utf-16\"", false},
        {",\"positionEncoding\":\"utf-32\"", true},
        {",\"positionEncoding\":17", true}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        char json[160];
        Arena arena;
        LspCaps got;
        u8 enc = 0U;
        bool unknown = false;

        (void)snprintf(json, sizeof(json),
            "{\"capabilities\":{\"textDocumentSync\":2%s}}",
            cases[i].member);
        arena_init(&arena);
        yew_lsp_caps_parse(&got, parse(&arena, json), &enc, &unknown);
        YEW_ASSERT_EQ_U64(enc, YEW_POSENC_UTF16);
        YEW_ASSERT_EQ_U64(unknown, cases[i].unknown);
        arena_free_all(&arena);
    }
}

void test_lsp_initialize_advertises_conservative_client(void)
{
    const LspServerCfg *cfg = yew_lsp_default_cfg("c");
    Bytebuf params;
    Arena arena;
    JsonValue *root;
    const JsonValue *encodings;

    bytebuf_init(&params);
    yew_lsp_initialize_params(&params, cfg, "/work/tree", 4711);
    arena_init(&arena);
    root = yew_json_parse(&arena, params.data, params.len, NULL);
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_get(root, "processId"), 0), 4711);
    encodings = yew_json_path(root,
        "capabilities.general.positionEncodings");
    YEW_ASSERT(yew_json_streq(yew_json_at(encodings, 0U), "utf-8"));
    YEW_ASSERT(!yew_json_bool(yew_json_path(root,
        "capabilities.textDocument.completion.completionItem.snippetSupport"),
        true));
    YEW_ASSERT(!yew_json_bool(yew_json_path(root,
        "capabilities.workspace.applyEdit"), true));
    YEW_ASSERT(yew_json_bool(yew_json_path(root,
        "capabilities.textDocument.publishDiagnostics.versionSupport"),
        false));
    arena_free_all(&arena);
    bytebuf_free(&params);
}
