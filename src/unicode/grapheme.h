#ifndef YEW_UNICODE_GRAPHEME_H
#define YEW_UNICODE_GRAPHEME_H

#include "util/base.h"

typedef enum {
    YEW_GCB_OTHER = 0,
    YEW_GCB_CR,
    YEW_GCB_LF,
    YEW_GCB_CONTROL,
    YEW_GCB_EXTEND,
    YEW_GCB_ZWJ,
    YEW_GCB_RI,
    YEW_GCB_PREPEND,
    YEW_GCB_SPACINGMARK,
    YEW_GCB_L,
    YEW_GCB_V,
    YEW_GCB_T,
    YEW_GCB_LV,
    YEW_GCB_LVT,
    YEW_GCB_COUNT
} YewGcb;

typedef enum {
    YEW_INCB_NONE = 0,
    YEW_INCB_LINKER,
    YEW_INCB_CONSONANT,
    YEW_INCB_EXTEND
} YewIncb;

typedef struct {
    u8 prev_gcb;
    u8 flags;
} YewGbState;

enum {
    YEW_GBF_RI_ODD = 1u << 0,
    YEW_GBF_PICT = 1u << 1,
    YEW_GBF_INCB_C = 1u << 2,
    YEW_GBF_INCB_L = 1u << 3
};

void yew_gb_init(YewGbState *st);
bool yew_gb_boundary(YewGbState *st, u32 cp);

size_t yew_gb_next_bytes(const u8 *s, size_t len, size_t pos);
size_t yew_gb_prev_bytes(const u8 *s, size_t len, size_t pos);
size_t yew_gb_count_bytes(const u8 *s, size_t len);

#define YEW_CLUSTER_TAB 255u

typedef struct {
    size_t off;
    size_t len;
    u32 base_cp;
    u8 cells;
} YewCluster;

bool yew_cluster_next(const u8 *s, size_t len, size_t *pos,
                      YewCluster *out);

/* TextBuf coordinate wrappers land in Sprint 9. Word_Break lands in
 * Sprint 16; case folding in Sprint 20. Glyph attributes and tab-stop
 * consumption land in Sprints 5 and 15. Normalization is deliberately
 * absent for 1.0: an editor must not rewrite untouched file bytes. UAX #14
 * line breaking is outside 1.0 because Sprint 15 wraps on whitespace and
 * cluster boundaries. UAX #9 bidi is outside 1.0, so RTL remains in logical
 * order. Unicode 17 tables are a post-1.0 update; 16.0.0 stays pinned to keep
 * this campaign's conformance results stable. */

#endif
