#ifndef SAG_UNICODE_WIDTH_H
#define SAG_UNICODE_WIDTH_H

#include "../util/base.h"

typedef enum {
    SAG_EAW_N = 0,
    SAG_EAW_NA,
    SAG_EAW_A,
    SAG_EAW_W,
    SAG_EAW_F,
    SAG_EAW_H
} SagEaw;

typedef struct {
    bool ambiguous_wide;
} SagWidthOpts;

/* Startup-only process option. NULL restores the deterministic default. */
void sag_width_set_opts(const SagWidthOpts *opts);

int sag_cp_width(u32 cp);
int sag_cluster_width(const u8 *s, size_t len);
int sag_str_width(const u8 *s, size_t len, u32 tabw);
size_t sag_str_clip(const u8 *s, size_t len, int max_cells,
                    int *out_cells);

/*
 * Glyph rendering for controls and invalid bytes lands in Sprint 5;
 * viewport tab-stop consumption lands in Sprint 15. Word_Break belongs to
 * Sprint 16 and case folding to Sprint 20. Sagitta deliberately provides no
 * normalization API: normalization would rewrite untouched file bytes.
 * UAX #14 line breaking is outside 1.0 because Sprint 15 wraps on whitespace
 * and cluster boundaries. UAX #9 bidi is outside 1.0, so RTL stays in logical
 * order. Unicode 17 tables are deferred until after 1.0 to keep the 16.0.0
 * conformance target stable throughout this campaign.
 */

#endif
