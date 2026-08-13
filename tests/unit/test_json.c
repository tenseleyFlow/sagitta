#include "harness.h"

#include <string.h>

#include "mod/lsp/json.h"
#include "util/arena.h"
#include "util/buf.h"

static JsonValue *parse_text(Arena *a, const char *text, JsonErr *err)
{
    return yew_json_parse(a, (const u8 *)text, (u64)strlen(text), err);
}

static void assert_bad(const char *text, const char *prefix)
{
    Arena a;
    JsonErr err;

    arena_init(&a);
    YEW_ASSERT_NULL(parse_text(&a, text, &err));
    YEW_ASSERT(strncmp(err.msg, prefix, strlen(prefix)) == 0);
    YEW_ASSERT(err.line >= 1u);
    YEW_ASSERT(err.col >= 1u);
    arena_free_all(&a);
}

void test_json_parse_values_and_accessors(void)
{
    static const char doc[] =
        "{\"z\":null,\"a\":[true,false,7,3.5,\"x\\u0000y\"],"
        "\"nested\":{\"line\":12}}";
    Arena a;
    JsonErr err;
    JsonValue *root;
    const JsonValue *array;
    const JsonValue *string;
    const u8 *bytes;
    u32 len = 99u;

    arena_init(&a);
    root = parse_text(&a, doc, &err);
    YEW_ASSERT_NOT_NULL(root);
    YEW_ASSERT_EQ_U64(root->kind, YEW_JS_OBJ);
    YEW_ASSERT_EQ_U64(yew_json_len(root), 3u);
    array = yew_json_get(root, "a");
    YEW_ASSERT_NOT_NULL(array);
    YEW_ASSERT_EQ_U64(yew_json_len(array), 5u);
    YEW_ASSERT(yew_json_bool(yew_json_at(array, 0u), false));
    YEW_ASSERT(!yew_json_bool(yew_json_at(array, 1u), true));
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_at(array, 2u), -1), 7);
    YEW_ASSERT(yew_json_real(yew_json_at(array, 3u), 0.0) == 3.5);
    string = yew_json_at(array, 4u);
    bytes = yew_json_str(string, &len);
    YEW_ASSERT_EQ_U64(len, 3u);
    YEW_ASSERT_EQ_MEM(bytes, "x\0y", 3u);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(root, "nested.line"), -1),
                      12);
    YEW_ASSERT_NULL(yew_json_path(root, "nested.missing"));
    YEW_ASSERT_NULL(yew_json_get(array, "a"));
    YEW_ASSERT_NULL(yew_json_at(root, 0u));
    YEW_ASSERT_EQ_U64(yew_json_len(yew_json_at(array, 2u)), 0u);
    YEW_ASSERT_EQ_I64(yew_json_int(string, 91), 91);
    YEW_ASSERT(yew_json_real(string, 9.25) == 9.25);
    YEW_ASSERT(yew_json_bool(string, true));
    YEW_ASSERT_NULL(yew_json_str(root, &len));
    YEW_ASSERT_EQ_U64(len, 0u);
    YEW_ASSERT(!yew_json_streq(root, "x"));
    YEW_ASSERT(yew_json_streq(string, "x") == false);
    arena_free_all(&a);
}

void test_json_parse_escapes_and_surrogates(void)
{
    static const u8 expected[] = {
        '"', '\\', '/', '\b', '\f', '\n', '\r', '\t', 0xE2, 0x82, 0xAC,
        0xF0, 0x9D, 0x84, 0x9E
    };
    Arena a;
    JsonErr err;
    JsonValue *v;

    arena_init(&a);
    v = parse_text(&a,
                   "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u20ac"
                   "\\uD834\\uDD1E\"",
                   &err);
    YEW_ASSERT_NOT_NULL(v);
    YEW_ASSERT_EQ_U64(v->kind, YEW_JS_STR);
    YEW_ASSERT_EQ_U64(v->s.len, sizeof(expected));
    YEW_ASSERT_EQ_MEM(v->s.p, expected, sizeof(expected));
    arena_free_all(&a);

    arena_init(&a);
    v = parse_text(&a, "\"\\uDCff\"", &err);
    YEW_ASSERT_NOT_NULL(v);
    YEW_ASSERT((v->flags & YEW_JSF_RAW_BYTE) != 0u);
    YEW_ASSERT_EQ_U64(v->s.len, 1u);
    YEW_ASSERT_EQ_U64(v->s.p[0], 0xffu);
    arena_free_all(&a);

    assert_bad("\"\\uD800\"", "lone high surrogate U+D800");
    assert_bad("\"\\uD800\\u0041\"", "lone high surrogate U+D800");
    assert_bad("\"\\uDC00\"", "lone low surrogate U+DC00");
    assert_bad("\"\\uDD00\"", "lone low surrogate U+DD00");
    assert_bad("\"\\u12x4\"", "non-hex digit in Unicode escape");
    assert_bad("\"\\q\"", "unknown escape '\\q'");
}

