#ifndef YEW_UI_COMPLMENU_H
#define YEW_UI_COMPLMENU_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "term/input.h"
#include "text/coords.h"
#include "ui/layout.h"
#include "util/base.h"
#include "util/vec.h"
#include "ws/finder.h"

typedef struct Ed Ed;
typedef struct Grid Grid;
typedef struct Win Win;

enum {
    YEW_COMPL_ROWS = 10,
    YEW_COMPL_LABEL_W = 44,
    YEW_COMPL_DETAIL_W = 12,
    YEW_COMPL_W = YEW_COMPL_LABEL_W + YEW_COMPL_DETAIL_W
};

/* The first five values deliberately match SymKind so the local symbol
 * source remains a zero-copy producer.  Protocol sources extend the visual
 * vocabulary without widening SymKind's ranking tables. */
typedef enum ComplKind {
    YEW_COMPLK_WORD = 0,
    YEW_COMPLK_FUNC,
    YEW_COMPLK_TYPE,
    YEW_COMPLK_MACRO,
    YEW_COMPLK_KEYWORD,
    YEW_COMPLK_VARIABLE,
    YEW_COMPLK_CONSTANT,
    YEW_COMPLK_FIELD,
    YEW_COMPLK_ENUM,
    YEW_COMPLK_MODULE,
    YEW_COMPLK_SNIPPET,
    YEW_COMPLK_NKIND
} ComplKind;

typedef struct ComplItem {
    const u8 *label;
    u32 label_len;
    const u8 *insert;
    u32 insert_len;
    const u8 *detail;
    u32 detail_len;
    const u8 *doc;
    u32 doc_len;
    u8 kind;
    i32 score;
    FzMatch m;
    void *user;
} ComplItem;

VEC_DECL(Vec_ComplItem, ComplItem);

enum {
    /* The source starts work from enumerate() and supplies rows later with
     * yew_compl_push(). */
    YEW_COMPL_SRC_ASYNC = 1U << 0,
    /* Protocol-backed sources may complete at punctuation or an empty gap. */
    YEW_COMPL_SRC_EMPTY_STEM = 1U << 1
};

typedef struct ComplSource {
    const char *name;
    u32 flags;
    u32 (*enumerate)(Ed *ed, Win *w, const u8 *stem, u32 slen,
                     Vec_ComplItem *out, void *ctx);
    void (*resolve)(Ed *ed, Win *w, ComplItem *item, void *ctx);
    /* A source-specific accept owns its complete atomic edit. */
    bool (*accept)(Ed *ed, Win *w, Span replace, const ComplItem *item,
                   void *ctx);
    /* Called exactly once for every item transferred into the menu. */
    void (*discard)(ComplItem *item, void *ctx);
    /* Cancel pending source work; item disposal follows this callback. */
    void (*close)(Ed *ed, Win *w, void *ctx);
    void *ctx;
} ComplSource;

typedef struct ComplMenu {
    bool open;
    Vec_ComplItem items;
    i32 sel;
    u32 top;
    Span replace;
    u64 buf_gen;
    Rect box;
    Rect panel;
    bool panel_open;
    const ComplSource *src;
    /* Opaque request slots for asynchronous sources.  Core never interprets
     * them; the owning source cancels them from close() or before a refill. */
    u64 source_request;
    u64 source_resolve;
    u64 source_seq;
    u32 source_server;
} ComplMenu;

extern const ComplSource yew_compl_source_index;

/* Sprint 43 compatibility: these two calls retain the arbitration-only
 * surface used by older consumers.  New completion sources use the explicit
 * variants below. */
void yew_compl_open(Ed *ed, Win *w);
void yew_compl_close(Ed *ed, Win *w);
bool yew_compl_open_source(Ed *ed, Win *w, const ComplSource *src);
void yew_compl_close_result(Ed *ed, Win *w, bool accepted);
void yew_compl_push(Ed *ed, Win *w, const ComplItem *it, u32 n);
bool yew_compl_key(Ed *ed, Win *w, const Key *k);
void yew_compl_after_key(Ed *ed, Win *w);
bool yew_compl_maybe_auto_trigger(Ed *ed, Win *w);
void yew_compl_resize(Ed *ed, Win *w);
void yew_compl_draw(Ed *ed, Win *w, Grid *g);
void yew_compl_free(ComplMenu *menu);
void yew_compl_select(Ed *ed, Win *w, i32 item);

CmdStatus yew_compl_cmd_open(CmdCtx *cx);
CmdStatus yew_compl_cmd_next(CmdCtx *cx);
CmdStatus yew_compl_cmd_prev(CmdCtx *cx);
CmdStatus yew_compl_cmd_page_next(CmdCtx *cx);
CmdStatus yew_compl_cmd_page_prev(CmdCtx *cx);
CmdStatus yew_compl_cmd_accept(CmdCtx *cx);
CmdStatus yew_compl_cmd_cancel(CmdCtx *cx);
CmdStatus yew_compl_cmd_doc_toggle(CmdCtx *cx);
CmdStatus yew_compl_cmd_stats(CmdCtx *cx);
CmdStatus yew_compl_cmd_reindex(CmdCtx *cx);

#endif /* YEW_UI_COMPLMENU_H */
