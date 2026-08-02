#include "unit/harness.h"

#include <string.h>

#include "edit/ed.h"
#include "ui/draw.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "util/buf.h"

typedef struct {
    Buffer buffer;
    Win win;
} WrapFixture;

static void wrap_fixture_init(WrapFixture *f, const u8 *text, size_t len,
                              u16 cols)
{
    Cursor c = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    memset(f, 0, sizeof(*f));
    f->buffer.tb = sag_textbuf_from_bytes(text, len);
    f->win.buf = &f->buffer;
    sag_cset_init(&f->win.cs, c);
    sag_vp_init(&f->win);
    f->win.vp.rows = 8U;
    f->win.vp.cols = cols;
    f->win.vp.wrap = true;
}

static void wrap_fixture_free(WrapFixture *f)
{
    sag_vp_free(&f->win);
    sag_cset_free(&f->win.cs);
    sag_textbuf_free(f->buffer.tb);
}

static void assert_row(WrapFixture *f, u32 sub, u64 lo, u64 hi)
{
    Span span = sag_wrap_row(&f->win, LINENO(0U), sub);
    SAG_ASSERT_EQ_U64(span.lo, lo);
    SAG_ASSERT_EQ_U64(span.hi, hi);
}

void test_wrap_prefers_space_runs_and_absorbs_trailing_space(void)
{
    WrapFixture f;
    Ed ed;
    size_t pos;
    const char *text = "ab   cd";
    wrap_fixture_init(&f, (const u8 *)text, strlen(text), 5U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 2U);
    assert_row(&f, 0U, 0U, 5U);
    assert_row(&f, 1U, 5U, 7U);
    wrap_fixture_free(&f);

    wrap_fixture_init(&f, (const u8 *)"ab  c", 5U, 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 2U);
    assert_row(&f, 0U, 0U, 4U);
    assert_row(&f, 1U, 4U, 5U);

    memset(&ed, 0, sizeof(ed));
    arena_init(&ed.arena);
    interner_init(&ed.interner, &ed.arena);
    SAG_ASSERT(sag_grid_init(&ed.grid, &ed.interner, 2U, 3U));
    f.win.rect = (Rect){0U, 0U, 3U, 2U};
    for (pos = 0U; pos <= 5U; pos++) {
        Cursor *cursor = &f.win.cs.curs.data[0];

        cursor->pos = BYTEOFF(pos);
        cursor->anchor = cursor->pos;
        sag_draw_cursor(&ed, &f.win);
        SAG_ASSERT(ed.grid.cur_vis);
        SAG_ASSERT(ed.grid.cur_row < 2U);
        SAG_ASSERT(ed.grid.cur_col < 3U);
    }
    sag_grid_free(&ed.grid);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
    wrap_fixture_free(&f);
}

void test_wrap_hard_breaks_long_token(void)
{
    WrapFixture f;
    const char *text = "abcdefghijkl";
    wrap_fixture_init(&f, (const u8 *)text, strlen(text), 5U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 3U);
    assert_row(&f, 0U, 0U, 5U);
    assert_row(&f, 1U, 5U, 10U);
    assert_row(&f, 2U, 10U, 12U);
    wrap_fixture_free(&f);
}

void test_wrap_breaks_after_hyphen_dash_and_slash(void)
{
    WrapFixture f;
    static const u8 text[] = "ab/cd-ef\xE2\x80\x94gh";
    wrap_fixture_init(&f, text, sizeof(text) - 1U, 4U);
    assert_row(&f, 0U, 0U, 3U);
    assert_row(&f, 1U, 3U, 6U);
    assert_row(&f, 2U, 6U, 11U);
    wrap_fixture_free(&f);
}

void test_wrap_cjk_breaks_without_splitting_clusters(void)
{
    WrapFixture f;
    static const u8 text[] = "a\xE6\xBC\xA2\xE5\xAD\x97z";
    wrap_fixture_init(&f, text, sizeof(text) - 1U, 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 2U);
    assert_row(&f, 0U, 0U, 4U);
    assert_row(&f, 1U, 4U, 8U);
    wrap_fixture_free(&f);
}

void test_wrap_zwj_family_is_atomic_at_boundary(void)
{
    WrapFixture f;
    static const u8 family[] =
        "abc\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7z";
    Span second;
    wrap_fixture_init(&f, family, sizeof(family) - 1U, 4U);
    assert_row(&f, 0U, 0U, 3U);
    second = sag_wrap_row(&f.win, LINENO(0U), 1U);
    SAG_ASSERT_EQ_U64(second.lo, 3U);
    SAG_ASSERT(second.hi > second.lo + 4U);
    wrap_fixture_free(&f);
}

