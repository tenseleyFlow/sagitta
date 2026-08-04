#ifndef SAG_UNICODE_CATEGORY_H
#define SAG_UNICODE_CATEGORY_H

/*
 * Unicode general-category and core-property bits.
 *
 * Sprint 2 emitted grapheme, width and word-break properties, which is
 * everything the renderer and the unit engines need.  Sprint 20's regex
 * engine needs more: \w is Alphabetic ∪ Nd ∪ Pc, and the POSIX bracket
 * classes are defined directly over general categories.  Answering those
 * from word-break data alone silently excludes CJK from \w — Han is
 * word-break Other by design, so that segmentation does not glue
 * ideographs together — which is correct for word motion and wrong for
 * "match a letter".  Hence this table.
 */

#include "../util/base.h"

#define SAG_CAT_TRIE_HI 0x30000u
#define SAG_CAT_TRIE_SHIFT 7u
#define SAG_CAT_TRIE_BLOCK (1u << SAG_CAT_TRIE_SHIFT)

enum {
    SAG_CAT_ALPHA = 0x0001u,    /* Alphabetic (derived core property)   */
    SAG_CAT_ND = 0x0002u,       /* general category Nd                  */
    SAG_CAT_UPPER = 0x0004u,    /* Uppercase                            */
    SAG_CAT_LOWER = 0x0008u,    /* Lowercase                            */
    SAG_CAT_PUNCT = 0x0010u,    /* P* or S*                             */
    SAG_CAT_CNTRL = 0x0020u,    /* Cc                                   */
    SAG_CAT_ZS = 0x0040u,       /* Zs                                   */
    SAG_CAT_PC = 0x0080u,       /* Pc — connector punctuation ('_')     */
    SAG_CAT_ASSIGNED = 0x0100u
};

struct SagCatRange {
    u32 lo;
    u32 hi;
    u16 rec;
};

extern const u16 sag_cat_stage1[SAG_CAT_TRIE_HI >> SAG_CAT_TRIE_SHIFT];
extern const u8 sag_cat_stage2[];
extern const u16 sag_cat_pal[];
extern const struct SagCatRange sag_cat_hi[];
extern const u32 sag_cat_hi_len;

u16 sag_cat_hi_lookup(u32 cp);

static inline u16 sag_cat_rec(u32 cp)
{
    if (cp < SAG_CAT_TRIE_HI) {
        size_t block = (size_t)sag_cat_stage1[cp >> SAG_CAT_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (SAG_CAT_TRIE_BLOCK - 1u));

        return sag_cat_pal[sag_cat_stage2[(block << SAG_CAT_TRIE_SHIFT) +
                                          slot]];
    }
    return sag_cat_hi_lookup(cp);
}

#endif