void test_json_parse_strict_rejections(void)
{
    static const char *bad_numbers[] = {
        "01", "+1", ".5", "1.", "1.e3", "-"
    };
    size_t i;

    for (i = 0u; i < YEW_ARRAY_LEN(bad_numbers); i++)
        assert_bad(bad_numbers[i], "malformed number");
    assert_bad("", "empty document");
    assert_bad("[1,]", "trailing comma in array");
    assert_bad("{\"a\":1,}", "trailing comma in object");
    assert_bad("{'a':1}", "expected string");
    assert_bad("{a:1}", "expected string");
    assert_bad("/*x*/null", "expected JSON value");
    assert_bad("\"a\001b\"", "control byte in string");
    assert_bad("true false", "trailing bytes after JSON value");
    assert_bad("Infinity", "number out of range");
    assert_bad("NaN", "number out of range");
}

void test_json_parse_duplicate_keys_and_depth(void)
{
    Arena a;
    JsonErr err;
    JsonValue *v;
    Bytebuf doc;
    u32 i;

    arena_init(&a);
    v = parse_text(&a, "{\"a\":1,\"b\":2,\"a\":3}", &err);
    YEW_ASSERT_NOT_NULL(v);
    YEW_ASSERT_EQ_U64(v->obj.n, 2u);
    YEW_ASSERT((v->flags & YEW_JSF_DUP_KEY) != 0u);
    YEW_ASSERT_EQ_MEM(v->obj.m[0].key, "a", 1u);
    YEW_ASSERT_EQ_MEM(v->obj.m[1].key, "b", 1u);
    YEW_ASSERT_EQ_I64(yew_json_int(v->obj.m[0].val, -1), 3);
    arena_free_all(&a);

    bytebuf_init(&doc);
    for (i = 0u; i < 127u; i++)
        bytebuf_push_u8(&doc, (u8)'[');
    bytebuf_push_u8(&doc, (u8)'0');
    for (i = 0u; i < 127u; i++)
        bytebuf_push_u8(&doc, (u8)']');
    arena_init(&a);
    YEW_ASSERT_NOT_NULL(yew_json_parse(&a, doc.data, doc.len, &err));
    arena_free_all(&a);
    bytebuf_push_u8(&doc, (u8)' ');
    bytebuf_free(&doc);

    bytebuf_init(&doc);
    for (i = 0u; i < 128u; i++)
        bytebuf_push_u8(&doc, (u8)'[');
    bytebuf_push_u8(&doc, (u8)'0');
    for (i = 0u; i < 128u; i++)
        bytebuf_push_u8(&doc, (u8)']');
    arena_init(&a);
    YEW_ASSERT_NULL(yew_json_parse(&a, doc.data, doc.len, &err));
    YEW_ASSERT_EQ_STR(err.msg, "nesting deeper than 128");
    YEW_ASSERT(err.line == 1u && err.col > 1u);
    arena_free_all(&a);
    bytebuf_free(&doc);

    bytebuf_init(&doc);
    for (i = 0u; i < 127u; i++)
        bytebuf_append(&doc, "{\"a\":", 5u);
    bytebuf_push_u8(&doc, (u8)'0');
    for (i = 0u; i < 127u; i++)
        bytebuf_push_u8(&doc, (u8)'}');
    arena_init(&a);
    YEW_ASSERT_NOT_NULL(yew_json_parse(&a, doc.data, doc.len, &err));
    arena_free_all(&a);
    bytebuf_free(&doc);

    bytebuf_init(&doc);
    for (i = 0u; i < 128u; i++)
        bytebuf_append(&doc, "{\"a\":", 5u);
    bytebuf_push_u8(&doc, (u8)'0');
    for (i = 0u; i < 128u; i++)
        bytebuf_push_u8(&doc, (u8)'}');
    arena_init(&a);
    YEW_ASSERT_NULL(yew_json_parse(&a, doc.data, doc.len, &err));
    YEW_ASSERT_EQ_STR(err.msg, "nesting deeper than 128");
    arena_free_all(&a);
    bytebuf_free(&doc);
}

void test_json_parse_size_and_node_limits(void)
{
    static const u8 unused = 0u;
    Arena a;
    JsonErr err;
    Bytebuf doc;
    size_t i;

    arena_init(&a);
    YEW_ASSERT_NULL(yew_json_parse(&a, &unused,
                                   (u64)YEW_JSON_MAX_BYTES + 1u, &err));
    YEW_ASSERT_EQ_STR(err.msg, "document exceeds 64 MiB");
    YEW_ASSERT_NULL(a.head);
    arena_free_all(&a);

    bytebuf_init(&doc);
    bytebuf_reserve(&doc, (size_t)YEW_JSON_MAX_NODES * 2u + 1u);
    doc.data[0] = (u8)'[';
    for (i = 0u; i < (size_t)YEW_JSON_MAX_NODES; i++) {
        doc.data[i * 2u + 1u] = (u8)'0';
        if (i + 1u < (size_t)YEW_JSON_MAX_NODES)
            doc.data[i * 2u + 2u] = (u8)',';
    }
    doc.data[(size_t)YEW_JSON_MAX_NODES * 2u] = (u8)']';
    doc.len = (size_t)YEW_JSON_MAX_NODES * 2u + 1u;

    arena_init(&a);
    YEW_ASSERT_NULL(yew_json_parse(&a, doc.data, doc.len, &err));
    YEW_ASSERT_EQ_STR(err.msg, "too many JSON nodes");
    YEW_ASSERT_EQ_U64(err.line, 1u);
    YEW_ASSERT(err.col > 1u);
    arena_free_all(&a);
    bytebuf_free(&doc);
}
