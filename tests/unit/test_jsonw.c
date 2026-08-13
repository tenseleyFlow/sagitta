#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mod/lsp/json.h"
#include "util/arena.h"
#include "util/buf.h"

static void assert_buf(const Bytebuf *b, const char *expected)
{
    YEW_ASSERT_EQ_U64(b->len, strlen(expected));
    YEW_ASSERT_EQ_MEM(b->data, expected, b->len);
}

void test_json_writer_scalars_and_escapes(void)
{
    static const u8 input[] = {
        '"', '\\', '/', '\b', '\f', '\n', '\r', '\t', 0x00, 0x01, 0x1f,
        'A', 0xe2, 0x82, 0xac, 0xff
    };
    Bytebuf out;
    JsonW w;

    bytebuf_init(&out);
    yew_jsonw_init(&w, &out);
    yew_jsonw_arr(&w);
    yew_jsonw_str(&w, input, sizeof(input));
    yew_jsonw_int(&w, INT64_MIN);
    yew_jsonw_real(&w, 3.5);
    yew_jsonw_bool(&w, true);
    yew_jsonw_bool(&w, false);
    yew_jsonw_null(&w);
    yew_jsonw_raw(&w, (const u8 *)"{\"raw\":1}", 9u);
    yew_jsonw_arr_end(&w);
    assert_buf(&out,
               "[\"\\\"\\\\/\\b\\f\\n\\r\\t\\u0000\\u0001\\u001f"
               "A€\\uDCff\",-9223372036854775808,3.5,true,false,null,"
               "{\"raw\":1}]");
    bytebuf_free(&out);
}

void test_json_writer_order_and_nested_shape(void)
{
    static const char *keys[] = {
        "k7", "k1", "k9", "k0", "k11", "k3",
        "k8", "k2", "k10", "k6", "k4", "k5"
    };
    Bytebuf out;
    JsonW w;
    u32 i;

    bytebuf_init(&out);
    yew_jsonw_init(&w, &out);
    yew_jsonw_obj(&w);
    for (i = 0u; i < YEW_ARRAY_LEN(keys); i++) {
        yew_jsonw_key(&w, keys[i]);
        if (i == 0u) {
            yew_jsonw_obj(&w);
            yew_jsonw_key(&w, "inside");
            yew_jsonw_arr(&w);
            yew_jsonw_cstr(&w, "x");
            yew_jsonw_arr_end(&w);
            yew_jsonw_obj_end(&w);
        } else {
            yew_jsonw_int(&w, (i64)i);
        }
    }
    yew_jsonw_obj_end(&w);
    assert_buf(&out,
               "{\"k7\":{\"inside\":[\"x\"]},\"k1\":1,\"k9\":2,"
               "\"k0\":3,\"k11\":4,\"k3\":5,\"k8\":6,\"k2\":7,"
               "\"k10\":8,\"k6\":9,\"k4\":10,\"k5\":11}");
    bytebuf_free(&out);
}

void test_json_writer_raw_byte_roundtrip(void)
{
    static const u8 raw[] = {0xff};
    Arena a;
    JsonErr err;
    JsonValue *v;
    Bytebuf out;
    JsonW w;

    bytebuf_init(&out);
    yew_jsonw_init(&w, &out);
    yew_jsonw_str(&w, raw, sizeof(raw));
    assert_buf(&out, "\"\\uDCff\"");
    arena_init(&a);
    v = yew_json_parse(&a, out.data, out.len, &err);
    YEW_ASSERT_NOT_NULL(v);
    YEW_ASSERT_EQ_U64(v->kind, YEW_JS_STR);
    YEW_ASSERT((v->flags & YEW_JSF_RAW_BYTE) != 0u);
    YEW_ASSERT_EQ_U64(v->s.len, 1u);
    YEW_ASSERT_EQ_U64(v->s.p[0], 0xffu);
    arena_free_all(&a);
    bytebuf_free(&out);
}

void test_json_writer_tree_roundtrip(void)
{
    static const char *corpus[] = {
        "null", "true", "false", "0", "-7", "3.25", "\"text\"",
        "[]", "{}", "[1,2,3]", "{\"a\":1}",
        "{\"b\":2,\"a\":1}", "[null,true,false]",
        "{\"nested\":{\"x\":[1,2]}}", "\"\\u20ac\"",
        "\"a\\u0000b\"", "9223372036854775808", "1e-400",
        "{\"escaped\":\"\\n\\t\\\"\"}", "[{},[],{\"z\":0}]"
    };
    size_t i;

    for (i = 0u; i < YEW_ARRAY_LEN(corpus); i++) {
        Arena first;
        Arena second;
        JsonErr err;
        JsonValue *a;
        JsonValue *b;
        Bytebuf out;
        JsonW w;

        arena_init(&first);
        arena_init(&second);
        bytebuf_init(&out);
        a = yew_json_parse(&first, (const u8 *)corpus[i], strlen(corpus[i]),
                           &err);
        YEW_ASSERT_NOT_NULL(a);
        yew_jsonw_init(&w, &out);
        yew_jsonw_value(&w, a);
        b = yew_json_parse(&second, out.data, out.len, &err);
        YEW_ASSERT_NOT_NULL(b);
        YEW_ASSERT(yew_json_eq(a, b));
        arena_free_all(&first);
        arena_free_all(&second);
        bytebuf_free(&out);
    }
}

static int writer_bug_exit(u32 action)
{
    pid_t child;
    pid_t waited;
    int status;

    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        Bytebuf out;
        JsonW w;

        if (freopen("/dev/null", "w", stderr) == NULL)
            _exit(126);
        bytebuf_init(&out);
        yew_jsonw_init(&w, &out);
        yew_jsonw_obj(&w);
        if (action == 0u)
            yew_jsonw_arr_end(&w);
        else if (action == 1u)
            yew_jsonw_int(&w, 1);
        else {
            yew_jsonw_key(&w, "missing");
            yew_jsonw_obj_end(&w);
        }
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_json_writer_structure_bugs(void)
{
    YEW_ASSERT_EQ_I64(writer_bug_exit(0u), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(writer_bug_exit(1u), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(writer_bug_exit(2u), YEW_EXIT_BUG);
}
