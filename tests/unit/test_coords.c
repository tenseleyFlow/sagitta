#include "harness.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text/cursor.h"
#include "text/piece.h"
#include "unicode/coords.h"

static size_t coords_parse_hex(char *field, u8 *out, size_t cap)
{
    char *p = field;
    size_t count = 0U;

    while (*p != '\0') {
        char *end;
        unsigned long value;

        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;
        value = strtoul(p, &end, 16);
        YEW_ASSERT(end != p);
        YEW_ASSERT(value <= 0xFFU);
        YEW_ASSERT(count < cap);
        out[count++] = (u8)value;
        p = end;
    }
    return count;
}

static size_t coords_parse_u64s(char *field, u64 *out, size_t cap)
{
    char *p = field;
    size_t count = 0U;

    while (*p != '\0') {
        char *end;
        unsigned long value = strtoul(p, &end, 10);

        YEW_ASSERT(end != p);
        YEW_ASSERT(count < cap);
        out[count++] = (u64)value;
        p = end;
        if (*p == ',')
            p++;
    }
    return count;
}

void test_coords_motion_golden(void)
{
    FILE *fp = fopen("tests/unit/fixtures/unicode/motion_golden.txt", "r");
    char row[2048];
    size_t cases = 0U;

    YEW_ASSERT_NOT_NULL(fp);
    while (fgets(row, sizeof(row), fp) != NULL) {
        char *fields[4];
        char *p = row;
        u8 bytes[512];
        u64 lengths[128];
        u64 widths[128];
        size_t byte_len;
        size_t cluster_count;
        size_t width_count;
        size_t i;
        u64 off = 0U;
        u64 cells = 0U;
        TextBuf *tb;
        Span line;
        Cursor cursor;

        while (isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;
        fields[0] = p;
        for (i = 1U; i < YEW_ARRAY_LEN(fields); i++) {
            p = strchr(p, '|');
            YEW_ASSERT_NOT_NULL(p);
            *p++ = '\0';
            fields[i] = p;
        }
        p = strchr(fields[3], '\n');
        if (p != NULL)
            *p = '\0';

        byte_len = coords_parse_hex(fields[1], bytes, sizeof(bytes));
        cluster_count = coords_parse_u64s(fields[2], lengths,
                                          YEW_ARRAY_LEN(lengths));
        width_count = coords_parse_u64s(fields[3], widths,
                                        YEW_ARRAY_LEN(widths));
        YEW_ASSERT_EQ_U64(cluster_count, width_count);
        tb = yew_textbuf_from_bytes(bytes, (u64)byte_len);
        YEW_ASSERT_NOT_NULL(tb);
        line = (Span){0U, (u64)byte_len};
        cursor.pos = BYTEOFF(0U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};

        YEW_ASSERT(yew_is_grapheme_boundary(tb, BYTEOFF(0U)));
        for (i = 0U; i < cluster_count; i++) {
            u64 start = off;

            off += lengths[i];
            yew_cursor_right(tb, &cursor);
            YEW_ASSERT_EQ_U64(cursor.pos.v, off);
            YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(start)).v,
                              off);
            YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(off)).v,
                              start);
            YEW_ASSERT(yew_is_grapheme_boundary(tb, BYTEOFF(off)));
            if (lengths[i] > 1U)
                YEW_ASSERT(!yew_is_grapheme_boundary(tb,
                                                     BYTEOFF(start + 1U)));

            if (strcmp(fields[0], "crlf") != 0) {
                YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                                 BYTEOFF(start)).v,
                                  i);
                YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                                                 (GCol){i}).v,
                                  start);
                YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line,
                                                 BYTEOFF(start), 4U).v,
                                  cells);
                YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line,
                                                 (CCol){cells}, 4U).v,
                                  start);
            }
            cells += widths[i];
        }
        YEW_ASSERT_EQ_U64(off, byte_len);
        for (i = cluster_count; i > 0U; i--) {
            off -= lengths[i - 1U];
            yew_cursor_left(tb, &cursor);
            YEW_ASSERT_EQ_U64(cursor.pos.v, off);
        }
        YEW_ASSERT_EQ_U64(off, 0U);
        off = (u64)byte_len;
        if (strcmp(fields[0], "crlf") == 0) {
            YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(2U)).v,
                              0U);
            YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v,
                              0U);
        } else {
            YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(off)).v,
                              cluster_count);
            YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                                             (GCol){cluster_count}).v,
                              off);
            YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(off), 4U).v,
                              cells);
            YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line,
                                             (CCol){cells}, 4U).v,
                              off);
        }
        yew_textbuf_free(tb);
        cases++;
    }
    YEW_ASSERT(!ferror(fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT(cases >= 9U);
}

void test_coords_piece_stream(void)
{
    static const u8 family[] = {
        0xF0, 0x9F, 0x91, 0xA8, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA7, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA6
    };
    TextBuf *tb = yew_textbuf_new();
    size_t i;

    YEW_ASSERT_NOT_NULL(tb);
    for (i = 0U; i < sizeof(family); i++) {
        static const u8 spacer = 'x';
        u64 len = yew_textbuf_len(tb);

        yew_textbuf_insert(tb, BYTEOFF(len), family + i, 1U);
        yew_textbuf_insert(tb, BYTEOFF(len + 1U), &spacer, 1U);
        yew_textbuf_delete(tb, (Span){len + 1U, len + 2U});
    }
    YEW_ASSERT(yew_textbuf_piece_count(tb) >= 3U);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), sizeof(family));
    YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(0U)).v,
                      sizeof(family));
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(sizeof(family))).v,
                      0U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, (Span){0U, sizeof(family)},
                                     BYTEOFF(sizeof(family))).v,
                      1U);
    YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, (Span){0U, sizeof(family)},
                                        BYTEOFF(sizeof(family))).v,
                      7U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, (Span){0U, sizeof(family)},
                                     BYTEOFF(sizeof(family)), 4U).v,
                      2U);
    yew_textbuf_free(tb);
}

