#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"

#include "syn_toy.h"

static u64 diff_rand(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void diff_settle_all(SynBuf *syn, const TextBuf *tb)
{
    SynSettleReport report;
    u32 calls = 0U;

    do {
        yew_syn_settle(syn, tb, LINENO(0U),
                       LINENO(yew_textbuf_line_count(tb)), INT64_MAX,
                       &report);
        YEW_ASSERT(++calls < 1000U);
    } while (!report.fixpoint);
}

static void assert_line_equal(const SynLineOut *a, const SynLineOut *b)
{
    u32 i;

    YEW_ASSERT_EQ_U64(a->stop, b->stop);
    YEW_ASSERT_EQ_U64(a->exit_state, b->exit_state);
    YEW_ASSERT_EQ_U64(a->n, b->n);
    for (i = 0U; i < a->n; i++) {
        YEW_ASSERT_EQ_U64(a->spans[i].start, b->spans[i].start);
        YEW_ASSERT_EQ_U64(a->spans[i].len, b->spans[i].len);
        YEW_ASSERT_EQ_U64(a->spans[i].attr, b->spans[i].attr);
        YEW_ASSERT_EQ_U64(a->spans[i].flags, b->spans[i].flags);
    }
}

static bool diff_has_pending_embed(const SynBuf *syn, SynEngine *engine)
{
    size_t i;

    for (i = 0U; i < syn->entry.len; i++) {
        const SynState *state =
            yew_syn_state_get(yew_syn_engine_states(engine),
                              syn->entry.data[i]);

        if (state != NULL &&
            (state->flags & YEW_SYN_F_EMBED_PEND) != 0U)
            return true;
    }
    return false;
}

static void diff_settle_embeds(SynBuf *syn, SynEngine *engine,
                               const TextBuf *tb)
{
    SynSettleReport report;
    u32 calls = 0U;

    do {
        yew_syn_settle(syn, tb, LINENO(0U),
                       LINENO(yew_textbuf_line_count(tb)), INT64_MAX,
                       &report);
        YEW_ASSERT(++calls <= YEW_SYN_DEF_MAX + 2U);
    } while (!report.fixpoint || diff_has_pending_embed(syn, engine));
}

static void assert_buf_matches_from_scratch(SynBuf *got, SynEngine *engine,
                                            const TextBuf *tb,
                                            const u8 *flat, size_t len)
{
    u32 state = YEW_SYN_STATE_ROOT;
    size_t lo = 0U;
    u64 line_count = yew_textbuf_line_count(tb);
    u64 line;

    YEW_ASSERT_EQ_U64(got->entry.len, line_count);
    for (line = 0U; line < line_count; line++) {
        size_t hi = lo;
        SynSpan got_spans[128];
        SynSpan want_spans[128];
        SynLineOut got_out = {got_spans, 0U, YEW_ARRAY_LEN(got_spans),
                              0U, 0U};
        SynLineOut want_out = {want_spans, 0U,
                               YEW_ARRAY_LEN(want_spans), 0U, 0U};

        while (hi < len && flat[hi] != (u8)'\n')
            hi++;
        YEW_ASSERT_EQ_U64(got->entry.data[line], state);
        yew_syn_line(engine, state, flat + lo, (u32)(hi - lo), &want_out);
        yew_syn_spans(got, tb, LINENO(line), &got_out);
        assert_line_equal(&got_out, &want_out);
        state = want_out.exit_state;
        lo = hi < len ? hi + 1U : hi;
    }
}

typedef struct EmbedBoundaryEdit {
    const char *marker;
    u32 within;
    u8 replacement;
} EmbedBoundaryEdit;

typedef struct EmbedDiffSpec {
    const char *path;
    const char *initial;
    const EmbedBoundaryEdit *boundary;
    u32 nboundary;
    const EmbedBoundaryEdit *body;
    u32 nbody;
} EmbedDiffSpec;

typedef struct EmbedDiffDoc {
    size_t len;
    u8 *flat;
    TextBuf *tb;
    SynEngine *engine;
    SynBuf syn;
    size_t boundary[8];
    u8 boundary_original[8];
    size_t body[8];
    u8 body_original[8];
    const EmbedDiffSpec *spec;
} EmbedDiffDoc;

static void embed_diff_resolve_points(const u8 *flat,
                                      const EmbedBoundaryEdit *points,
                                      u32 npoints, size_t offsets[8],
                                      u8 original[8])
{
    u32 i;

    YEW_ASSERT(npoints <= 8U);
    for (i = 0U; i < npoints; i++) {
        const char *found = strstr((const char *)flat, points[i].marker);

        YEW_ASSERT_NOT_NULL(found);
        offsets[i] = (size_t)(found - (const char *)flat) + points[i].within;
        original[i] = flat[offsets[i]];
        YEW_ASSERT(original[i] != (u8)'\n');
        YEW_ASSERT(original[i] != points[i].replacement);
    }
}

static void embed_diff_open(EmbedDiffDoc *doc, const EmbedDiffSpec *spec)
{
    u32 lang;

    (void)memset(doc, 0, sizeof(*doc));
    doc->spec = spec;
    doc->len = strlen(spec->initial);
    doc->flat = malloc(doc->len + 1U);
    YEW_ASSERT_NOT_NULL(doc->flat);
    (void)memcpy(doc->flat, spec->initial, doc->len + 1U);
    embed_diff_resolve_points(doc->flat, spec->boundary, spec->nboundary,
                              doc->boundary, doc->boundary_original);
    embed_diff_resolve_points(doc->flat, spec->body, spec->nbody,
                              doc->body, doc->body_original);
    doc->tb = yew_textbuf_from_bytes(doc->flat, doc->len);
    lang = yew_syn_lang_for(spec->path, doc->flat,
                            (u32)strcspn(spec->initial, "\n"));
    YEW_ASSERT(lang != YEW_LANG_NONE);
    doc->engine = yew_syn_engine_for(lang);
    YEW_ASSERT_NOT_NULL(doc->engine);
    yew_syn_buf_init(&doc->syn);
    yew_syn_attach(&doc->syn, lang, doc->tb);
    yew_syn_buf_bind(&doc->syn, doc->engine);
    diff_settle_embeds(&doc->syn, doc->engine, doc->tb);
    assert_buf_matches_from_scratch(&doc->syn, doc->engine, doc->tb,
                                    doc->flat, doc->len);
}

static void embed_diff_close(EmbedDiffDoc *doc)
{
    yew_syn_detach(&doc->syn);
    yew_textbuf_free(doc->tb);
    free(doc->flat);
}

static void embed_diff_edit(EmbedDiffDoc *doc, bool boundary, u64 *seed)
{
    const EmbedBoundaryEdit *points = boundary ? doc->spec->boundary :
                                                 doc->spec->body;
    u32 npoints = boundary ? doc->spec->nboundary : doc->spec->nbody;
    size_t *offsets = boundary ? doc->boundary : doc->body;
    u8 *original = boundary ? doc->boundary_original : doc->body_original;
    u32 point = (u32)(diff_rand(seed) % npoints);
    size_t at = offsets[point];
    u8 byte = doc->flat[at] == original[point]
                  ? points[point].replacement : original[point];
    LineNo line = yew_textbuf_line_of(doc->tb, BYTEOFF(at));

    yew_textbuf_delete(doc->tb, (Span){at, at + 1U});
    yew_textbuf_insert(doc->tb, BYTEOFF(at), &byte, 1U);
    doc->flat[at] = byte;
    yew_syn_edit(&doc->syn, line, 0U, 0U);
    diff_settle_embeds(&doc->syn, doc->engine, doc->tb);
    assert_buf_matches_from_scratch(&doc->syn, doc->engine, doc->tb,
                                    doc->flat, doc->len);
}

static void run_embed_diff_seed(u64 seed, u32 edits)
{
    static const char markdown[] =
        "# embedded boundary differential\n"
        "```javascript\n"
        "const answer = 42;\n"
        "```\n"
        "tail\n";
    static const EmbedBoundaryEdit markdown_boundary[] = {
        {"```javascript", 0U, (u8)'~'},
        {"```javascript", 3U, (u8)'x'},
        {"```\ntail", 0U, (u8)'~'}
    };
    static const EmbedBoundaryEdit markdown_body[] = {
        {"embedded", 0U, (u8)'E'},
        {"answer", 0U, (u8)'A'},
        {"tail", 0U, (u8)'T'}
    };
    static const char html[] =
        "<script>\n"
        "const answer = 42;\n"
        "</script>\n"
        "<style>\n"
        ".answer { color: red; }\n"
        "</style>\n";
    static const EmbedBoundaryEdit html_boundary[] = {
        {"<script>", 1U, (u8)'x'},
        {"</script>", 2U, (u8)'x'},
        {"<style>", 1U, (u8)'x'},
        {"</style>", 2U, (u8)'x'}
    };
    static const EmbedBoundaryEdit html_body[] = {
        {"answer", 0U, (u8)'A'},
        {"42", 0U, (u8)'5'},
        {"color", 0U, (u8)'C'}
    };
    static const char shell[] =
        "header plain\n"
        "before=$(printf '%s' value)\n"
        "middle plain\n"
        "after=$((1 + 2))\n"
        "tail plain\n";
    static const EmbedBoundaryEdit shell_boundary[] = {
        {"$(printf", 0U, (u8)'x'},
        {"value)", 5U, (u8)'x'},
        {"$((1", 0U, (u8)'x'},
        {"2))", 2U, (u8)'x'}
    };
    static const EmbedBoundaryEdit shell_body[] = {
        {"header", 0U, (u8)'H'},
        {"middle", 0U, (u8)'M'},
        {"tail", 0U, (u8)'T'}
    };
    static const EmbedDiffSpec specs[] = {
        {"fixture.md", markdown, markdown_boundary,
         YEW_ARRAY_LEN(markdown_boundary), markdown_body,
         YEW_ARRAY_LEN(markdown_body)},
        {"fixture.html", html, html_boundary,
         YEW_ARRAY_LEN(html_boundary), html_body,
         YEW_ARRAY_LEN(html_body)},
        {"fixture.sh", shell, shell_boundary,
         YEW_ARRAY_LEN(shell_boundary), shell_body,
         YEW_ARRAY_LEN(shell_body)}
    };
    EmbedDiffDoc docs[YEW_ARRAY_LEN(specs)];
    u32 boundary_edits = 0U;
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(specs); i++)
        embed_diff_open(&docs[i], &specs[i]);

    for (i = 0U; i < edits; i++) {
        bool boundary = (i & 3U) == 0U;
        u32 doc = (u32)(diff_rand(&seed) % YEW_ARRAY_LEN(docs));

        if (boundary)
            boundary_edits++;
        embed_diff_edit(&docs[doc], boundary, &seed);
    }
    YEW_ASSERT_EQ_U64(i, edits);
    YEW_ASSERT(boundary_edits * 4U >= edits);

    for (i = 0U; i < YEW_ARRAY_LEN(docs); i++)
        embed_diff_close(&docs[i]);
}

