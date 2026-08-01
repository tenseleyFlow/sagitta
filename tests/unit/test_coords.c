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
        SAG_ASSERT(end != p);
        SAG_ASSERT(value <= 0xFFU);
        SAG_ASSERT(count < cap);
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

        SAG_ASSERT(end != p);
        SAG_ASSERT(count < cap);
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

    SAG_ASSERT_NOT_NULL(fp);
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
        for (i = 1U; i < SAG_ARRAY_LEN(fields); i++) {
            p = strchr(p, '|');
            SAG_ASSERT_NOT_NULL(p);
            *p++ = '\0';
            fields[i] = p;
        }
        p = strchr(fields[3], '\n');
        if (p != NULL)
            *p = '\0';

        byte_len = coords_parse_hex(fields[1], bytes, sizeof(bytes));
        cluster_count = coords_parse_u64s(fields[2], lengths,
                                          SAG_ARRAY_LEN(lengths));
        width_count = coords_parse_u64s(fields[3], widths,
                                        SAG_ARRAY_LEN(widths));
        SAG_ASSERT_EQ_U64(cluster_count, width_count);
        tb = sag_textbuf_from_bytes(bytes, (u64)byte_len);
        SAG_ASSERT_NOT_NULL(tb);
        line = (Span){0U, (u64)byte_len};
        cursor.pos = BYTEOFF(0U);
        cursor.anchor = cursor.pos;
        cursor.goal_col = (GCol){0U};

        SAG_ASSERT(sag_is_grapheme_boundary(tb, BYTEOFF(0U)));
        for (i = 0U; i < cluster_count; i++) {
            u64 start = off;

            off += lengths[i];
            sag_cursor_right(tb, &cursor);
            SAG_ASSERT_EQ_U64(cursor.pos.v, off);
            SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(start)).v,
                              off);
            SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(off)).v,
                              start);
            SAG_ASSERT(sag_is_grapheme_boundary(tb, BYTEOFF(off)));
            if (lengths[i] > 1U)
                SAG_ASSERT(!sag_is_grapheme_boundary(tb,
                                                     BYTEOFF(start + 1U)));

            if (strcmp(fields[0], "crlf") != 0) {
                SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line,
                                                 BYTEOFF(start)).v,
                                  i);
                SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line,
                                                 (GCol){i}).v,
                                  start);
                SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line,
                                                 BYTEOFF(start), 4U).v,
                                  cells);
                SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line,
                                                 (CCol){cells}, 4U).v,
                                  start);
            }
            cells += widths[i];
        }
        SAG_ASSERT_EQ_U64(off, byte_len);
        for (i = cluster_count; i > 0U; i--) {
            off -= lengths[i - 1U];
            sag_cursor_left(tb, &cursor);
            SAG_ASSERT_EQ_U64(cursor.pos.v, off);
        }
        SAG_ASSERT_EQ_U64(off, 0U);
        off = (u64)byte_len;
        if (strcmp(fields[0], "crlf") == 0) {
            SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(2U)).v,
                              0U);
            SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v,
                              0U);
        } else {
            SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(off)).v,
                              cluster_count);
            SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line,
                                             (GCol){cluster_count}).v,
                              off);
            SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(off), 4U).v,
                              cells);
            SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line,
                                             (CCol){cells}, 4U).v,
                              off);
        }
        sag_textbuf_free(tb);
        cases++;
    }
    SAG_ASSERT(!ferror(fp));
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT(cases >= 9U);
}

void test_coords_piece_stream(void)
{
    static const u8 family[] = {
        0xF0, 0x9F, 0x91, 0xA8, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA7, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA6
    };
    TextBuf *tb = sag_textbuf_new();
    size_t i;

    SAG_ASSERT_NOT_NULL(tb);
    for (i = 0U; i < sizeof(family); i++) {
        static const u8 spacer = 'x';
        u64 len = sag_textbuf_len(tb);

        sag_textbuf_insert(tb, BYTEOFF(len), family + i, 1U);
        sag_textbuf_insert(tb, BYTEOFF(len + 1U), &spacer, 1U);
        sag_textbuf_delete(tb, (Span){len + 1U, len + 2U});
    }
    SAG_ASSERT(sag_textbuf_piece_count(tb) >= 3U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), sizeof(family));
    SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(0U)).v,
                      sizeof(family));
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(sizeof(family))).v,
                      0U);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, (Span){0U, sizeof(family)},
                                     BYTEOFF(sizeof(family))).v,
                      1U);
    SAG_ASSERT_EQ_U64(sag_off_to_charcol(tb, (Span){0U, sizeof(family)},
                                        BYTEOFF(sizeof(family))).v,
                      7U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, (Span){0U, sizeof(family)},
                                     BYTEOFF(sizeof(family)), 4U).v,
                      2U);
    sag_textbuf_free(tb);
}

