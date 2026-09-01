#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
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

static const SymEntry *find_index_sym(const SymIndex *idx,
                                      const Interner *intern,
                                      const char *name)
{
    size_t i;

    for (i = 0U; i < idx->e.len; i++) {
        const char *candidate = yew_intern_str(intern, idx->e.data[i].name);

        if (candidate != NULL && strcmp(candidate, name) == 0)
            return &idx->e.data[i];
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

void test_symidx_binary_buffers_never_bind_or_schedule_source_work(void)
{
    static const u8 bytes[] = {
        0xcfU, 0xfaU, 0xedU, 0xfeU, 0U, 'p', 'r', 'o', 'g', 'r', 'a', 'm',
        ' ', 'm', 'o', 'd', 'u', 'l', 'e', ' ', 'e', 'n', 'd', '\n'
    };
    Ed ed;
    SymIndex direct;
    SymIndex *stale;
    EditCtx edit;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, bytes, sizeof(bytes),
                                  "extensionless-binary"));
    ed.buffer.path = arena_strdup(&ed.arena, "/tmp/yew-binary-encode");
    ed.buffer.meta.binary = true;
    yew_ed_syn_bind(&ed.buffer);
    YEW_ASSERT_NULL(ed.buffer.lang);
    YEW_ASSERT_EQ_U64(ed.buffer.syn.lang, YEW_LANG_NONE);
    YEW_ASSERT_NULL(ed.buffer.syn.engine);

    /* Defense in depth for a reload that changes classification after an
     * index already exists. */
    ed.buffer.meta.binary = false;
    stale = yew_symidx_buffer(&ed.ws, ed.buffer.id, true);
    YEW_ASSERT_NOT_NULL(stale);
    YEW_ASSERT(yew_symidx_scan(
                   stale, &ed.buffer,
                   (Span){0U, yew_textbuf_len(ed.buffer.tb)}) != 0U);
    ed.buffer.meta.binary = true;
    yew_symidx_pump(&ed, INT64_MAX);
    YEW_ASSERT_EQ_U64(ed.ws.sym_buf.len, 0U);

    yew_symidx_init(&direct, &ed.interner);
    YEW_ASSERT_EQ_U64(yew_symidx_scan(
                          &direct, &ed.buffer,
                          (Span){0U, yew_textbuf_len(ed.buffer.tb)}),
                      0U);
    YEW_ASSERT_EQ_U64(direct.e.len, 0U);
    YEW_ASSERT(!yew_symidx_pending(&ed));
    yew_symidx_pump(&ed, INT64_MAX);
    YEW_ASSERT_EQ_U64(ed.ws.sym_buf.len, 0U);

    /* Memory fixtures have no crash-journal destination; expose the path
     * only while exercising language detection. */
    ed.buffer.path = NULL;
    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(!yew_symidx_pending(&ed));
    YEW_ASSERT_EQ_U64(ed.ws.sym_buf.len, 0U);

    yew_symidx_free(&direct);
    yew_ed_free(&ed);
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

void test_symidx_real_c_highlighter_filters_comments_and_strings(void)
{
    static const u8 text[] =
        "int live_code(void) {\n"
        "    // hidden_comment prose_word\n"
        "    const char *label = \"hidden_string\";\n"
        "    return live_code();\n"
        "}\n";
    Ed ed;
    SymIndex real;
    SymIndex plain;
    size_t real_count;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "real-c"));
    ed.buffer.path = arena_strdup(&ed.arena, "/tmp/yew-symidx-real.c");
    yew_ed_syn_bind(&ed.buffer);
    YEW_ASSERT_NOT_NULL(ed.buffer.lang);
    YEW_ASSERT_EQ_MEM(ed.buffer.lang, "c", 1U);

    yew_symidx_init(&real, &ed.interner);
    (void)yew_symidx_scan(&real, &ed.buffer,
                          (Span){0U, sizeof(text) - 1U});
    real_count = real.e.len;
    YEW_ASSERT(real_count >= 6U);
    YEW_ASSERT_NOT_NULL(find_index_sym(&real, &ed.interner, "live_code"));
    YEW_ASSERT_NOT_NULL(find_index_sym(&real, &ed.interner, "label"));
    YEW_ASSERT_NULL(find_index_sym(&real, &ed.interner, "hidden_comment"));
    YEW_ASSERT_NULL(find_index_sym(&real, &ed.interner, "prose_word"));
    YEW_ASSERT_NULL(find_index_sym(&real, &ed.interner, "hidden_string"));

    yew_syn_detach(&ed.buffer.syn);
    yew_syn_attach(&ed.buffer.syn, YEW_LANG_NONE, ed.buffer.tb);
    yew_symidx_init(&plain, &ed.interner);
    (void)yew_symidx_scan(&plain, &ed.buffer,
                          (Span){0U, sizeof(text) - 1U});
    YEW_ASSERT(plain.e.len > real_count);
    YEW_ASSERT_NOT_NULL(find_index_sym(&plain, &ed.interner,
                                       "hidden_comment"));
    YEW_ASSERT_NOT_NULL(find_index_sym(&plain, &ed.interner,
                                       "prose_word"));
    YEW_ASSERT_NOT_NULL(find_index_sym(&plain, &ed.interner,
                                       "hidden_string"));
    yew_symidx_free(&plain);
    yew_symidx_free(&real);
    yew_ed_free(&ed);
}