void test_coords_tabs_wide_and_invalid(void)
{
    static const u8 bytes[] = {
        'a', '\t', 0xE6, 0xBC, 0xA2, 0xFF, 'b'
    };
    TextBuf *tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    Span line = {0U, sizeof(bytes)};

    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(2U), 2U).v, 2U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v, 4U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(2U), 8U).v, 8U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){5U}, 4U).v, 2U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){6U}, 4U).v, 5U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(6U), 4U).v, 10U);
    YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, line, BYTEOFF(6U)).v, 4U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(6U)).v, 4U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){11U}, 4U).v,
                      sizeof(bytes));
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){12U}, 4U).v, 6U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off_padded(tb, line, (CCol){12U}, 4U).v,
                      sizeof(bytes));
    YEW_ASSERT_EQ_U64(yew_ccol_shortfall((CCol){12U}, (CCol){10U}), 2U);
    YEW_ASSERT_EQ_U64(yew_ccol_max((CCol){12U}, (CCol){10U}).v, 12U);
    YEW_ASSERT_EQ_U64(yew_ccol_max((CCol){10U}, (CCol){12U}).v, 12U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){UINT64_MAX}, 4U).v,
                      6U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){5U}).v,
                      sizeof(bytes));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){6U}).v, 6U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){UINT64_MAX}).v,
                      6U);

    YEW_ASSERT_EQ_U64(yew_ccol_to_off(
                          tb, line,
                          yew_off_to_ccol(tb, line, BYTEOFF(2U), 2U), 2U).v,
                      2U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(
                          tb, line,
                          yew_off_to_ccol(tb, line, BYTEOFF(2U), 4U), 4U).v,
                      2U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(
                          tb, line,
                          yew_off_to_ccol(tb, line, BYTEOFF(2U), 8U), 8U).v,
                      2U);
    yew_textbuf_free(tb);
}

void test_coords_crlf_and_clamping(void)
{
    static const u8 bytes[] = {'a', 'b', '\r', '\n', 'x'};
    TextBuf *tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    Span first;

    YEW_ASSERT_NOT_NULL(tb);
    first = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(first.lo, 0U);
    YEW_ASSERT_EQ_U64(first.hi, 4U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, first, (GCol){UINT64_MAX}).v,
                      2U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, first, (CCol){UINT64_MAX}, 4U).v,
                      2U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, first, BYTEOFF(4U)).v, 2U);
    YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, first, BYTEOFF(4U)).v, 2U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, first, BYTEOFF(4U), 4U).v, 2U);
    YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(2U)).v, 4U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(4U)).v, 2U);
    YEW_ASSERT(!yew_is_grapheme_boundary(tb, BYTEOFF(3U)));
    YEW_ASSERT(yew_is_grapheme_boundary(tb, BYTEOFF(4U)));
    YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(UINT64_MAX)).v,
                      sizeof(bytes));
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(UINT64_MAX)).v, 4U);
    yew_textbuf_free(tb);
}