void test_wrap_tab_moves_whole_and_width_one_wide_progresses(void)
{
    WrapFixture f;
    static const u8 tabbed[] = "ab\tx";
    static const u8 wide[] = "\xE6\xBC\xA2\xE5\xAD\x97";

    wrap_fixture_init(&f, tabbed, sizeof(tabbed) - 1U, 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 3U);
    assert_row(&f, 0U, 0U, 2U);
    assert_row(&f, 1U, 2U, 3U);
    assert_row(&f, 2U, 3U, 4U);
    wrap_fixture_free(&f);

    wrap_fixture_init(&f, wide, sizeof(wide) - 1U, 1U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 2U);
    assert_row(&f, 0U, 0U, 3U);
    assert_row(&f, 1U, 3U, 6U);
    wrap_fixture_free(&f);
}

void test_wrap_cache_is_bounded_and_generation_aware(void)
{
    WrapFixture f;
    char text[400];
    size_t at = 0U;
    u32 before;
    unsigned i;

    for (i = 0U; i < 100U; i++) {
        text[at++] = 'a';
        text[at++] = '\n';
    }
    wrap_fixture_init(&f, (const u8 *)text, at, 2U);
    before = sag_wrap_rows(&f.win, LINENO(0U));
    SAG_ASSERT_EQ_U64(before, 1U);
    SAG_ASSERT(f.win.wrap_cache.len <=
               (size_t)f.win.vp.rows + SAG_VP_WRAP_SLACK + 1U);
    sag_textbuf_insert(f.buffer.tb, BYTEOFF(1U), (const u8 *)"xyz", 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 2U);
    SAG_ASSERT_EQ_U64(f.win.wrap_cache.generation, f.buffer.tb->gen);
    sag_vp_invalidate_from(&f.win, LINENO(80U));
    SAG_ASSERT(f.win.wrap_cache.valid);
    sag_vp_invalidate_from(&f.win, LINENO(0U));
    SAG_ASSERT(!f.win.wrap_cache.valid);
    wrap_fixture_free(&f);
}

void test_wrap_warm_edit_matches_cold_recompute(void)
{
    WrapFixture warm;
    WrapFixture cold;
    u32 rows;
    u32 sub;

    wrap_fixture_init(&warm, (const u8 *)"ab cd", 5U, 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&warm.win, LINENO(0U)), 2U);
    (void)sag_wrap_row(&warm.win, LINENO(0U), 0U);
    SAG_ASSERT(warm.win.wrap_cache.spans_valid);

    sag_textbuf_insert(warm.buffer.tb, BYTEOFF(2U), (const u8 *)"  ", 2U);
    wrap_fixture_init(&cold, (const u8 *)"ab   cd", 7U, 3U);
    rows = sag_wrap_rows(&cold.win, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&warm.win, LINENO(0U)), rows);
    for (sub = 0U; sub < rows; sub++) {
        Span warmed = sag_wrap_row(&warm.win, LINENO(0U), sub);
        Span recomputed = sag_wrap_row(&cold.win, LINENO(0U), sub);

        SAG_ASSERT_EQ_U64(warmed.lo, recomputed.lo);
        SAG_ASSERT_EQ_U64(warmed.hi, recomputed.hi);
    }
    SAG_ASSERT(warm.win.wrap_cache.spans_len <=
               (size_t)warm.win.vp.rows + 1U);
    wrap_fixture_free(&cold);
    wrap_fixture_free(&warm);
}

void test_wrap_empty_lines_are_one_row_and_spans_exclude_eol(void)
{
    WrapFixture f;
    Span row;

    wrap_fixture_init(&f, (const u8 *)"\nabc\n", 5U, 3U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(0U)), 1U);
    row = sag_wrap_row(&f.win, LINENO(0U), 0U);
    SAG_ASSERT_EQ_U64(row.lo, 0U);
    SAG_ASSERT_EQ_U64(row.hi, 0U);
    row = sag_wrap_row(&f.win, LINENO(1U), 0U);
    SAG_ASSERT_EQ_U64(row.lo, 1U);
    SAG_ASSERT_EQ_U64(row.hi, 4U);
    SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(2U)), 1U);
    wrap_fixture_free(&f);
}

typedef struct {
    size_t lo;
    size_t hi;
    u32 base_cp;
    u8 cells;
} OracleCluster;

static bool oracle_cjk(u32 cp)
{
    return (cp >= 0x2E80U && cp <= 0xA4CFU) ||
           (cp >= 0xAC00U && cp <= 0xD7A3U) ||
           (cp >= 0xF900U && cp <= 0xFAFFU) ||
           (cp >= 0x20000U && cp <= 0x3FFFFU);
}

static bool oracle_space(u32 cp)
{
    return cp == (u32)' ' || cp == (u32)'\t';
}