static void run_diff_seed(u64 seed, u32 edits)
{
    static const char initial[] =
        "if value = 10\n"
        "plain text\n"
        "/* block\n"
        "comment */ return\n"
        "\"str\\n\" + false\n"
        "// final comment\n"
        "while x = 99\n"
        "tail\n";
    static const u8 replacements[] =
        "abcdefghijklmnopqrstuvwxyz0123456789/*\"\\=+- ";
    size_t len = sizeof(initial) - 1U;
    u8 *flat = malloc(len);
    TextBuf *tb;
    SynToy toy;
    SynBuf syn;
    u32 i;

    YEW_ASSERT_NOT_NULL(flat);
    (void)memcpy(flat, initial, len);
    tb = yew_textbuf_from_bytes(flat, len);
    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    diff_settle_all(&syn, tb);

    for (i = 0U; i < edits; i++) {
        size_t at;
        u8 byte;
        LineNo line;

        do {
            at = (size_t)(diff_rand(&seed) % len);
        } while (flat[at] == (u8)'\n');
        byte = replacements[diff_rand(&seed) %
                            (sizeof(replacements) - 1U)];
        line = yew_textbuf_line_of(tb, BYTEOFF(at));
        yew_textbuf_delete(tb, (Span){at, at + 1U});
        yew_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
        flat[at] = byte;
        yew_syn_edit(&syn, line, 0U, 0U);
        diff_settle_all(&syn, tb);
        YEW_ASSERT_EQ_U64(syn.entry.len, yew_textbuf_line_count(tb));
        YEW_ASSERT_EQ_U64(syn.entry.data[0], YEW_SYN_STATE_ROOT);
    }

    {
        u32 state = YEW_SYN_STATE_ROOT;
        u64 lo = 0U;
        u64 line_count = yew_textbuf_line_count(tb);
        u64 line;

        for (line = 0U; line < line_count; line++) {
            u64 hi = lo;
            SynSpan want_spans[128];
            SynSpan got_spans[128];
            SynLineOut want = {want_spans, 0U, YEW_ARRAY_LEN(want_spans),
                               0U, 0U};
            SynLineOut got = {got_spans, 0U, YEW_ARRAY_LEN(got_spans),
                              0U, 0U};

            while (hi < len && flat[hi] != (u8)'\n')
                hi++;
            YEW_ASSERT_EQ_U64(syn.entry.data[line], state);
            yew_syn_line(toy.engine, state, flat + lo, (u32)(hi - lo),
                         &want);
            yew_syn_spans(&syn, tb, LINENO(line), &got);
            assert_line_equal(&got, &want);
            state = want.exit_state;
            lo = hi < len ? hi + 1U : hi;
        }
    }

    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
    free(flat);
}

void test_syn_diff_incremental_matches_from_scratch_four_seeds(void)
{
    static const u64 seeds[] = {
        UINT64_C(0x123456789abcdef0),
        UINT64_C(0x0ddc0ffeebadf00d),
        UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0xfeedfacecafebeef)
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(seeds); i++) {
        run_diff_seed(seeds[i], 100000U);
        run_embed_diff_seed(seeds[i], 100000U);
    }
}

void test_syn_diff_same_seed_is_deterministic(void)
{
    /* Running the complete oracle twice also catches accidental dependence
     * on allocation addresses or process-global state. */
    run_diff_seed(UINT64_C(0x5eed5eed5eed5eed), 1000U);
    run_diff_seed(UINT64_C(0x5eed5eed5eed5eed), 1000U);
    YEW_ASSERT(true);
}