void test_coords_inside_cluster_rounds_left(void)
{
    static const u8 bytes[] = {'e', 0xCC, 0x81, 'z'};
    TextBuf *tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    Span line = {0U, sizeof(bytes)};

    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(1U)).v, 0U);
    YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, line, BYTEOFF(2U)).v, 1U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v, 0U);
    YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(1U)).v, 3U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(2U)).v, 0U);
    YEW_ASSERT(!yew_is_grapheme_boundary(tb, BYTEOFF(1U)));
    YEW_ASSERT(!yew_is_grapheme_boundary(tb, BYTEOFF(2U)));
    yew_textbuf_free(tb);
}

void test_coords_streams_large_cluster_width(void)
{
    enum { EXTEND_COUNT = 256 * 1024 };
    const u64 len = 1U + (u64)EXTEND_COUNT * 2U;
    u8 *bytes = yew_xmalloc((size_t)len);
    TextBuf *tb;
    Span line = {0U, len};
    size_t i;

    bytes[0] = 'e';
    for (i = 0U; i < EXTEND_COUNT; i++) {
        bytes[1U + i * 2U] = 0xccU;
        bytes[2U + i * 2U] = 0x81U;
    }
    tb = yew_textbuf_from_owned_bytes(bytes, len);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(len)).v, 1U);
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(len), 4U).v, 1U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){1U}, 4U).v, len);
    YEW_ASSERT_EQ_U64(yew_grapheme_next(tb, BYTEOFF(0U)).v, len);
    yew_textbuf_free(tb);
}

static TextBuf *coords_ri_buffer(size_t count)
{
    static const u8 ri[] = {0xF0U, 0x9FU, 0x87U, 0xA6U};
    size_t len = count * sizeof(ri);
    u8 *bytes = yew_xmalloc(len);
    size_t i;

    for (i = 0U; i < count; i++)
        memcpy(bytes + i * sizeof(ri), ri, sizeof(ri));
    return yew_textbuf_from_owned_bytes(bytes, (u64)len);
}

static TextBuf *coords_extend_buffer(size_t count)
{
    static const u8 extend[] = {0xCCU, 0x81U};
    size_t len = count * sizeof(extend);
    u8 *bytes = yew_xmalloc(len);
    size_t i;

    for (i = 0U; i < count; i++)
        memcpy(bytes + i * sizeof(extend), extend, sizeof(extend));
    return yew_textbuf_from_owned_bytes(bytes, (u64)len);
}

void test_coords_reverse_long_context_runs(void)
{
    TextBuf *tb = coords_ri_buffer(65U);

    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(260U)).v,
                      256U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(260U)).v, 256U);
    yew_textbuf_free(tb);

    tb = coords_ri_buffer(129U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(516U)).v,
                      512U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev(tb, BYTEOFF(516U)).v, 512U);
    yew_textbuf_free(tb);

    tb = coords_extend_buffer(65U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(130U)).v,
                      0U);
    YEW_ASSERT(yew_is_grapheme_boundary(tb, BYTEOFF(0U)));
    yew_textbuf_free(tb);

    tb = coords_extend_buffer(129U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(258U)).v,
                      0U);
    YEW_ASSERT(yew_is_grapheme_boundary(tb, BYTEOFF(0U)));
    yew_textbuf_free(tb);
}