static bool oracle_break_after(u32 cp)
{
    return cp == (u32)'-' || cp == (u32)'/' || cp == 0x2013U ||
           cp == 0x2014U;
}

static u32 naive_wrap_rows(const u8 *bytes, size_t len, u16 width,
                           u64 *steps)
{
    OracleCluster clusters[256];
    size_t count = 0U;
    size_t pos = 0U;
    size_t start = 0U;
    u32 rows = 0U;

    while (pos < len) {
        SagCluster cluster;

        SAG_ASSERT(count < SAG_ARRAY_LEN(clusters));
        SAG_ASSERT(sag_cluster_next(bytes, len, &pos, &cluster));
        clusters[count++] = (OracleCluster){cluster.off,
                                            cluster.off + cluster.len,
                                            cluster.base_cp,
                                            cluster.cells};
    }
    if (count == 0U)
        return 1U;
    while (start < count) {
        size_t opportunity = start;
        size_t i;
        CCol used = {0U};
        u64 limit = width == 0U ? 1U : width;
        size_t next_start = count;

        for (i = start; i < count; i++) {
            u32 cells;

            (*steps)++;
            SAG_ASSERT(*steps < UINT64_C(2000000));
            if (oracle_cjk(clusters[i].base_cp) && i > start)
                opportunity = i;
            cells = clusters[i].cells == SAG_CLUSTER_TAB
                        ? sag_tab_cells(used, SAG_VP_TABWIDTH)
                        : clusters[i].cells;
            if (used.v + cells > limit) {
                if (clusters[i].base_cp == (u32)' ') {
                    size_t after = i + 1U;

                    while (after < count &&
                           clusters[after].base_cp == (u32)' ')
                        after++;
                    next_start = after;
                } else {
                    next_start = opportunity > start ? opportunity :
                                 i > start ? i : i + 1U;
                }
                break;
            }
            used.v += cells;
            if (oracle_space(clusters[i].base_cp) ||
                oracle_cjk(clusters[i].base_cp)) {
                opportunity = i + 1U;
            } else if (oracle_break_after(clusters[i].base_cp) &&
                       i + 1U < count &&
                       !oracle_space(clusters[i + 1U].base_cp)) {
                opportunity = i + 1U;
            }
        }
        SAG_ASSERT(next_start > start);
        start = next_start;
        rows++;
    }
    return rows;
}

static u64 wrap_random(u64 *state)
{
    u64 x = *state;

    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    *state = x;
    return x;
}

void test_wrap_randomized_sum_matches_naive_oracle(void)
{
    static const char *const tokens[] = {
        "a", "b", "word", " ", "  ", "\t", "-", "/",
        "\xE6\xBC\xA2", "\xE5\xAD\x97", "\xE2\x80\x93",
        "\xE2\x80\x94",
        ("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
         "\xE2\x80\x8D\xF0\x9F\x91\xA7")
    };
    static const u16 widths[] = {1U, 2U, 3U, 17U, 80U, 200U};
    enum { RANDOM_LINES = 1000 };
    u64 starts[RANDOM_LINES];
    u64 ends[RANDOM_LINES];
    Bytebuf text;
    WrapFixture f;
    u64 state = UINT64_C(0x9e3779b97f4a7c15);
    u64 steps = 0U;
    size_t line;
    size_t wi;

    bytebuf_init(&text);
    for (line = 0U; line < RANDOM_LINES; line++) {
        size_t token_count = 1U + (size_t)(wrap_random(&state) % 48U);
        size_t token;

        starts[line] = text.len;
        if (line % 17U == 0U)
            bytebuf_append(&text, "\xE6\xBC\xA2", 3U);
        for (token = 0U; token < token_count; token++) {
            const char *part = tokens[wrap_random(&state) %
                                      SAG_ARRAY_LEN(tokens)];

            bytebuf_append(&text, part, strlen(part));
        }
        ends[line] = text.len;
        bytebuf_push_u8(&text, (u8)'\n');
    }

    wrap_fixture_init(&f, text.data, text.len, widths[0]);
    for (wi = 0U; wi < SAG_ARRAY_LEN(widths); wi++) {
        f.win.vp.cols = widths[wi];
        for (line = 0U; line < RANDOM_LINES; line++) {
            u32 expected = naive_wrap_rows(text.data + (size_t)starts[line],
                                           (size_t)(ends[line] - starts[line]),
                                           widths[wi], &steps);

            SAG_ASSERT_EQ_U64(sag_wrap_rows(&f.win, LINENO(line)), expected);
        }
    }
    SAG_ASSERT(steps != 0U);
    wrap_fixture_free(&f);
    bytebuf_free(&text);
}
