#ifndef YEW_TERM_RENDER_H
#define YEW_TERM_RENDER_H

#include <stdbool.h>
#include <stddef.h>

#include "term/grid.h"
#include "term/tty.h"
#include "util/base.h"
#include "util/buf.h"

enum {
    YEW_RENDER_TIER_16 = 0,
    YEW_RENDER_TIER_256 = 1,
    YEW_RENDER_TIER_TRUECOLOR = 2
};

typedef struct Render {
    YewColor fg;
    YewColor bg;
    u16 attrs;
    bool pen_known;
    u16 row;
    u16 col;
    bool pos_known;
    u8 tier;
    /*
     * Sprint 27 §8: NO_COLOR is set and non-empty.
     *
     * Read ONCE, at init (Sprint 0's single-decision rule).  When it is
     * on, ZERO colour SGR parameters are emitted and identity is
     * carried by attributes alone — reverse for active/selected, dim
     * for inactive, bold for headers, underline for the current match.
     * A half-honoured NO_COLOR is worse than none: the user who set it
     * did so because the colours are unreadable on their terminal.
     */
    bool no_color;
    bool sync;
    bool undercurl;
    bool cursor_known;
    bool cursor_visible;
    YewCursorShape cursor_shape;
    u64 cursor_generation;
    u64 frames;
    u64 bytes;
} Render;

u8 yew_render_tier(const TtyCaps *caps,
                   const char *(*getv)(const char *));
u8 yew_rgb_to_256(u8 r, u8 g, u8 b);
u8 yew_rgb_to_16(u8 r, u8 g, u8 b);

void yew_render_init(Render *r, const TtyCaps *caps,
                     const char *(*getv)(const char *));
size_t yew_render_frame(Render *r, Grid *g, Bytebuf *out);

/*
 * Out-of-band terminal controls are queued during an input burst and are
 * appended only after the frame's synchronized-output end marker. The queue
 * is process-global because yew owns exactly one terminal output stream.
 */
void yew_term_oob_queue(const u8 *seq, u64 n);
u64 yew_term_oob_pending(void);
size_t yew_term_oob_flush(Bytebuf *out);
void yew_term_oob_clear(void);

#endif
