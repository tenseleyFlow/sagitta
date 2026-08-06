/*
 * Sprint 25 §2/§4: the Fletch literal codec.
 *
 * The property that matters is a FIXPOINT: emit(parse(emit(x))) is
 * byte-identical to emit(x).  Sprint 36 replaces this implementation
 * with the Fletch VM data path and must land on the same bytes, so
 * anything the emitter does that these tests do not pin is something
 * s36 is free to change and thereby break every user's state file.
 *
 * The other theme is that PATHS ARE BYTES.  A codec that quietly
 * validated UTF-8 would pass every test written with ASCII fixtures and
 * lose exactly the files invariant 2 exists to protect.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"

typedef struct FlFix {
    Arena arena;
    Bytebuf out;
} FlFix;

static void ff_make(FlFix *f)
{
    arena_init(&f->arena);
    bytebuf_init(&f->out);
}

static void ff_free(FlFix *f)
{
    bytebuf_free(&f->out);
    arena_free_all(&f->arena);
}

/* parse -> emit, returning the emitted bytes NUL-terminated. */
static bool ff_round(FlFix *f, const char *src, Bytebuf *out)
{
    FlParseErr err;
    FlLit *lit = sag_fl_parse(&f->arena, (const u8 *)src, strlen(src),
                              &err);
    FlEmit e;

    if (lit == NULL)
        return false;
    out->len = 0U;
    sag_fl_emit_init(&e, out);
    sag_fl_emit_lit(&e, NULL, lit);
    sag_fl_emit_done(&e);
    bytebuf_push_u8(out, 0U);
    out->len--;
    return true;
}

/* ---------------------------------------------------------------- */

/* The canonical layout, pinned byte for byte: two-space indent, one
 * entry per line, `key: value`, trailing comma after EVERY element. */
void test_fl_lit_emits_the_canonical_layout(void)
{
    FlFix f;
    FlEmit e;
    static const char want[] =
        "{\n"
        "  version: 1,\n"
        "  writer: \"sagitta\",\n"
        "  flag: true,\n"
        "  gone: nil,\n"
        "  list: [\n"
        "    1,\n"
        "    2,\n"
        "  ],\n"
        "  inner: {\n"
        "    deep: -7,\n"
        "  },\n"
        "}\n";

    ff_make(&f);
    sag_fl_emit_init(&e, &f.out);
    sag_fl_map_open(&e, NULL);
    sag_fl_int(&e, "version", 1);
    sag_fl_str(&e, "writer", "sagitta", 7U);
    sag_fl_bool(&e, "flag", true);
    sag_fl_nil(&e, "gone");
    sag_fl_list_open(&e, "list");
    sag_fl_int(&e, NULL, 1);
    sag_fl_int(&e, NULL, 2);
    sag_fl_list_close(&e);
    sag_fl_map_open(&e, "inner");
    sag_fl_int(&e, "deep", -7);
    sag_fl_map_close(&e);
    sag_fl_map_close(&e);
    sag_fl_emit_done(&e);
    bytebuf_push_u8(&f.out, 0U);
    SAG_ASSERT_EQ_STR((const char *)f.out.data, want);
    ff_free(&f);
}

/* emit(parse(emit(x))) == emit(x), over a document using every kind. */
void test_fl_lit_round_trip_is_a_fixpoint(void)
{
    FlFix f;
    Bytebuf a;
    Bytebuf b;
    static const char src[] =
        "{\n"
        "  version: 1,\n"
        "  s: \"a\\tb\",\n"
        "  neg: -9223372036854775808,\n"
        "  pos: 9223372036854775807,\n"
        "  t: true,\n"
        "  f: false,\n"
        "  n: nil,\n"
        "  l: [ 1, [ 2, ], { k: 3, }, ],\n"
        "}";

    ff_make(&f);
    bytebuf_init(&a);
    bytebuf_init(&b);
    SAG_ASSERT(ff_round(&f, src, &a));
    /* The canonical form re-parses and re-emits identically. */
    SAG_ASSERT(ff_round(&f, (const char *)a.data, &b));
    SAG_ASSERT_EQ_U64(a.len, b.len);
    SAG_ASSERT_EQ_I64(memcmp(a.data, b.data, a.len), 0);
    /* i64 extremes survive exactly — the reason there are no floats. */
    SAG_ASSERT_NOT_NULL(strstr((const char *)a.data,
                               "-9223372036854775808"));
    SAG_ASSERT_NOT_NULL(strstr((const char *)a.data,
                               "9223372036854775807"));
    bytebuf_free(&a);
    bytebuf_free(&b);
    ff_free(&f);
}

