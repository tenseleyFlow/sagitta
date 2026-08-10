#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/word.h"
#include "unicode/utf8.h"

enum {
    WB_LINE_CAP = 1024,
    WB_BYTE_CAP = 512,
    WB_MARK_CAP = 256,
    WB_EXPECTED_ROWS = 1826,
    WB_EXPECTED_MARKS = 8030
};

static bool wb_marker(const char *token, bool *is_break)
{
    if (strcmp(token, "\xc3\xb7") == 0) {
        *is_break = true;
        return true;
    }
    if (strcmp(token, "\xc3\x97") == 0) {
        *is_break = false;
        return true;
    }
    return false;
}

void test_wordbreak_unicode_16_corpus(void)
{
    FILE *fp = fopen("ucd/16.0.0/WordBreakTest.txt", "r");
    char line[WB_LINE_CAP];
    size_t rows = 0U;
    size_t marks_total = 0U;

    YEW_ASSERT_NOT_NULL(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        u8 bytes[WB_BYTE_CAP];
        u64 mark_pos[WB_MARK_CAP];
        bool mark_break[WB_MARK_CAP];
        size_t byte_len = 0U;
        size_t mark_count = 0U;
        size_t cp_count = 0U;
        char *comment = strchr(line, '#');
        char *token;
        bool expect_marker = true;

        YEW_ASSERT(strchr(line, '\n') != NULL || feof(fp));
        if (comment != NULL)
            *comment = '\0';
        token = strtok(line, " \t\r\n");
        if (token == NULL)
            continue;

        while (token != NULL) {
            if (expect_marker) {
                bool is_break = false;

                YEW_ASSERT(wb_marker(token, &is_break));
                YEW_ASSERT(mark_count < WB_MARK_CAP);
                mark_pos[mark_count] = (u64)byte_len;
                mark_break[mark_count] = is_break;
                mark_count++;
            } else {
                char *end;
                unsigned long value = strtoul(token, &end, 16);
                u8 encoded[YEW_UTF8_MAX];
                size_t encoded_len;

                YEW_ASSERT(end != token && *end == '\0' &&
                           value <= 0x10FFFFUL);
                encoded_len = yew_utf8_encode((u32)value, encoded);
                YEW_ASSERT(encoded_len > 0U);
                YEW_ASSERT(byte_len + encoded_len <= WB_BYTE_CAP);
                memcpy(bytes + byte_len, encoded, encoded_len);
                byte_len += encoded_len;
                cp_count++;
            }
            expect_marker = !expect_marker;
            token = strtok(NULL, " \t\r\n");
        }

        YEW_ASSERT(!expect_marker);
        YEW_ASSERT_EQ_U64(mark_count, cp_count + 1U);
        {
            TextBuf *tb = yew_textbuf_from_bytes(bytes, byte_len);
            size_t i;

            YEW_ASSERT_NOT_NULL(tb);
            for (i = 0U; i < mark_count; i++)
                YEW_ASSERT(yew_word_boundary(tb, BYTEOFF(mark_pos[i])) ==
                           mark_break[i]);
            yew_textbuf_free(tb);
        }
        rows++;
        marks_total += mark_count;
    }
    YEW_ASSERT(ferror(fp) == 0);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_U64(rows, WB_EXPECTED_ROWS);
    YEW_ASSERT_EQ_U64(marks_total, WB_EXPECTED_MARKS);
}
