#ifndef YEW_MOD_GIT_BLAME_H
#define YEW_MOD_GIT_BLAME_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/base.h"

enum {
    YEW_BLAME_RANGE_LINES = 64,
    YEW_BLAME_MAX_INFLIGHT = 2,
    YEW_BLAME_DEBOUNCE_MS = 200,
    YEW_BLAME_MIN_REMAINING_CELLS = 24,
    YEW_BLAME_SHA_MAX = 64
};

typedef struct BlameCache BlameCache;

/* All line and range values are zero-based; range_hi is exclusive. */
typedef struct BlameRequest {
    u64 token;
    u64 text_gen;
    u32 buf_id;
    u32 range_lo;
    u32 range_hi;
} BlameRequest;

typedef struct BlameLine {
    const char *sha;
    const char *author;
    const char *summary;
    i64 author_time;
    bool uncommitted;
    bool stale;
} BlameLine;

BlameCache *yew_blame_cache_new(void);
void yew_blame_cache_free(BlameCache *cache);

/* Quantizes a visible inclusive range to complete 64-line blocks. */
void yew_blame_quantize(LineNo top, LineNo bottom, u64 line_count,
                        u32 *range_lo, u32 *range_hi);

/* Records the most recently observed viewport. A changed observation starts
 * (or restarts) the debounce interval but never discards usable old data. */
void yew_blame_cache_observe(BlameCache *cache, u32 buf_id, u64 text_gen,
                             LineNo top, LineNo bottom, u64 line_count,
                             i64 now_ms);

/* Extracts one due request. False means debounce has not elapsed, the range is
 * already current/in flight, or the two-job cap is occupied. */
bool yew_blame_cache_take_request(BlameCache *cache, i64 now_ms,
                                  BlameRequest *request);

/* Publishes raw `git blame --incremental` output for a request. Completion of
 * an obsolete request only releases its slot; it cannot replace newer data. */
bool yew_blame_cache_finish(BlameCache *cache, const BlameRequest *request,
                            const u8 *output, u64 output_len);
void yew_blame_cache_fail(BlameCache *cache, const BlameRequest *request);

/* Returns current data, or the preceding generation marked stale while the
 * requested generation is pending. The pointer lives until the next lookup
 * or cache mutation. */
const BlameLine *yew_blame_cache_at(BlameCache *cache, u32 buf_id,
                                    u64 text_gen, LineNo line);
u32 yew_blame_cache_inflight(const BlameCache *cache);
size_t yew_blame_cache_metadata_count(const BlameCache *cache);

/* Deterministic presentation helpers. `now_epoch` is supplied by the caller;
 * no clock or locale is consulted here. Returned lengths exclude the NUL. */
size_t yew_blame_relative_time(char *dst, size_t cap, i64 author_epoch,
                               i64 now_epoch);
size_t yew_blame_format(char *dst, size_t cap, const BlameLine *line,
                        i64 now_epoch);

/* Decides whether an annotation may occupy the remainder of a display row.
 * `text_end` is the first cell after real text and `row_cells` is content
 * width. Wrapped logical lines are eligible only on their final display row. */
bool yew_blame_layout(u16 row_cells, u16 text_end, bool wrapped,
                      bool final_display_row, u16 *annotation_col,
                      u16 *available_cells);

#endif
