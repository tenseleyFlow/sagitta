#ifndef SAG_UNICODE_GRAPHEME_H
#define SAG_UNICODE_GRAPHEME_H

#include "util/base.h"

typedef enum {
    SAG_GCB_OTHER = 0,
    SAG_GCB_CR,
    SAG_GCB_LF,
    SAG_GCB_CONTROL,
    SAG_GCB_EXTEND,
    SAG_GCB_ZWJ,
    SAG_GCB_RI,
    SAG_GCB_PREPEND,
    SAG_GCB_SPACINGMARK,
    SAG_GCB_L,
    SAG_GCB_V,
    SAG_GCB_T,
    SAG_GCB_LV,
    SAG_GCB_LVT,
    SAG_GCB_COUNT
} SagGcb;

typedef enum {
    SAG_INCB_NONE = 0,
    SAG_INCB_LINKER,
    SAG_INCB_CONSONANT,
    SAG_INCB_EXTEND
} SagIncb;

typedef struct {
    u8 prev_gcb;
    u8 flags;
} SagGbState;

enum {
    SAG_GBF_RI_ODD = 1u << 0,
    SAG_GBF_PICT = 1u << 1,
    SAG_GBF_INCB_C = 1u << 2,
    SAG_GBF_INCB_L = 1u << 3
};

void sag_gb_init(SagGbState *st);
bool sag_gb_boundary(SagGbState *st, u32 cp);

size_t sag_gb_next_bytes(const u8 *s, size_t len, size_t pos);
size_t sag_gb_prev_bytes(const u8 *s, size_t len, size_t pos);
size_t sag_gb_count_bytes(const u8 *s, size_t len);

#define SAG_CLUSTER_TAB 255u

typedef struct {
    size_t off;
    size_t len;
    u32 base_cp;
    u8 cells;
} SagCluster;

bool sag_cluster_next(const u8 *s, size_t len, size_t *pos,
                      SagCluster *out);

/* TextBuf coordinate wrappers land in Sprint 9. Word_Break lands in
 * Sprint 16; case folding in Sprint 20. Glyph attributes and tab-stop
 * consumption land in Sprints 5 and 15. Normalization is deliberately
 * absent for 1.0: an editor must not rewrite untouched file bytes. UAX #14
 * line breaking is outside 1.0 because Sprint 15 wraps on whitespace and
 * cluster boundaries. UAX #9 bidi is outside 1.0, so RTL remains in logical
 * order. Unicode 17 tables are a post-1.0 update; 16.0.0 stays pinned to keep
 * this campaign's conformance results stable. */

#endif
