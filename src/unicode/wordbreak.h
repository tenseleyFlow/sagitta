#ifndef SAG_UNICODE_WORDBREAK_H
#define SAG_UNICODE_WORDBREAK_H

#include "../util/base.h"

#define SAG_WB_TRIE_HI 0x30000u
#define SAG_WB_TRIE_SHIFT 7u
#define SAG_WB_TRIE_BLOCK (1u << SAG_WB_TRIE_SHIFT)

typedef enum {
    SAG_WB_OTHER = 0,
    SAG_WB_CR,
    SAG_WB_LF,
    SAG_WB_NEWLINE,
    SAG_WB_EXTEND,
    SAG_WB_FORMAT,
    SAG_WB_ZWJ,
    SAG_WB_WSEGSPACE,
    SAG_WB_ALETTER,
    SAG_WB_HEBREW_LETTER,
    SAG_WB_NUMERIC,
    SAG_WB_KATAKANA,
    SAG_WB_EXTENDNUMLET,
    SAG_WB_REGIONAL_INDICATOR,
    SAG_WB_MIDLETTER,
    SAG_WB_MIDNUM,
    SAG_WB_MIDNUMLET,
    SAG_WB_SINGLE_QUOTE,
    SAG_WB_DOUBLE_QUOTE
} SagWb;

enum {
    SAG_WB_RECORD_MASK = 0x1Fu,
    SAG_WB_RECORD_WHITE_SPACE = 0x20u
};

struct SagWbRange {
    u32 lo;
    u32 hi;
    u8 rec;
};

extern const u16 sag_wb_stage1[SAG_WB_TRIE_HI >> SAG_WB_TRIE_SHIFT];
extern const u8 sag_wb_stage2[];
extern const struct SagWbRange sag_wb_hi[];
extern const u32 sag_wb_hi_len;

SagWb sag_wb_prop(u32 cp);
bool sag_wb_is_ignored(SagWb prop);
bool sag_unicode_is_white_space(u32 cp);
bool sag_unicode_is_extended_pictographic(u32 cp);

#endif