/* Every escape row, in both directions. */
void test_fl_lit_escape_table_round_trips(void)
{
    FlFix f;
    FlEmit e;
    static const char raw[] = {
        '"', '\\', '\n', '\t', '\r', '\0', 0x01, 0x1F, 0x7F, 'z'
    };
    FlParseErr err;
    FlLit *lit;
    const FlLit *v;
    u64 n = 0U;
    const char *s;

    ff_make(&f);
    sag_fl_emit_init(&e, &f.out);
    sag_fl_map_open(&e, NULL);
    sag_fl_str(&e, "x", raw, sizeof(raw));
    sag_fl_map_close(&e);
    sag_fl_emit_done(&e);
    bytebuf_push_u8(&f.out, 0U);

    /* Exactly the pinned spellings, and \x for the rest. */
    SAG_ASSERT_NOT_NULL(strstr((const char *)f.out.data,
                               "\"\\\"\\\\\\n\\t\\r\\0\\x01\\x1f\\x7fz\""));

    lit = sag_fl_parse(&f.arena, f.out.data, f.out.len - 1U, &err);
    SAG_ASSERT_NOT_NULL(lit);
    v = sag_fl_get(lit, "x");
    s = sag_fl_str_or(v, NULL, &n);
    SAG_ASSERT_EQ_U64(n, sizeof(raw));
    SAG_ASSERT_EQ_I64(memcmp(s, raw, sizeof(raw)), 0);
    ff_free(&f);
}

/*
 * Invalid UTF-8 and embedded NULs survive verbatim.
 *
 * A codec that validated here would reject a real path — and the user
 * would lose the record of a file they were working on because its name
 * was not text.
 */
void test_fl_lit_carries_invalid_utf8_and_nul_paths(void)
{
    FlFix f;
    FlEmit e;
    static const u8 path[] = {'/', 't', 0xC3U, 0x28U, '/', 0x00U,
                              0xFFU, 'x'};
    FlParseErr err;
    FlLit *lit;
    u64 n = 0U;
    const char *s;

    ff_make(&f);
    sag_fl_emit_init(&e, &f.out);
    sag_fl_map_open(&e, NULL);
    sag_fl_str(&e, "path", (const char *)path, sizeof(path));
    sag_fl_map_close(&e);
    sag_fl_emit_done(&e);

    /* The high bytes ride through VERBATIM — not \xNN — so a UTF-8
     * path stays readable in the file. */
    SAG_ASSERT_NOT_NULL(memchr(f.out.data, 0xC3, f.out.len));
    SAG_ASSERT_NOT_NULL(memchr(f.out.data, 0xFF, f.out.len));

    lit = sag_fl_parse(&f.arena, f.out.data, f.out.len, &err);
    SAG_ASSERT_NOT_NULL(lit);
    s = sag_fl_str_or(sag_fl_get(lit, "path"), NULL, &n);
    SAG_ASSERT_EQ_U64(n, sizeof(path));
    SAG_ASSERT_EQ_I64(memcmp(s, path, sizeof(path)), 0);
    ff_free(&f);
}

/* Comments are accepted and never re-emitted; the manual says so
 * rather than the parser discovering it for the user. */
void test_fl_lit_accepts_comments_and_drops_them(void)
{
    FlFix f;
    Bytebuf out;
    static const char src[] =
        "# a note the user left\n"
        "{\n"
        "  a: 1, # trailing note\n"
        "  # another\n"
        "  b: 2,\n"
        "}\n";

    ff_make(&f);
    bytebuf_init(&out);
    SAG_ASSERT(ff_round(&f, src, &out));
    SAG_ASSERT_EQ_STR((const char *)out.data, "{\n  a: 1,\n  b: 2,\n}\n");
    SAG_ASSERT(strchr((const char *)out.data, '#') == NULL);
    bytebuf_free(&out);
    ff_free(&f);
}

/*
 * Unknown keys are PRESERVED across parse -> emit.
 *
 * An older sagitta reading a newer one's state must not silently delete
 * the settings it does not understand; the next save would hand the
 * user back a file with their preferences quietly removed.
 */
void test_fl_lit_preserves_unknown_keys_in_order(void)
{
    FlFix f;
    Bytebuf out;
    static const char src[] =
        "{ known: 1, from_the_future: \"x\", also_new: [ 1, ], }";

    ff_make(&f);
    bytebuf_init(&out);
    SAG_ASSERT(ff_round(&f, src, &out));
    SAG_ASSERT_NOT_NULL(strstr((const char *)out.data, "from_the_future"));
    SAG_ASSERT_NOT_NULL(strstr((const char *)out.data, "also_new"));
    /* Insertion order, not hash order — the fixpoint depends on it. */
    SAG_ASSERT(strstr((const char *)out.data, "known") <
               strstr((const char *)out.data, "from_the_future"));
    SAG_ASSERT(strstr((const char *)out.data, "from_the_future") <
               strstr((const char *)out.data, "also_new"));
    bytebuf_free(&out);
    ff_free(&f);
}

/* Quoted keys exist because option names carry dots. */
void test_fl_lit_accepts_quoted_and_dotted_keys(void)
{
    FlFix f;
    FlParseErr err;
    FlLit *lit;
    static const char src[] =
        "{ \"tabs.group_hover_preview\": true, plain.key: 1, }";

    ff_make(&f);
    lit = sag_fl_parse(&f.arena, (const u8 *)src, strlen(src), &err);
    SAG_ASSERT_NOT_NULL(lit);
    SAG_ASSERT(sag_fl_bool_or(sag_fl_get(lit, "tabs.group_hover_preview"),
                              false));
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(lit, "plain.key"), 0), 1);
    ff_free(&f);
}