void test_symidx_save_replaces_workspace_file_tier(void)
{
    static const u8 before[] = "first_symbol first_symbol\n";
    static const u8 after[] = "second_symbol\n";
    Ed ed;
    EditCtx edit;
    const SymEntry *entry;
    char *path;
    u32 file;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, before, sizeof(before) - 1U,
                                  "workspace-replace"));
    path = arena_strdup(&ed.arena, "/tmp/yew-symidx-workspace.txt");
    ed.buffer.path = path;
    yew_ed_syn_bind(&ed.buffer);
    yew_symidx_workspace_replace(&ed.ws, &ed.buffer);
    entry = find_index_sym(&ed.ws.sym_ws, &ed.interner, "first_symbol");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->buf_id, 0U);
    YEW_ASSERT_EQ_U64(entry->hits, 2U);
    file = entry->file;
    YEW_ASSERT(file != 0U);
    YEW_ASSERT_EQ_U64(yew_symidx_workspace_bytes(&ed.ws),
                      ed.ws.sym_ws.bytes);

    /* The memory fixture has no crash-journal path; expose the canonical
     * path only at the two save-index boundaries. */
    ed.buffer.path = NULL;
    edit = yew_ed_edit_ctx(&ed);
    yew_undo_begin(&edit, YEW_TXN_PASTE);
    YEW_ASSERT(yew_edit_delete(&edit,
                               (Span){0U, yew_textbuf_len(edit.tb)}));
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(0U), after,
                               sizeof(after) - 1U));
    yew_undo_end(&edit);
    yew_ed_finish_edit(&ed, &edit);
    ed.buffer.path = path;
    yew_symidx_workspace_replace(&ed.ws, &ed.buffer);
    YEW_ASSERT_NULL(find_index_sym(&ed.ws.sym_ws, &ed.interner,
                                   "first_symbol"));
    entry = find_index_sym(&ed.ws.sym_ws, &ed.interner, "second_symbol");
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT_EQ_U64(entry->buf_id, 0U);
    YEW_ASSERT_EQ_U64(entry->file, file);
    YEW_ASSERT(yew_symidx_workspace_bytes(&ed.ws) <=
               YEW_SYMIDX_BYTES_MAX);
    yew_ed_free(&ed);
}

void test_symidx_query_is_byte_identical_across_entry_permutations(void)
{
    static const u8 text[] =
        "alpha_name beta_alpha gamma_alpha\n"
        "delta_alpha alpha_helper theta_alpha\n";
    Ed ed;
    SymIndex *idx;
    SymQuery query = {"a", 1U, 0U, {0U}, 16U, true};
    SymHit baseline[16];
    SymHit got[16];
    u32 baseline_len;
    u32 round;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "symidx-order.txt"));
    while (yew_symidx_pending(&ed))
        yew_symidx_pump(&ed, INT64_MAX);
    query.buf_id = ed.buffer.id;
    idx = yew_symidx_buffer(&ed.ws, ed.buffer.id, false);
    YEW_ASSERT_NOT_NULL(idx);
    baseline_len = yew_symidx_query(&ed.ws, &query, baseline,
                                    YEW_ARRAY_LEN(baseline));
    YEW_ASSERT(baseline_len >= 5U);
    for (round = 0U; idx != NULL && round < 8U; round++) {
        SymEntry first_entry;
        u32 first_updated;
        u64 first_sig;
        u32 got_len;

        if (idx->e.len > 1U) {
            first_entry = idx->e.data[0];
            first_updated = idx->updated.data[0];
            first_sig = idx->sig.data[0];
            (void)memmove(&idx->e.data[0], &idx->e.data[1],
                          (idx->e.len - 1U) * sizeof(*idx->e.data));
            (void)memmove(&idx->updated.data[0], &idx->updated.data[1],
                          (idx->updated.len - 1U) *
                              sizeof(*idx->updated.data));
            (void)memmove(&idx->sig.data[0], &idx->sig.data[1],
                          (idx->sig.len - 1U) * sizeof(*idx->sig.data));
            idx->e.data[idx->e.len - 1U] = first_entry;
            idx->updated.data[idx->updated.len - 1U] = first_updated;
            idx->sig.data[idx->sig.len - 1U] = first_sig;
        }
        got_len = yew_symidx_query(&ed.ws, &query, got,
                                   YEW_ARRAY_LEN(got));
        YEW_ASSERT_EQ_U64(got_len, baseline_len);
        YEW_ASSERT_EQ_MEM(got, baseline,
                          baseline_len * sizeof(*baseline));
    }
    yew_ed_free(&ed);
}
