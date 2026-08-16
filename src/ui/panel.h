#ifndef YEW_UI_PANEL_H
#define YEW_UI_PANEL_H

#include <stdbool.h>

#include "edit/keymap.h"
#include "term/input.h"
#include "text/coords.h"
#include "ui/layout.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct Grid Grid;

enum {
    YEW_PANEL_MAX_W = 72,
    YEW_PANEL_MAX_H = 16
};

typedef enum PanelPlace {
    YEW_PANEL_BELOW = 0,
    YEW_PANEL_ABOVE,
    YEW_PANEL_CURSOR
} PanelPlace;

VEC_DECL(Vec_Span, Span);

typedef struct PanelSpec {
    const char *title;
    const u8 *body;
    u32 len;
    u16 x, y;
    u8 place;
    u16 max_w, max_h;
    const char *role;
    Vec_Span *emph;
} PanelSpec;

typedef struct Panel {
    bool open;
    Rect rect;
    u16 scroll;
    u32 nrows;
    Vec_Span rows;

    /* Owned input and placement state retained so resize is deterministic. */
    u8 *body;
    u32 len;
    char *title;
    char *role;
    Vec_Span emph;
    Keymap keys;
    u16 anchor_x, anchor_y;
    u16 max_w, max_h;
    u8 place;
} Panel;

bool yew_panel_open(Ed *ed, Panel *p, const PanelSpec *spec);
void yew_panel_close(Ed *ed, Panel *p);
void yew_panel_resize(Ed *ed, Panel *p);

/* True means the key was consumed.  Esc and every non-scroll key close the
 * panel and return false so the editor can dispatch that same key normally. */
bool yew_panel_key(Ed *ed, Panel *p, const Key *k);
void yew_panel_draw(Ed *ed, const Panel *p, Grid *g);

CmdStatus yew_panel_cmd_move(CmdCtx *cx);

#endif /* YEW_UI_PANEL_H */
