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