void test_coords_tabs_wide_and_invalid(void)
{
    static const u8 bytes[] = {
        'a', '\t', 0xE6, 0xBC, 0xA2, 0xFF, 'b'
    };
    TextBuf *tb = sag_textbuf_from_bytes(bytes, sizeof(bytes));
    Span line = {0U, sizeof(bytes)};

    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(2U), 2U).v, 2U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v, 4U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(2U), 8U).v, 8U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){5U}, 4U).v, 2U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){6U}, 4U).v, 5U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(6U), 4U).v, 10U);
    SAG_ASSERT_EQ_U64(sag_off_to_charcol(tb, line, BYTEOFF(6U)).v, 4U);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(6U)).v, 4U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){11U}, 4U).v,
                      sizeof(bytes));
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){12U}, 4U).v, 6U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){UINT64_MAX}, 4U).v,
                      6U);
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){5U}).v,
                      sizeof(bytes));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){6U}).v, 6U);
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){UINT64_MAX}).v,
                      6U);

    SAG_ASSERT_EQ_U64(sag_ccol_to_off(
                          tb, line,
                          sag_off_to_ccol(tb, line, BYTEOFF(2U), 2U), 2U).v,
                      2U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(
                          tb, line,
                          sag_off_to_ccol(tb, line, BYTEOFF(2U), 4U), 4U).v,
                      2U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(
                          tb, line,
                          sag_off_to_ccol(tb, line, BYTEOFF(2U), 8U), 8U).v,
                      2U);
    sag_textbuf_free(tb);
}

void test_coords_crlf_and_clamping(void)
{
    static const u8 bytes[] = {'a', 'b', '\r', '\n', 'x'};
    TextBuf *tb = sag_textbuf_from_bytes(bytes, sizeof(bytes));
    Span first;

    SAG_ASSERT_NOT_NULL(tb);
    first = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(first.lo, 0U);
    SAG_ASSERT_EQ_U64(first.hi, 4U);
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, first, (GCol){UINT64_MAX}).v,
                      2U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, first, (CCol){UINT64_MAX}, 4U).v,
                      2U);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, first, BYTEOFF(4U)).v, 2U);
    SAG_ASSERT_EQ_U64(sag_off_to_charcol(tb, first, BYTEOFF(4U)).v, 2U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, first, BYTEOFF(4U), 4U).v, 2U);
    SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(2U)).v, 4U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(4U)).v, 2U);
    SAG_ASSERT(!sag_is_grapheme_boundary(tb, BYTEOFF(3U)));
    SAG_ASSERT(sag_is_grapheme_boundary(tb, BYTEOFF(4U)));
    SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(UINT64_MAX)).v,
                      sizeof(bytes));
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(UINT64_MAX)).v, 4U);
    sag_textbuf_free(tb);
}

void test_coords_inside_cluster_rounds_left(void)
{
    static const u8 bytes[] = {'e', 0xCC, 0x81, 'z'};
    TextBuf *tb = sag_textbuf_from_bytes(bytes, sizeof(bytes));
    Span line = {0U, sizeof(bytes)};

    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(1U)).v, 0U);
    SAG_ASSERT_EQ_U64(sag_off_to_charcol(tb, line, BYTEOFF(2U)).v, 1U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(2U), 4U).v, 0U);
    SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(1U)).v, 3U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(2U)).v, 0U);
    SAG_ASSERT(!sag_is_grapheme_boundary(tb, BYTEOFF(1U)));
    SAG_ASSERT(!sag_is_grapheme_boundary(tb, BYTEOFF(2U)));
    sag_textbuf_free(tb);
}

