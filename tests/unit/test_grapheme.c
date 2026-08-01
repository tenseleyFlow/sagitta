#include "harness.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/grapheme.h"
#include "unicode/utf8.h"

static void assert_clusters(const u8 *s, size_t len,
                            const size_t *lengths, size_t count)
{
    size_t i;
    size_t pos = 0;

    SAG_ASSERT_EQ_U64(sag_gb_count_bytes(s, len), count);
    for (i = 0; i < count; i++) {
        size_t start = pos;
        size_t next = sag_gb_next_bytes(s, len, pos);
        SAG_ASSERT_EQ_U64(next - pos, lengths[i]);
        pos = next;
        SAG_ASSERT_EQ_U64(sag_gb_prev_bytes(s, len, pos), start);
    }
    SAG_ASSERT_EQ_U64(pos, len);
}

void test_grapheme_state_size(void)
{
    SagGbState st;

    memset(&st, 0xFF, sizeof(st));
    sag_gb_init(&st);
    SAG_ASSERT_EQ_U64(sizeof(st), 2);
    SAG_ASSERT_EQ_U64(st.prev_gcb, SAG_GCB_OTHER);
    SAG_ASSERT_EQ_U64(st.flags, 0);
}

void test_grapheme_ascii(void)
{
    static const u8 text[] = "arrow";
    static const size_t lengths[] = {1, 1, 1, 1, 1};

    assert_clusters(text, sizeof(text) - 1, lengths,
                    SAG_ARRAY_LEN(lengths));
}

void test_grapheme_crlf_control(void)
{
    static const u8 text[] = {'a', '\r', '\n', 0, 'b'};
    static const size_t lengths[] = {1, 2, 1, 1};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_extend_spacing_prepend(void)
{
    static const u8 text[] = {
        0x65, 0xCC, 0x81,             /* e + acute */
        0x20,
        0xD8, 0x80, 0x61,             /* prepend + a */
        0x20,
        0xE0, 0xA4, 0x95, 0xE0, 0xA4, 0xBE /* ka + spacing mark */
    };
    static const size_t lengths[] = {3, 1, 3, 1, 6};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_hangul(void)
{
    static const u8 text[] = {
        0xE1, 0x84, 0x80, 0xE1, 0x85, 0xA1, 0xE1, 0x86, 0xA8,
        0x20, 0xEA, 0xB0, 0x80, 0xE1, 0x86, 0xA8
    };
    static const size_t lengths[] = {9, 1, 6};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_ri_pairs(void)
{
    static const u8 text[] = {
        0xF0, 0x9F, 0x87, 0xA6, 0xF0, 0x9F, 0x87, 0xBA,
        0xF0, 0x9F, 0x87, 0xB8, 0xF0, 0x9F, 0x87, 0xA6
    };
    static const size_t lengths[] = {8, 8};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_emoji_zwj(void)
{
    static const u8 family[] = {
        0xF0, 0x9F, 0x91, 0xA8, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA7, 0xE2, 0x80, 0x8D,
        0xF0, 0x9F, 0x91, 0xA6
    };
    static const size_t lengths[] = {25};

    assert_clusters(family, sizeof(family), lengths,
                    SAG_ARRAY_LEN(lengths));
}

void test_grapheme_indic_conjunct(void)
{
    static const u8 text[] = {
        0xE0, 0xA4, 0x95, 0xE0, 0xA5, 0x8D, 0xE0, 0xA4, 0xB7
    };
    static const size_t lengths[] = {9};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_invalid_bytes(void)
{
    static const u8 text[] = {0xFF, 0x41, 0xED, 0xA0, 0x80};
    static const size_t lengths[] = {1, 1, 1, 1, 1};

    assert_clusters(text, sizeof(text), lengths, SAG_ARRAY_LEN(lengths));
}

void test_grapheme_backward_bound(void)
{
    u8 text[65 * 4];
    size_t pos = 0;
    size_t i;

    for (i = 0; i < 65; i++) {
        u8 encoded[SAG_UTF8_MAX];
        size_t n = sag_utf8_encode(0x1F1E6u + (u32)(i % 26), encoded);
        memcpy(text + pos, encoded, n);
        pos += n;
    }
    SAG_ASSERT_EQ_U64(pos, sizeof(text));
    /* The bounded restart deliberately sees an even 64-codepoint suffix,
     * so this odd RI run takes the documented parity approximation. */
    SAG_ASSERT_EQ_U64(sag_gb_prev_bytes(text, sizeof(text), sizeof(text)),
                      sizeof(text) - 8);
    SAG_ASSERT_EQ_U64(
        sag_gb_next_bytes(
            text, sizeof(text),
            sag_gb_prev_bytes(text, sizeof(text), sizeof(text))),
        sizeof(text));
}

static size_t parse_hex_bytes(char *field, u8 *out, size_t cap)
{
    size_t n = 0;
    char *p = field;

    while (*p != '\0') {
        char *end;
        unsigned long value;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;
        value = strtoul(p, &end, 16);
        SAG_ASSERT(end != p && value <= 0xFFu && n < cap);
        out[n++] = (u8)value;
        p = end;
    }
    return n;
}

static size_t parse_sizes(char *field, size_t *out, size_t cap)
{
    size_t n = 0;
    char *p = field;

    while (*p != '\0') {
        char *end;
        unsigned long value = strtoul(p, &end, 10);
        SAG_ASSERT(end != p && n < cap);
        out[n++] = (size_t)value;
        p = end;
        if (*p == ',')
            p++;
    }
    return n;
}

void test_grapheme_corpus(void)
{
    FILE *fp = fopen("tests/unit/fixtures/unicode/corpus.txt", "r");
    char line[2048];
    size_t cases = 0;

    SAG_ASSERT_NOT_NULL(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *fields[4];
        char *p = line;
        size_t i;
        u8 bytes[512];
        size_t lengths[256];
        size_t widths[256];
        size_t byte_len;
        size_t length_count;
        size_t width_count;
        size_t pos = 0;

        while (isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;
        fields[0] = p;
        for (i = 1; i < SAG_ARRAY_LEN(fields); i++) {
            p = strchr(p, '|');
            SAG_ASSERT_NOT_NULL(p);
            *p++ = '\0';
            fields[i] = p;
        }
        p = strchr(fields[3], '\n');
        if (p != NULL)
            *p = '\0';

        byte_len = parse_hex_bytes(fields[1], bytes, sizeof(bytes));
        length_count = parse_sizes(fields[2], lengths,
                                   SAG_ARRAY_LEN(lengths));
        width_count = parse_sizes(fields[3], widths, SAG_ARRAY_LEN(widths));
        SAG_ASSERT_EQ_U64(length_count, width_count);
        SAG_ASSERT_EQ_U64(sag_gb_count_bytes(bytes, byte_len), length_count);

        for (i = 0; i < length_count; i++) {
            SagCluster cluster;
            size_t start = pos;
            SAG_ASSERT(sag_cluster_next(bytes, byte_len, &pos, &cluster));
            SAG_ASSERT_EQ_U64(cluster.off, start);
            SAG_ASSERT_EQ_U64(cluster.len, lengths[i]);
            SAG_ASSERT_EQ_U64(cluster.cells, widths[i]);
            SAG_ASSERT_EQ_U64(sag_gb_prev_bytes(bytes, byte_len, pos), start);
        }
        SAG_ASSERT_EQ_U64(pos, byte_len);
        cases++;
    }
    SAG_ASSERT(!ferror(fp));
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
    SAG_ASSERT(cases >= 40);
}
