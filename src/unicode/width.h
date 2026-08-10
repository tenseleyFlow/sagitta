#ifndef YEW_UNICODE_WIDTH_H
#define YEW_UNICODE_WIDTH_H

#include "../util/base.h"

typedef enum {
    YEW_EAW_N = 0,
    YEW_EAW_NA,
    YEW_EAW_A,
    YEW_EAW_W,
    YEW_EAW_F,
    YEW_EAW_H
} YewEaw;

typedef struct {
    bool ambiguous_wide;
} YewWidthOpts;

typedef struct {
    u32 base_cp;
    u16 base_rec;
    u8 flags;
    u8 ri_count;
} YewClusterWidthState;

/* Startup-only process option. NULL restores the deterministic default. */
void yew_width_set_opts(const YewWidthOpts *opts);

int yew_cp_width(u32 cp);
void yew_cluster_width_init(YewClusterWidthState *state);
void yew_cluster_width_push(YewClusterWidthState *state, u32 cp);
int yew_cluster_width_finish(const YewClusterWidthState *state);
int yew_cluster_width(const u8 *s, size_t len);
int yew_str_width(const u8 *s, size_t len, u32 tabw);
size_t yew_str_clip(const u8 *s, size_t len, int max_cells,
                    int *out_cells);

/*
 * Glyph rendering for controls and invalid bytes lands in Sprint 5;
 * viewport tab-stop consumption lands in Sprint 15. Word_Break belongs to
 * Sprint 16 and case folding to Sprint 20. yew deliberately provides no
 * normalization API: normalization would rewrite untouched file bytes.
 * UAX #14 line breaking is outside 1.0 because Sprint 15 wraps on whitespace
 * and cluster boundaries. UAX #9 bidi is outside 1.0, so RTL stays in logical
 * order. Unicode 17 tables are deferred until after 1.0 to keep the 16.0.0
 * conformance target stable throughout this campaign.
 */

#endif