void test_coords_sparse_index_edit_invalidation(void)
{
    u8 bytes[130];
    u8 two_lines[261];
    u8 long_lines[1401];
    u8 deferred[700];
    u8 *large_ascii;
    static const u8 accent[] = {0xCCU, 0x81U};
    static const u8 prepend[] = {0xD8U, 0x80U}; /* U+0600, GCB=Prepend */
    static const u8 split[] = {'\n', 'z', 'z'};
    static const u8 x = 'x';
    static const u8 y = 'y';
    static const u8 split_cluster[] = {
        'a', 0xCCU, 0x81U, 0xCCU, 0x81U, 0xCCU, 0x81U
    };
    static const u8 latin2_run[] = {
        0xC3U, 0xA9U, 0xC3U, 0xA9U, 0xC3U, 0xA9U
    };
    TextBuf *tb;
    Span line;
    Cursor cursor;

    memset(bytes, 'a', sizeof(bytes));
    tb = yew_textbuf_from_bytes(bytes, sizeof(bytes));
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.len, 2U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(130U)).v, 130U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){64U}).v, 64U);

    yew_textbuf_insert(tb, BYTEOFF(1U), accent, sizeof(accent));
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){64U}).v, 66U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(132U)).v, 130U);

    yew_textbuf_delete(tb, (Span){1U, 3U});
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){128U}).v, 128U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(130U)).v, 130U);
    yew_textbuf_free(tb);

    memset(two_lines, 'a', 130U);
    two_lines[130U] = '\n';
    memset(two_lines + 131U, 'b', 130U);
    tb = yew_textbuf_from_bytes(two_lines, sizeof(two_lines));
    YEW_ASSERT_EQ_U64(tb->graphemes.len, 4U);
    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){64U}).v, 195U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(261U)).v, 130U);
    yew_textbuf_free(tb);

    memset(long_lines, 'a', 700U);
    long_lines[700U] = '\n';
    memset(long_lines + 701U, 'b', 700U);
    tb = yew_textbuf_from_bytes(long_lines, sizeof(long_lines));
    yew_textbuf_insert(tb, BYTEOFF(650U), split, sizeof(split));
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb), 3U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(650U)).v, 650U);
    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(703U)).v, 52U);
    line = yew_textbuf_line_span(tb, LINENO(2U));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){64U}).v, 768U);

    yew_textbuf_delete(tb, (Span){650U, 653U});
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb), 2U);
    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){64U}).v, 765U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(1401U)).v, 700U);
    yew_textbuf_free(tb);

    memset(deferred, 'a', sizeof(deferred));
    tb = yew_textbuf_from_bytes(deferred, sizeof(deferred));
    yew_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(701U)).v,
                      700U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(701U)).v, 701U);
    yew_textbuf_delete(tb, (Span){10U, 11U});
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    YEW_ASSERT_EQ_U64(yew_grapheme_prev_boundary(tb, BYTEOFF(700U)).v,
                      699U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(700U)).v, 700U);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(deferred, sizeof(deferred));
    yew_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    yew_textbuf_insert(tb, BYTEOFF(20U), &y, 1U);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(702U)).v, 702U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(long_lines, sizeof(long_lines));
    yew_textbuf_insert(tb, BYTEOFF(1000U), &x, 1U);
    yew_textbuf_insert(tb, BYTEOFF(100U), split, sizeof(split));
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb), 3U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(100U)).v, 100U);
    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(703U)).v, 602U);
    line = yew_textbuf_line_span(tb, LINENO(2U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(1405U)).v,
                      701U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){300U}).v, 1004U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(deferred, sizeof(deferred));
    for (u64 edit_at = 10U; edit_at <= 90U; edit_at += 10U) {
        yew_textbuf_insert(tb, BYTEOFF(edit_at), &x, 1U);
        yew_textbuf_check(tb);
    }
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen - 1U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(709U)).v, 709U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 0U);
    yew_textbuf_free(tb);

    large_ascii = yew_xmalloc(64U * 1024U);
    memset(large_ascii, 'a', 64U * 1024U);
    tb = yew_textbuf_from_owned_bytes(large_ascii, 64U * 1024U);
    YEW_ASSERT(tb->graphemes.initialized);
    YEW_ASSERT(tb->graphemes.simple_ascii);
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);
    YEW_ASSERT_EQ_U64(tb->graphemes.len, 0U);
    yew_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    YEW_ASSERT(tb->graphemes.initialized);
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 0U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(64U * 1024U + 1U)).v,
                      64U * 1024U + 1U);
    YEW_ASSERT(tb->graphemes.initialized);
    YEW_ASSERT(tb->graphemes.simple_ascii);
    yew_textbuf_delete(tb, (Span){0U, 2048U});
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);
    yew_textbuf_insert(tb, BYTEOFF(20U), &x, 1U);
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(63490U)).v,
                      63490U);
    yew_textbuf_insert(tb, BYTEOFF(32U * 1024U), accent,
                       sizeof(accent));
    YEW_ASSERT(!tb->graphemes.simple_ascii);
    YEW_ASSERT(!tb->graphemes.initialized);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 0U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(63492U)).v,
                      63490U);
    YEW_ASSERT(!tb->graphemes.initialized);
    yew_textbuf_free(tb);

    large_ascii = yew_xmalloc(64U * 1024U);
    memset(large_ascii, 'a', 64U * 1024U);
    tb = yew_textbuf_from_owned_bytes(large_ascii, 64U * 1024U);
    yew_textbuf_insert(tb, BYTEOFF(32U * 1024U), prepend,
                       sizeof(prepend));
    YEW_ASSERT(!tb->graphemes.initialized);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(32U * 1024U + 1U)).v,
                      32U * 1024U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(32U * 1024U + 2U)).v,
                      32U * 1024U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(32U * 1024U + 3U)).v,
                      32U * 1024U + 1U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(64U * 1024U + 2U)).v,
                      64U * 1024U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                                     (GCol){32U * 1024U}).v,
                      32U * 1024U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                                     (GCol){32U * 1024U + 1U}).v,
                      32U * 1024U + 3U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                                     (GCol){64U * 1024U}).v,
                      64U * 1024U + 2U);
    YEW_ASSERT(!tb->graphemes.initialized);
    yew_textbuf_free(tb);

    large_ascii = yew_xmalloc(64U * 1024U);
    memset(large_ascii, 'a', 64U * 1024U);
    large_ascii[100U] = '\n';
    large_ascii[200U] = '\n';
    tb = yew_textbuf_from_owned_bytes(large_ascii, 64U * 1024U);
    YEW_ASSERT(tb->graphemes.initialized);
    YEW_ASSERT(tb->graphemes.simple_ascii);
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);
    YEW_ASSERT_EQ_U64(tb->graphemes.len, 0U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb), 3U);
    line = yew_textbuf_line_span(tb, LINENO(0U));
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(100U)).v, 100U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(101U)).v, 100U);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){101U}).v, 100U);
    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, BYTEOFF(201U), 4U).v,
                      99U);
    YEW_ASSERT_EQ_U64(yew_ccol_to_off(tb, line, (CCol){100U}, 4U).v,
                      200U);
    yew_textbuf_insert(tb, BYTEOFF(10U), accent, sizeof(accent));
    YEW_ASSERT(!tb->graphemes.simple_ascii);
    YEW_ASSERT(!tb->graphemes.initialized);
    line = yew_textbuf_line_span(tb, LINENO(2U));
    YEW_ASSERT_EQ_U64(line.lo, 203U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, BYTEOFF(1002U)).v,
                      799U);
    YEW_ASSERT(!tb->graphemes.initialized);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(split_cluster, sizeof(split_cluster));
    yew_textbuf_insert(tb, BYTEOFF(3U), &x, 1U);
    cursor.pos = BYTEOFF(sizeof(split_cluster) + 1U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){UINT64_MAX};
    yew_cursor_left(tb, &cursor);
    YEW_ASSERT_EQ_U64(cursor.pos.v, 3U);
    YEW_ASSERT_EQ_U64(cursor.goal_col.v, 1U);
    yew_cursor_left(tb, &cursor);
    YEW_ASSERT_EQ_U64(cursor.pos.v, 0U);
    YEW_ASSERT_EQ_U64(cursor.goal_col.v, 0U);
    yew_textbuf_free(tb);

    tb = yew_textbuf_from_bytes(latin2_run, sizeof(latin2_run));
    yew_textbuf_insert(tb, BYTEOFF(0U), &x, 1U);
    yew_textbuf_insert(tb, BYTEOFF(3U), &y, 1U);
    YEW_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    YEW_ASSERT(tb->graphemes.gen != tb->gen);
    cursor.pos = BYTEOFF(sizeof(latin2_run) + 2U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){UINT64_MAX};
    yew_cursor_left(tb, &cursor);
    YEW_ASSERT_EQ_U64(cursor.pos.v, 6U);
    YEW_ASSERT_EQ_U64(cursor.goal_col.v, 4U);
    yew_cursor_left(tb, &cursor);
    YEW_ASSERT_EQ_U64(cursor.pos.v, 4U);
    YEW_ASSERT_EQ_U64(cursor.goal_col.v, 3U);
    YEW_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    yew_textbuf_free(tb);
}

void test_coords_deferred_index_keeps_line_local_motion_local(void)
{
    const size_t len = 15U * 1024U * 1024U;
    static const u8 accent[] = {0xCCU, 0x81U};
    u8 *bytes = yew_xmalloc(len);
    TextBuf *tb;
    Span line;

    (void)memset(bytes, 'x', len);
    bytes[len - 101U] = '\n';
    tb = yew_textbuf_from_owned_bytes_simple(bytes, len, true);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT(tb->graphemes.initialized);
    YEW_ASSERT(tb->graphemes.simple_ascii_direct);

    yew_textbuf_insert(tb, BYTEOFF(0U), accent, sizeof(accent));
    YEW_ASSERT(!tb->graphemes.initialized);

    line = yew_textbuf_line_span(tb, LINENO(1U));
    YEW_ASSERT_EQ_U64(line.lo, len - 98U);
    YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line,
                                     BYTEOFF(line.lo + 40U)).v,
                      40U);
    YEW_ASSERT(!tb->graphemes.initialized);
    YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line, (GCol){25U}).v,
                      line.lo + 25U);
    YEW_ASSERT(!tb->graphemes.initialized);
    yew_textbuf_free(tb);
}