void test_coords_streams_large_cluster_width(void)
{
    enum { EXTEND_COUNT = 256 * 1024 };
    const u64 len = 1U + (u64)EXTEND_COUNT * 2U;
    u8 *bytes = sag_xmalloc((size_t)len);
    TextBuf *tb;
    Span line = {0U, len};
    size_t i;

    bytes[0] = 'e';
    for (i = 0U; i < EXTEND_COUNT; i++) {
        bytes[1U + i * 2U] = 0xccU;
        bytes[2U + i * 2U] = 0x81U;
    }
    tb = sag_textbuf_from_owned_bytes(bytes, len);
    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(len)).v, 1U);
    SAG_ASSERT_EQ_U64(sag_off_to_ccol(tb, line, BYTEOFF(len), 4U).v, 1U);
    SAG_ASSERT_EQ_U64(sag_ccol_to_off(tb, line, (CCol){1U}, 4U).v, len);
    SAG_ASSERT_EQ_U64(sag_grapheme_next(tb, BYTEOFF(0U)).v, len);
    sag_textbuf_free(tb);
}

static TextBuf *coords_ri_buffer(size_t count)
{
    static const u8 ri[] = {0xF0U, 0x9FU, 0x87U, 0xA6U};
    size_t len = count * sizeof(ri);
    u8 *bytes = sag_xmalloc(len);
    size_t i;

    for (i = 0U; i < count; i++)
        memcpy(bytes + i * sizeof(ri), ri, sizeof(ri));
    return sag_textbuf_from_owned_bytes(bytes, (u64)len);
}

static TextBuf *coords_extend_buffer(size_t count)
{
    static const u8 extend[] = {0xCCU, 0x81U};
    size_t len = count * sizeof(extend);
    u8 *bytes = sag_xmalloc(len);
    size_t i;

    for (i = 0U; i < count; i++)
        memcpy(bytes + i * sizeof(extend), extend, sizeof(extend));
    return sag_textbuf_from_owned_bytes(bytes, (u64)len);
}

void test_coords_reverse_long_context_runs(void)
{
    TextBuf *tb = coords_ri_buffer(65U);

    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(260U)).v,
                      256U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(260U)).v, 256U);
    sag_textbuf_free(tb);

    tb = coords_ri_buffer(129U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(516U)).v,
                      512U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev(tb, BYTEOFF(516U)).v, 512U);
    sag_textbuf_free(tb);

    tb = coords_extend_buffer(65U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(130U)).v,
                      0U);
    SAG_ASSERT(sag_is_grapheme_boundary(tb, BYTEOFF(0U)));
    sag_textbuf_free(tb);

    tb = coords_extend_buffer(129U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(258U)).v,
                      0U);
    SAG_ASSERT(sag_is_grapheme_boundary(tb, BYTEOFF(0U)));
    sag_textbuf_free(tb);
}

