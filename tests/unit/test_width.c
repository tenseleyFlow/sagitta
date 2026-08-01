#include "harness.h"

#include "unicode/utf8.h"
#include "unicode/width.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH_FIXTURE "tests/unit/fixtures/unicode/width_golden.txt"

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static size_t parse_hex_bytes(const char *text, u8 *out, size_t cap)
{
    size_t len = 0u;

    while (*text != '\0') {
        int hi;
        int lo;

        while (isspace((unsigned char)*text))
            text++;
        if (*text == '\0')
            break;
        hi = hex_digit((unsigned char)text[0]);
        lo = hex_digit((unsigned char)text[1]);
        SAG_ASSERT(hi >= 0 && lo >= 0);
        SAG_ASSERT(len < cap);
        out[len++] = (u8)((hi << 4) | lo);
        text += 2;
    }
    return len;
}

static size_t split_row(char *line, char **cols, size_t cap)
{
    size_t n = 0u;
    char *p = line;

    while (n < cap) {
        char *bar;

        cols[n++] = p;
        bar = strchr(p, '|');
        if (bar == NULL)
            break;
        *bar = '\0';
        p = bar + 1;
    }
    if (n > 0u) {
        size_t last = strlen(cols[n - 1u]);
        while (last > 0u && (cols[n - 1u][last - 1u] == '\n' ||
                             cols[n - 1u][last - 1u] == '\r'))
            cols[n - 1u][--last] = '\0';
    }
    return n;
}

void test_width_codepoints(void)
{
    SagWidthOpts opts = {false};

    sag_width_set_opts(&opts);
    SAG_ASSERT_EQ_I64(sag_cp_width((u32)'A'), 1);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x0009u), -1);
    SAG_ASSERT_EQ_I64(sag_cluster_width((const u8 *)"\t", 1u), 0);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x0000u), 2);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x007fu), 2);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x0085u), 4);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x00adu), 1);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x0301u), 0);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x200du), 0);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x6f22u), 2);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x1f600u), 2);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x00a1u), 1);
    SAG_ASSERT_EQ_I64(sag_cp_width(sag_utf8_escape_of(0xffu)), 4);

    opts.ambiguous_wide = true;
    sag_width_set_opts(&opts);
    SAG_ASSERT_EQ_I64(sag_cp_width(0x00a1u), 2);
    SAG_ASSERT_EQ_I64(sag_cp_width((u32)'A'), 1);
    sag_width_set_opts(NULL);
}

void test_width_golden(void)
{
    FILE *fp = fopen(WIDTH_FIXTURE, "r");
    char line[512];
    size_t rows = 0u;

    SAG_ASSERT_NOT_NULL(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *cols[4];
        u8 bytes[128];
        size_t ncols;
        size_t len;
        int narrow;
        int wide;
        SagWidthOpts opts = {false};

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        ncols = split_row(line, cols, SAG_ARRAY_LEN(cols));
        SAG_ASSERT_EQ_U64(ncols, 4u);
        len = parse_hex_bytes(cols[1], bytes, sizeof(bytes));
        narrow = (int)strtol(cols[2], NULL, 10);
        wide = (int)strtol(cols[3], NULL, 10);

        sag_width_set_opts(&opts);
        SAG_ASSERT_EQ_I64(sag_cluster_width(bytes, len), narrow);
        opts.ambiguous_wide = true;
        sag_width_set_opts(&opts);
        SAG_ASSERT_EQ_I64(sag_cluster_width(bytes, len), wide);
        rows++;
    }
    SAG_ASSERT(ferror(fp) == 0);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT(rows >= 60u);
    sag_width_set_opts(NULL);
}

void test_width_strings(void)
{
    static const u8 mixed[] = {'A', '\t', 0xe6u, 0xbcu, 0xa2u};
    static const u8 clusters[] = {
        'e', 0xccu, 0x81u,
        0xf0u, 0x9fu, 0x87u, 0xbau,
        0xf0u, 0x9fu, 0x87u, 0xb8u,
        0xffu
    };

    sag_width_set_opts(NULL);
    SAG_ASSERT_EQ_I64(sag_str_width(NULL, 12u, 4u), 0);
    SAG_ASSERT_EQ_I64(sag_str_width(mixed, sizeof(mixed), 4u), 6);
    SAG_ASSERT_EQ_I64(sag_str_width(mixed, sizeof(mixed), 8u), 10);
    SAG_ASSERT_EQ_I64(sag_str_width(mixed, sizeof(mixed), 0u), 4);
    SAG_ASSERT_EQ_I64(sag_str_width(clusters, sizeof(clusters), 4u), 7);
}

void test_width_clip(void)
{
    static const u8 text[] = {
        'A', 0xe6u, 0xbcu, 0xa2u, 'B',
        0xf0u, 0x9fu, 0x91u, 0x8du,
        0xf0u, 0x9fu, 0x8fu, 0xbdu, 'C'
    };
    static const size_t kept[] = {0u, 1u, 1u, 4u, 5u, 5u, 13u, 14u};
    static const int cells[] = {0, 1, 1, 3, 4, 4, 6, 7};
    int max_cells;

    for (max_cells = 0; max_cells <= 7; max_cells++) {
        int out_cells = -1;
        size_t bytes = sag_str_clip(text, sizeof(text), max_cells,
                                    &out_cells);

        SAG_ASSERT_EQ_U64(bytes, kept[max_cells]);
        SAG_ASSERT_EQ_I64(out_cells, cells[max_cells]);
        SAG_ASSERT(out_cells <= max_cells);
    }
    {
        int tab_cells = -1;

        SAG_ASSERT_EQ_U64(sag_str_clip((const u8 *)"\tA", 2u, 1,
                                       &tab_cells), 1u);
        SAG_ASSERT_EQ_I64(tab_cells, 1);
    }
    SAG_ASSERT_EQ_U64(sag_str_clip(NULL, 4u, 3, NULL), 0u);
    SAG_ASSERT_EQ_U64(sag_str_clip(text, sizeof(text), -1, NULL), 0u);
}