/* Each cap is corruption, classified — not a silent truncation the
 * reader has to notice later. */
void test_fl_lit_caps_are_corruption(void)
{
    FlFix f;
    FlParseErr err;
    char *big;
    u32 i;

    ff_make(&f);
    /* Depth 33. */
    big = sag_xmalloc(256U);
    for (i = 0U; i < 33U; i++)
        big[i] = '[';
    for (i = 0U; i < 33U; i++)
        big[33U + i] = ']';
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)big, 66U, &err));
    SAG_ASSERT_NOT_NULL(err.msg);
    free(big);

    /* Depth 32 is fine — the cap is a boundary, not an approximation. */
    big = sag_xmalloc(256U);
    for (i = 0U; i < 32U; i++)
        big[i] = '[';
    for (i = 0U; i < 32U; i++)
        big[32U + i] = ']';
    SAG_ASSERT_NOT_NULL(sag_fl_parse(&f.arena, (const u8 *)big, 64U, &err));
    free(big);

    /* A 4097-byte string. */
    {
        u64 n = 4097U + 16U;
        char *s = sag_xmalloc((size_t)n);

        s[0] = '"';
        (void)memset(s + 1, 'a', 4097U);
        s[4098] = '"';
        SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)s, 4099U, &err));
        free(s);
    }
    /* A document over 8 MiB is rejected without being walked. */
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"{}",
                                 (u64)SAG_FL_MAX_BYTES + 1U, &err));
    ff_free(&f);
}

/* Escapes outside the pinned table are rejected: a format with an
 * open-ended escape set cannot be reimplemented compatibly. */
void test_fl_lit_rejects_unknown_escapes(void)
{
    FlFix f;
    FlParseErr err;

    ff_make(&f);
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"\"\\q\"", 4U,
                                 &err));
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"\"\\u0041\"", 8U,
                                 &err));
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"\"\\xZZ\"", 6U,
                                 &err));
    /* And an unterminated one does not read past the end. */
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"\"\\", 2U, &err));
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"\"abc", 4U, &err));
    ff_free(&f);
}

/* Integers out of i64 range are corruption, not a wrapped value. */
void test_fl_lit_rejects_out_of_range_integers(void)
{
    FlFix f;
    FlParseErr err;
    static const char too_big[] = "9223372036854775808";
    static const char too_small[] = "-9223372036854775809";
    static const char way_out[] = "99999999999999999999999";

    ff_make(&f);
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)too_big,
                                 strlen(too_big), &err));
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)too_small,
                                 strlen(too_small), &err));
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)way_out,
                                 strlen(way_out), &err));
    /* The exact bounds parse. */
    SAG_ASSERT_NOT_NULL(sag_fl_parse(&f.arena,
                                     (const u8 *)"-9223372036854775808",
                                     20U, &err));
    ff_free(&f);
}

/* A wrong-typed value takes its default and the document survives — a
 * state file is a cache, and one bad field must not cost a layout. */
void test_fl_lit_readers_are_type_safe(void)
{
    FlFix f;
    FlParseErr err;
    FlLit *lit;
    u64 n = 0U;
    static const char src[] = "{ i: \"not an int\", b: 3, s: true, }";

    ff_make(&f);
    lit = sag_fl_parse(&f.arena, (const u8 *)src, strlen(src), &err);
    SAG_ASSERT_NOT_NULL(lit);
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(lit, "i"), 42), 42);
    SAG_ASSERT(sag_fl_bool_or(sag_fl_get(lit, "b"), true));
    SAG_ASSERT_EQ_STR(sag_fl_str_or(sag_fl_get(lit, "s"), "dflt", &n),
                      "dflt");
    /* A missing key is the same as a wrong-typed one. */
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(lit, "absent"), -1), -1);
    SAG_ASSERT_NULL(sag_fl_get(lit, "absent"));
    ff_free(&f);
}

/* Garbage never crashes and never loops. */
void test_fl_lit_rejects_garbage_totally(void)
{
    FlFix f;
    FlParseErr err;
    static const char *const bad[] = {
        "", "{", "}", "[", "]", "{ a }", "{ a: }", "{ : 1 }",
        "{ a: 1", "[ 1", "tru", "nilx", "-", "--1", "{ a: 1, } trailing",
        "\xff\xfe\xfd", "{ \"unterminated: 1 }"
    };
    size_t i;

    ff_make(&f);
    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        FlLit *lit = sag_fl_parse(&f.arena, (const u8 *)bad[i],
                                  (u64)strlen(bad[i]), &err);

        /* Some of these are legal ("nilx" is not); what matters is that
         * every one returns rather than crashing or spinning. */
        (void)lit;
    }
    /* `nilx` must NOT parse as nil followed by garbage. */
    SAG_ASSERT_NULL(sag_fl_parse(&f.arena, (const u8 *)"nilx", 4U, &err));
    ff_free(&f);
}
