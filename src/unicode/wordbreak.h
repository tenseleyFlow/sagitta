#ifndef YEW_UNICODE_WORDBREAK_H
#define YEW_UNICODE_WORDBREAK_H

#include "../util/base.h"

#define YEW_WB_TRIE_HI 0x30000u
#define YEW_WB_TRIE_SHIFT 7u
#define YEW_WB_TRIE_BLOCK (1u << YEW_WB_TRIE_SHIFT)

typedef enum {
    YEW_WB_OTHER = 0,
    YEW_WB_CR,
    YEW_WB_LF,
    YEW_WB_NEWLINE,
    YEW_WB_EXTEND,
    YEW_WB_FORMAT,
    YEW_WB_ZWJ,
    YEW_WB_WSEGSPACE,
    YEW_WB_ALETTER,
    YEW_WB_HEBREW_LETTER,
    YEW_WB_NUMERIC,
    YEW_WB_KATAKANA,
    YEW_WB_EXTENDNUMLET,
    YEW_WB_REGIONAL_INDICATOR,
    YEW_WB_MIDLETTER,
    YEW_WB_MIDNUM,
    YEW_WB_MIDNUMLET,
    YEW_WB_SINGLE_QUOTE,
    YEW_WB_DOUBLE_QUOTE
} YewWb;

enum {
    YEW_WB_RECORD_MASK = 0x1Fu,
    YEW_WB_RECORD_WHITE_SPACE = 0x20u
};

struct YewWbRange {
    u32 lo;
    u32 hi;
    u8 rec;
};

extern const u16 yew_wb_stage1[YEW_WB_TRIE_HI >> YEW_WB_TRIE_SHIFT];
extern const u8 yew_wb_stage2[];
extern const struct YewWbRange yew_wb_hi[];
extern const u32 yew_wb_hi_len;

YewWb yew_wb_prop(u32 cp);
bool yew_wb_is_ignored(YewWb prop);
bool yew_unicode_is_white_space(u32 cp);
bool yew_unicode_is_extended_pictographic(u32 cp);

#endif
