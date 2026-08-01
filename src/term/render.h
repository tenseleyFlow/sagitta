#ifndef SAG_TERM_RENDER_H
#define SAG_TERM_RENDER_H

#include <stdbool.h>
#include <stddef.h>

#include "term/grid.h"
#include "term/tty.h"
#include "util/base.h"
#include "util/buf.h"

enum {
    SAG_RENDER_TIER_16 = 0,
    SAG_RENDER_TIER_256 = 1,
    SAG_RENDER_TIER_TRUECOLOR = 2
};

typedef struct Render {
    SagColor fg;
    SagColor bg;
    u16 attrs;
    bool pen_known;
    u16 row;
    u16 col;
    bool pos_known;
    u8 tier;
    bool sync;
    bool undercurl;
    bool cursor_known;
    bool cursor_visible;
    u64 frames;
    u64 bytes;
} Render;

u8 sag_render_tier(const TtyCaps *caps,
                   const char *(*getv)(const char *));
u8 sag_rgb_to_256(u8 r, u8 g, u8 b);
u8 sag_rgb_to_16(u8 r, u8 g, u8 b);

void sag_render_init(Render *r, const TtyCaps *caps,
                     const char *(*getv)(const char *));
size_t sag_render_frame(Render *r, Grid *g, Bytebuf *out);

/*
 * Out-of-band terminal controls are queued during an input burst and are
 * appended only after the frame's synchronized-output end marker. The queue
 * is process-global because Sagitta owns exactly one terminal output stream.
 */
void sag_term_oob_queue(const u8 *seq, u64 n);
u64 sag_term_oob_pending(void);
size_t sag_term_oob_flush(Bytebuf *out);
void sag_term_oob_clear(void);

#endif
