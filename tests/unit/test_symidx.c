#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "syn_toy.h"
#include "ws/symidx.h"

typedef struct SymFixture {
    Arena arena;
    Interner intern;
    Buffer buf;
    SynToy toy;
    SymIndex idx;
} SymFixture;

static void fixture_init(SymFixture *f, const u8 *bytes, size_t len,
                         bool syntax)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->intern, &f->arena);
    f->buf.id = 7U;
    f->buf.tb = yew_textbuf_from_bytes(bytes, len);
    yew_syn_buf_init(&f->buf.syn);
    if (syntax) {
        syn_toy_init(&f->toy);
        yew_syn_buf_bind(&f->buf.syn, f->toy.engine);
        yew_syn_attach(&f->buf.syn, 1U, f->buf.tb);
    } else {
        yew_syn_attach(&f->buf.syn, YEW_LANG_NONE, f->buf.tb);
    }
    yew_symidx_init(&f->idx, &f->intern);
}

static void fixture_free(SymFixture *f, bool syntax)
{
    yew_symidx_free(&f->idx);
    yew_syn_detach(&f->buf.syn);
    if (syntax)
        syn_toy_free(&f->toy);
    yew_textbuf_free(f->buf.tb);
    interner_free(&f->intern);
    arena_free_all(&f->arena);
}

static const SymEntry *find_sym(const SymFixture *f, const char *name)
{
    size_t i;

    for (i = 0U; i < f->idx.e.len; i++) {
        const char *candidate = yew_intern_str(&f->intern,
                                               f->idx.e.data[i].name);

        if (candidate != NULL && strcmp(candidate, name) == 0)
            return &f->idx.e.data[i];
    }
    return NULL;
}

void test_symidx_filters_syntax_and_infers_kinds(void)
{
    static const u8 text[] =
        "alpha beta // hiddenComment\n"
        "\"hiddenString\" gamma\n"
        "return delta_fn(\n"
        "struct TypeName\n"
        "#define MACRO_NAME value\n"
        "alpha\n";
    SymFixture f;
    const SymEntry *entry;

    fixture_init(&f, text, sizeof(text) - 1U, true);
    YEW_ASSERT(yew_symidx_scan(&f.idx, &f.buf,
                               (Span){0U, sizeof(text) - 1U}) >= 9U);
    YEW_ASSERT_NOT_NULL(find_sym(&f, "alpha"));
    YEW_ASSERT_NOT_NULL(find_sym(&f, "beta"));
    YEW_ASSERT_NOT_NULL(find_sym(&f, "gamma"));
    YEW_ASSERT_NULL(find_sym(&f, "hiddenComment"));
    YEW_ASSERT_NULL(find_sym(&f, "hiddenString"));

    entry = find_sym(&f, "return");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->kind, YEW_SYMK_KEYWORD);
    entry = find_sym(&f, "delta_fn");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->kind, YEW_SYMK_FUNC);
    YEW_ASSERT((entry->flags & YEW_SYMF_DECL) != 0U);
    entry = find_sym(&f, "TypeName");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->kind, YEW_SYMK_TYPE);
    entry = find_sym(&f, "MACRO_NAME");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->kind, YEW_SYMK_MACRO);
    entry = find_sym(&f, "alpha");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->hits, 2U);
    YEW_ASSERT_EQ_U64(entry->off, 0U);
    fixture_free(&f, true);
}

void test_symidx_plain_text_identifier_shape_table(void)
{
    static const u8 text[] =
        "valid_name _ok 123abc 1,000 don't if "
        "\xE6\xBC\xA2\xE5\xAD\x97 "
        "a\xCC\x81" "bc "
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9 ";
    u8 fixture[sizeof(text) + 230U];
    size_t at = 0U;
    SymFixture f;

    (void)memcpy(fixture, text, sizeof(text) - 1U);
    at = sizeof(text) - 1U;
    fixture[at++] = 0xffU;
    fixture[at++] = (u8)'x';
    fixture[at++] = (u8)' ';
    (void)memset(fixture + at, 'z', 200U);
    at += 200U;
    fixture[at++] = (u8)'\n';

    fixture_init(&f, fixture, at, false);
    (void)yew_symidx_scan(&f.idx, &f.buf, (Span){0U, at});
    YEW_ASSERT_NOT_NULL(find_sym(&f, "valid_name"));
    YEW_ASSERT_NOT_NULL(find_sym(&f, "_ok"));
    /* Han is Word_Break Other and each ideograph is its own word unit;
     * neither may be fabricated into a multi-codepoint identifier. */
    YEW_ASSERT_NULL(find_sym(&f, "漢字"));
    YEW_ASSERT_NOT_NULL(find_sym(&f, "ábc"));
    YEW_ASSERT_NULL(find_sym(&f, "123abc"));
    YEW_ASSERT_NULL(find_sym(&f, "1,000"));
    YEW_ASSERT_NULL(find_sym(&f, "don't"));
    YEW_ASSERT_NULL(find_sym(&f, "if"));
    YEW_ASSERT_EQ_U64(f.idx.e.len, 3U);
    fixture_free(&f, false);
}

void test_symidx_plain_text_is_syntax_superset(void)
{
    static const u8 text[] = "alpha // commentWord\n\"stringWord\" omega\n";
    SymFixture syntax;
    SymFixture plain;

    fixture_init(&syntax, text, sizeof(text) - 1U, true);
    fixture_init(&plain, text, sizeof(text) - 1U, false);
    (void)yew_symidx_scan(&syntax.idx, &syntax.buf,
                          (Span){0U, sizeof(text) - 1U});
    (void)yew_symidx_scan(&plain.idx, &plain.buf,
                          (Span){0U, sizeof(text) - 1U});
    YEW_ASSERT(plain.idx.e.len > syntax.idx.e.len);
    YEW_ASSERT_NULL(find_sym(&syntax, "commentWord"));
    YEW_ASSERT_NULL(find_sym(&syntax, "stringWord"));
    YEW_ASSERT_NOT_NULL(find_sym(&plain, "commentWord"));
    YEW_ASSERT_NOT_NULL(find_sym(&plain, "stringWord"));
    YEW_ASSERT_NOT_NULL(find_sym(&syntax, "alpha"));
    YEW_ASSERT_NOT_NULL(find_sym(&syntax, "omega"));
    fixture_free(&plain, false);
    fixture_free(&syntax, true);
}
