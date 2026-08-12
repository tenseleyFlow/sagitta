#ifndef YEW_EDIT_SHADOW_H
#define YEW_EDIT_SHADOW_H

#include <stdbool.h>

#include "edit/loop.h"
#include "text/piece.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct EditCtx EditCtx;
typedef struct Win Win;

typedef enum ShadowProv {
    YEW_SHADOW_INDEX = 0,
    YEW_SHADOW_LSP,
    YEW_SHADOW_AI,
    YEW_SHADOW_NPROV
} ShadowProv;

typedef struct ShadowReq {
    u32 buf_id;
    u64 buf_gen;
    ByteOff pos;
    Span line;
    u32 seq;
    u8 prov;
} ShadowReq;

typedef struct ShadowSug {
    u32 seq;
    u8 prov;
    u32 buf_id;
    u64 buf_gen;
    ByteOff pos;
    const u8 *text;
    u32 len;
    u32 consumed;
    TextBuf *scratch;
} ShadowSug;

typedef struct ShadowProvider {
    const char *name;
    u8 prov;
    u32 debounce_ms;
    bool (*request)(Ed *ed, const ShadowReq *req);
    void (*cancel)(Ed *ed, u32 buf_id, u32 up_to);
} ShadowProvider;

typedef struct Shadow {
    bool live;
    bool suppressed;
    /* Keep provider-owned bytes alive until the edit choke point has
     * finished journaling an acceptance that consumes the whole ghost. */
    bool accepting;
    ShadowSug sug;
    u8 *owned_text;
    u32 seq_next[YEW_SHADOW_NPROV];
    u32 seq_min[YEW_SHADOW_NPROV];
    TimerId timer;
    i64 armed_at_ms;
    u8 pending_mask;
    u8 max_lines;
    u16 draw_row;
    u16 vrows;
} Shadow;

typedef struct ShadowStats {
    u64 requests;
    u64 delivered;
    u64 dropped_stale;
    u64 dropped_gen;
    u64 accepted_word;
    u64 accepted_line;
    u64 accepted_all;
    u64 revalidate_fail;
} ShadowStats;

void yew_shadow_init(Shadow *shadow);
void yew_shadow_free(Shadow *shadow);
void yew_shadow_dismiss(Ed *ed, Win *win);

void yew_shadow_register(const ShadowProvider *provider);
void yew_shadow_deliver(Ed *ed, const ShadowSug *suggestion);
/* Passive suggestions are single-cursor only; ineligible windows are
 * dismissed and carry no timer. */
void yew_shadow_arm(Ed *ed, Win *win);
void yew_shadow_fire(Ed *ed, Win *win);
bool yew_shadow_accept_word(Ed *ed, Win *win, bool alt);
bool yew_shadow_accept_line(Ed *ed, Win *win);
bool yew_shadow_accept_all(Ed *ed, Win *win);

/* Internal guard exposed so the staleness suite can exercise byte-exact
 * piece-boundary cases without accepting text. */
i64 yew_shadow_revalidate(const TextBuf *tb, const ShadowSug *suggestion,
                          ByteOff cursor);

/* Called only by the fixed post-edit notification list. */
void yew_shadow_on_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len);

#endif