void test_coords_sparse_index_edit_invalidation(void)
{
    u8 bytes[130];
    u8 two_lines[261];
    u8 long_lines[1401];
    u8 deferred[700];
    static const u8 accent[] = {0xCCU, 0x81U};
    static const u8 split[] = {'\n', 'z', 'z'};
    static const u8 x = 'x';
    static const u8 y = 'y';
    static const u8 z = 'z';
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
    tb = sag_textbuf_from_bytes(bytes, sizeof(bytes));
    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    SAG_ASSERT_EQ_U64(tb->graphemes.len, 2U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(130U)).v, 130U);
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){64U}).v, 64U);

    sag_textbuf_insert(tb, BYTEOFF(1U), accent, sizeof(accent));
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){64U}).v, 66U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(132U)).v, 130U);

    sag_textbuf_delete(tb, (Span){1U, 3U});
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){128U}).v, 128U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(130U)).v, 130U);
    sag_textbuf_free(tb);

    memset(two_lines, 'a', 130U);
    two_lines[130U] = '\n';
    memset(two_lines + 131U, 'b', 130U);
    tb = sag_textbuf_from_bytes(two_lines, sizeof(two_lines));
    SAG_ASSERT_EQ_U64(tb->graphemes.len, 4U);
    line = sag_textbuf_line_span(tb, LINENO(1U));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){64U}).v, 195U);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(261U)).v, 130U);
    sag_textbuf_free(tb);

    memset(long_lines, 'a', 700U);
    long_lines[700U] = '\n';
    memset(long_lines + 701U, 'b', 700U);
    tb = sag_textbuf_from_bytes(long_lines, sizeof(long_lines));
    sag_textbuf_insert(tb, BYTEOFF(650U), split, sizeof(split));
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 3U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(650U)).v, 650U);
    line = sag_textbuf_line_span(tb, LINENO(1U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(703U)).v, 52U);
    line = sag_textbuf_line_span(tb, LINENO(2U));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){64U}).v, 768U);

    sag_textbuf_delete(tb, (Span){650U, 653U});
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 2U);
    line = sag_textbuf_line_span(tb, LINENO(1U));
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){64U}).v, 765U);
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(1401U)).v, 700U);
    sag_textbuf_free(tb);

    memset(deferred, 'a', sizeof(deferred));
    tb = sag_textbuf_from_bytes(deferred, sizeof(deferred));
    sag_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(701U)).v,
                      700U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(701U)).v, 701U);
    sag_textbuf_delete(tb, (Span){10U, 11U});
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    SAG_ASSERT_EQ_U64(sag_grapheme_prev_boundary(tb, BYTEOFF(700U)).v,
                      699U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(700U)).v, 700U);
    sag_textbuf_free(tb);

    tb = sag_textbuf_from_bytes(deferred, sizeof(deferred));
    sag_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 1U);
    sag_textbuf_insert(tb, BYTEOFF(20U), &y, 1U);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(702U)).v, 702U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    sag_textbuf_free(tb);

    tb = sag_textbuf_from_bytes(long_lines, sizeof(long_lines));
    sag_textbuf_insert(tb, BYTEOFF(1000U), &x, 1U);
    sag_textbuf_insert(tb, BYTEOFF(100U), split, sizeof(split));
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 3U);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(100U)).v, 100U);
    line = sag_textbuf_line_span(tb, LINENO(1U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(703U)).v, 602U);
    line = sag_textbuf_line_span(tb, LINENO(2U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(1405U)).v,
                      701U);
    SAG_ASSERT_EQ_U64(sag_gcol_to_off(tb, line, (GCol){300U}).v, 1004U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    sag_textbuf_free(tb);

    tb = sag_textbuf_from_bytes(deferred, sizeof(deferred));
    sag_textbuf_insert(tb, BYTEOFF(10U), &x, 1U);
    sag_textbuf_insert(tb, BYTEOFF(20U), &y, 1U);
    sag_textbuf_insert(tb, BYTEOFF(30U), &z, 1U);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 0U);
    SAG_ASSERT(tb->graphemes.pending.rebuild_required);
    line = sag_textbuf_line_span(tb, LINENO(0U));
    SAG_ASSERT_EQ_U64(sag_off_to_gcol(tb, line, BYTEOFF(703U)).v, 703U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    SAG_ASSERT(!tb->graphemes.pending.rebuild_required);
    sag_textbuf_free(tb);

    tb = sag_textbuf_from_bytes(split_cluster, sizeof(split_cluster));
    sag_textbuf_insert(tb, BYTEOFF(3U), &x, 1U);
    cursor.pos = BYTEOFF(sizeof(split_cluster) + 1U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){UINT64_MAX};
    sag_cursor_left(tb, &cursor);
    SAG_ASSERT_EQ_U64(cursor.pos.v, 3U);
    SAG_ASSERT_EQ_U64(cursor.goal_col.v, 1U);
    sag_cursor_left(tb, &cursor);
    SAG_ASSERT_EQ_U64(cursor.pos.v, 0U);
    SAG_ASSERT_EQ_U64(cursor.goal_col.v, 0U);
    sag_textbuf_free(tb);

    tb = sag_textbuf_from_bytes(latin2_run, sizeof(latin2_run));
    sag_textbuf_insert(tb, BYTEOFF(0U), &x, 1U);
    sag_textbuf_insert(tb, BYTEOFF(3U), &y, 1U);
    SAG_ASSERT_EQ_U64(tb->graphemes.pending.len, 2U);
    SAG_ASSERT(tb->graphemes.gen != tb->gen);
    cursor.pos = BYTEOFF(sizeof(latin2_run) + 2U);
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){UINT64_MAX};
    sag_cursor_left(tb, &cursor);
    SAG_ASSERT_EQ_U64(cursor.pos.v, 6U);
    SAG_ASSERT_EQ_U64(cursor.goal_col.v, 4U);
    sag_cursor_left(tb, &cursor);
    SAG_ASSERT_EQ_U64(cursor.pos.v, 4U);
    SAG_ASSERT_EQ_U64(cursor.goal_col.v, 3U);
    SAG_ASSERT_EQ_U64(tb->graphemes.gen, tb->gen);
    sag_textbuf_free(tb);
}
