#ifndef SAG_TEST_PTY_VT_H
#define SAG_TEST_PTY_VT_H

#include <stdbool.h>
#include <stddef.h>

#include "term/grid.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct VtCell {
    u8 g[8];
    u8 nb;
    SagColor fg;
    SagColor bg;
    u16 attrs;
    u8 w;
} VtCell;

typedef enum VtProfile {
    VT_PROFILE_MODERN = 0,
    VT_PROFILE_NOKITTY,
    VT_PROFILE_NOSYNC,
    VT_PROFILE_DUMB
} VtProfile;

enum {
    VT_MODE_BRACKETED_PASTE = 1u << 0,
    VT_MODE_BUTTON_MOUSE = 1u << 1,
    VT_MODE_SGR_MOUSE = 1u << 2,
    VT_MODE_FOCUS = 1u << 3
};

enum {
    VT_CLUSTER_BYTES_MAX = 4096,
    VT_TRANSCRIPT_BYTES_MAX = 4096
};

enum {
    VT_PROBE_KITTY = 1u << 0,
    VT_PROBE_SYNC = 1u << 1,
    VT_PROBE_DA = 1u << 2
};

typedef struct VtScreen {
    int rows;
    int cols;
    int cur_r;
    int cur_c;
    bool alt;
    bool cur_vis;
    bool in_sync;
    bool primary_written;
    u32 modes;
    u32 kitty[8];
    int ksp;
    VtCell *cells;
    Bytebuf errors;
    u32 nerrors;
    u32 nsync_pairs;

    /* Probe replies are drained by the pty runner and written to master. */
    VtProfile profile;
    Bytebuf replies;
    u32 probes;
    u8 probe_order[3];
    u8 nprobes;

    SagColor fg;
    SagColor bg;
    u16 attrs;
    int saved_r;
    int saved_c;
    bool saved_valid;

    u8 parse_state;
    u8 seq[128];
    size_t nseq;
    SagU8Dec u8dec;
    SagGbState gb;
    int cluster_r;
    int cluster_c;
    bool cluster_valid;

    /* Full graphemes live here when VtCell.g's pinned inline store spills. */
    Bytebuf glyphs;
    size_t *glyph_off;
    size_t *glyph_len;

    /* Strict by default; exceptional cases may retain an allowed transcript. */
    Bytebuf primary;
    bool allow_primary_text;
    bool allow_idempotent_restore;
} VtScreen;

void vt_init(VtScreen *v, int rows, int cols);
void vt_free(VtScreen *v);
void vt_feed(VtScreen *v, const u8 *b, size_t n);
void vt_resize(VtScreen *v, int rows, int cols);

bool vt_profile_from_name(const char *name, VtProfile *out);
void vt_set_profile(VtScreen *v, VtProfile profile);
/* Appends pending replies to out and clears the VT's reply queue. */
void vt_take_replies(VtScreen *v, Bytebuf *out);
u32 vt_take_queries(VtScreen *v);
const u8 *vt_cell_bytes(const VtScreen *v, const VtCell *cell, size_t *len);
bool vt_set_cell(VtScreen *v, int row, int col, const u8 *bytes, size_t n,
                 SagColor fg, SagColor bg, u16 attrs, u8 width);
void vt_set_primary_policy(VtScreen *v, bool allow_text);
void vt_set_restore_policy(VtScreen *v, bool allow_idempotent_restore);

#endif
