#define _POSIX_C_SOURCE 200809L

#include "ui/mouse.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "mod/git/fussmode.h"
#include "term/tty.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/complmenu.h"
#include "ui/ctxmenu.h"
#include "ui/ctxrows.h"
#include "ui/groupnav.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/panel.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"

/* ---------------------------------------------------------------- */
/* Small predicates                                                 */
/* ---------------------------------------------------------------- */

static bool is_wheel(u8 button)
{
    return button == (u8)YEW_MB_WHEEL_UP ||
           button == (u8)YEW_MB_WHEEL_DOWN ||
           button == (u8)YEW_MB_WHEEL_LEFT ||
           button == (u8)YEW_MB_WHEEL_RIGHT;
}

static i32 wheel_dir(u8 button)
{
    return (button == (u8)YEW_MB_WHEEL_UP ||
            button == (u8)YEW_MB_WHEEL_LEFT)
               ? -1
               : 1;
}

/*
 * Declared here because the WHEEL needs them and the wheel is routed
 * before §5 defines them: a wheel dismisses an open menu, and it
 * scrolls the FUSS drawer by geometry rather than by region.
 */
static void menu_close(Ed *ed);
static bool rect_has(Rect r, u16 x, u16 y);

void yew_mouse_init(MouseState *m)
{
    if (m == NULL)
        return;
    (void)memset(m, 0, sizeof(*m));
    m->drag_to_slot = -1;
}

/*
 * Ends a GESTURE without ending the multi-click run.
 *
 * The distinction matters: every click ends with a release, and a
 * double-click is two of them.  Zeroing the whole struct at release
 * would clear the counter the second click has to read, and no
 * double-click could ever be recognised — which is exactly the bug the
 * §6 tests caught.
 */
static void gesture_reset(MouseState *m)
{
    i64 last_ms = m->last_click_ms;
    u16 last_x = m->last_click_x;
    u16 last_y = m->last_click_y;
    RegionKind last_kind = m->last_click_kind;
    i32 last_payload = m->last_click_payload;
    u8 clicks = m->click_n;

    yew_mouse_init(m);
    m->last_click_ms = last_ms;
    m->last_click_x = last_x;
    m->last_click_y = last_y;
    m->last_click_kind = last_kind;
    m->last_click_payload = last_payload;
    m->click_n = clicks;
}

bool yew_mouse_gesture_active(const Ed *ed)
{
    return ed != NULL && ed->mouse.phase != YEW_MP_IDLE;
}

/* ---------------------------------------------------------------- */
/* Sprint 18.5 §8: the completion menu's first refusal              */
/* ---------------------------------------------------------------- */

/*
 * Gated on cmdline.active because YEW_REGION_BLOCK is not the menu's
 * alone — Sprint 24's group picker registers one over its own rectangle
 * for the same swallow-the-gap reason.  The two overlays are never open
 * at once, so the prompt's own state is what disambiguates them.
 *
 * Sprint 27 moved this out of ed.c: DoD 2 is that no mouse event
 * becomes an action anywhere but this file.
 */
bool yew_mouse_claimed_by_menu(Ed *ed, Key key)
{
    Region hit;

    if (ed == NULL || key.kind != (u16)YEW_EV_MOUSE)
        return false;
    hit = yew_region_hit(key.col, key.row);
    if (ed->win != NULL && ed->win->compl.open) {
        if (hit.kind != YEW_REGION_COMPL_ROW &&
            hit.kind != YEW_REGION_BLOCK)
            return false;
        if (key.button == (u8)YEW_MB_WHEEL_UP ||
            key.button == (u8)YEW_MB_WHEEL_DOWN) {
            i32 next = ed->win->compl.sel +
                       (key.button == (u8)YEW_MB_WHEEL_UP ?
                            -YEW_WHEEL_ROWS : YEW_WHEEL_ROWS);

            if (next < 0)
                next = 0;
            if ((size_t)next >= ed->win->compl.items.len)
                next = ed->win->compl.items.len == 0U ? -1 :
                       (i32)ed->win->compl.items.len - 1;
            yew_compl_select(ed, ed->win, next);
            return true;
        }
        if (key.button != (u8)YEW_MB_LEFT)
            return false;
        if (key.ev == YEW_KEY_PRESS && hit.kind == YEW_REGION_COMPL_ROW)
            yew_compl_select(ed, ed->win, hit.payload);
        return true;
    }
    if (!ed->cmdline.active)
        return false;
    if (hit.kind != YEW_REGION_MENU_ROW && hit.kind != YEW_REGION_BLOCK)
        return false;
    if (key.button == (u8)YEW_MB_WHEEL_UP ||
        key.button == (u8)YEW_MB_WHEEL_DOWN) {
        /* Wheeling over the menu scrolls the LIST and deliberately does
         * not move the selection: looking is not choosing. */
        (void)yew_cmdline_menu_scroll(
            ed, key.button == (u8)YEW_MB_WHEEL_UP ? -YEW_WHEEL_ROWS
                                                  : YEW_WHEEL_ROWS);
        return true;
    }
    if (key.button != (u8)YEW_MB_LEFT)
        return false;
    /* Swallow the release of a press we handled, so it cannot fall
     * through to whatever is underneath. */
    if (key.ev != YEW_KEY_PRESS)
        return true;
    if (hit.kind == YEW_REGION_MENU_ROW)
        (void)yew_cmdline_menu_click(ed, hit.payload);
    /* A BLOCK hit is inert by construction: it exists so a click on a
     * gap in the menu does not land in the pane underneath it. */
    return true;
}

/* ---------------------------------------------------------------- */
/* §3: the wheel                                                    */
/* ---------------------------------------------------------------- */

/*
 * How far right the view may travel.  There is no global "longest
 * line" — computing one would be a full-buffer scan per notch — so the
 * bound is the widest line CURRENTLY IN VIEW.  Scrolling into empty
 * space past the end of every visible line is the one horizontal-scroll
 * misbehaviour users notice, because the text simply vanishes and no
 * key brings it back except Home.
 */
static u64 hscroll_limit(Win *w)
{
    TextBuf *tb;
    u64 widest = 0U;
    u16 row;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return 0U;
    tb = w->buf->tb;
    for (row = 0U; row < w->vp.rows; row++) {
        LineNo line;
        u32 sub = 0U;
        Span span;
        CCol end;

        if (!yew_vp_line_of_row(w, row, &line, &sub))
            break;
        span = yew_textbuf_line_span(tb, line);
        end = yew_off_to_ccol(tb, span, BYTEOFF(span.hi), YEW_VP_TABWIDTH);
        if (end.v > widest)
            widest = end.v;
    }
    return widest;
}

/*
 * Shift+wheel.  A no-op when wrap is on — there is nowhere to go — and
 * silent, because a wheel is not a command that can fail and a message
 * per notch would be noise.
 */
static void wheel_horizontal(Win *w, i32 cells)
{
    i64 want;
    u64 limit;

    if (w == NULL || w->vp.wrap)
        return;
    want = (i64)w->vp.left.v + cells;
    if (want < 0)
        want = 0;
    limit = hscroll_limit(w);
    if ((u64)want > limit)
        want = (i64)limit;
    w->vp.left = (CCol){(u64)want};
}

/*
 * Wheel over a pane scrolls THAT pane, whether or not it is focused,
 * and moves neither the focus nor the cursor.
 *
 * Two panes side by side exist to be compared.  If the wheel followed
 * focus you would have to click the other pane first — and clicking
 * moves the cursor, so reading a second file would mutate your editing
 * position.  A scroll must never change document state.
 *
 * PITFALL, and the reason the scrolloff-restoring viewport call is
 * absent from this whole file — DoD 5 greps for its name, so this
 * comment describes it rather than spelling it.  That call drags the
 * viewport back around the cursor, which is exactly where it already
 * was; after a wheel it would make an unfocused pane appear not to
 * scroll at all, and a focused one snap back.  yew_vp_scroll already
 * clamps, which is all a wheel needs.  Scrolloff resumes at the next
 * cursor motion, as always.
 */
static void wheel_pane(Ed *ed, const Region *hit, const Key *k)
{
    Pane *leaf = yew_pane_leaf_by_index(ed, hit->payload);
    Win *w = leaf != NULL ? leaf->win : NULL;
    i32 dir = wheel_dir(k->button);

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    if ((k->mods & (u16)YEW_MOD_SHIFT) != 0U ||
        k->button == (u8)YEW_MB_WHEEL_LEFT ||
        k->button == (u8)YEW_MB_WHEEL_RIGHT)
        wheel_horizontal(w, dir * YEW_WHEEL_COLS);
    else
        yew_vp_scroll(w, dir * YEW_WHEEL_ROWS);
    ed->full_damage = true;
}

/* Row 1 or row 2?  The strip rect is registered state, not a
 * re-derivation of the layout — see region.h's law. */
static bool region_is_member_row(const Ed *ed, const Region *hit)
{
    return ed->tab_strip_rect.h >= 2U &&
           hit->rect.y == (u16)(ed->tab_strip_rect.y + 1U);
}

static void strip_scroll(Ed *ed, bool row2, i32 delta)
{
    int *scroll = row2 ? &ed->tabs.member_scroll : &ed->tabs.scroll;
    int limit = (int)ed->tabs.v.len;
    int to = *scroll + delta;

    if (row2)
        limit = yew_group_member_count(ed, yew_active_group_id(ed));
    if (to < 0)
        to = 0;
    if (to >= limit)
        to = limit > 0 ? limit - 1 : 0;
    if (*scroll == to)
        return;
    *scroll = to;
    ed->full_damage = true;
}

static void mouse_wheel(Ed *ed, const Key *k)
{
    Region hit;

    /*
     * Ctrl+wheel is DELIBERATELY unbound: it is the terminal emulator's
     * font-size gesture, and stealing it would fight the host
     * application over a key the user does not think of as ours.
     */
    if ((k->mods & (u16)YEW_MOD_CTRL) != 0U)
        return;
    /*
     * A wheel DISMISSES an open menu and is then routed normally.
     * Scrolling the thing under a pop-up while the pop-up stays put is
     * how a menu ends up pointing at a row that has moved; closing
     * first keeps the capture-at-open law honest.
     */
    if (yew_ctx_active())
        menu_close(ed);
    if (yew_mouse_claimed_by_menu(ed, *k))
        return;
    /*
     * 57.11 §3: THE WHEEL SCROLLS THE TREE, over its rows and over the
     * blank drawer below them alike — a list that only scrolled where
     * it happened to have drawn a row would feel like it had holes.
     */
    if (yew_fuss_active(ed) &&
        rect_has(yew_fuss_drawer_rect(ed), k->col, k->row)) {
        yew_fuss_scroll(ed, wheel_dir(k->button) * YEW_WHEEL_ROWS);
        return;
    }
    hit = yew_region_hit(k->col, k->row);
    switch (hit.kind) {
    case YEW_REGION_PANE:
        wheel_pane(ed, &hit, k);
        break;
    case YEW_REGION_TAB:
        strip_scroll(ed, region_is_member_row(ed, &hit), wheel_dir(k->button));
        break;
    case YEW_REGION_TAB_SCROLL:
        strip_scroll(ed, hit.payload == 2 || hit.payload == -2,
                     wheel_dir(k->button));
        break;
    case YEW_REGION_PICK_ROW:
        yew_picker_scroll(ed, wheel_dir(k->button) * YEW_WHEEL_ROWS);
        break;
    case YEW_REGION_GP_ROW:
    case YEW_REGION_GP_NAME:
        yew_gp_scroll(ed, wheel_dir(k->button) * YEW_WHEEL_ROWS);
        break;
    /*
     * A wheel over a pane BORDER, a context menu, or a modal's own
     * BLOCK does nothing.  Swallowed rather than passed through: the
     * whole point of BLOCK is that the document underneath is inert.
     */
    case YEW_REGION_PANE_BORDER:
    case YEW_REGION_TAB_NEW:
    case YEW_REGION_CTX_ROW:
    case YEW_REGION_BLOCK:
    case YEW_REGION_MENU_ROW:
    case YEW_REGION_COMPL_ROW:
    case YEW_REGION_NONE:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §5: the context menus                                            */
/* ---------------------------------------------------------------- */

/*
 * The rows are OPAQUE ACTIONS to ctxmenu.c, which is what lets that
 * module stay below the editor in the dependency graph.  Their meaning
 * is `yew_ctx_actions` in ui/ctxrows.h, and the row sets themselves are
 * built in ui/ctxrows.c: THIS FILE MAY NOT ALLOCATE — tests/perf/mouse.c
 * reads its source and fails on any allocation call it finds — and a
 * row set is strings and a copied path.
 */

/* Where a menu may be placed: everything above the footer. */
static Rect menu_allowed(const Ed *ed)
{
    u16 rows = ed->grid.rows;

    if (rows > 1U)
        rows = (u16)(rows - 1U); /* the footer keeps its row */
    return (Rect){0U, 0U, ed->grid.cols, rows};
}

/*
 * Sprint 57.11 §6: the menu's look, from the theme's `menu.*` roles.
 *
 * A UI role is an OVERLAY, not a replacement — the same idiom as the
 * tab strip's `tab_role_style`: a foreground-only role keeps the menu
 * surface behind it, a background-only one keeps readable text, and
 * presence owns the attributes so `mono: "plain"` can deliberately
 * clear a colourful rendition's emphasis.  With no theme loaded every
 * role falls back to the editor's `fg`/`bg` and to attributes alone —
 * reverse for the hovered row, dim for greyed rows, accelerators and
 * rules — so the menu is legible before any theme is, and under
 * NO_COLOR.
 */
static ThemeEnt menu_role_style(const Ed *ed, const char *role,
                                ThemeEnt fallback)
{
    const ThemeEnt *themed = yew_theme_ui_tab(ed, role);

    if (themed == NULL)
        return fallback;
    if (themed->fg.tag != YEW_COLOR_DEFAULT)
        fallback.fg = themed->fg;
    if (themed->bg.tag != YEW_COLOR_DEFAULT)
        fallback.bg = themed->bg;
    fallback.attrs = themed->attrs;
    return fallback;
}

static ThemeEnt menu_base_style(const Ed *ed)
{
    const ThemeEnt *fg = yew_theme_ui_tab(ed, "fg");
    const ThemeEnt *bg = yew_theme_ui_tab(ed, "bg");
    ThemeEnt style = {
        {YEW_COLOR_DEFAULT, 0U, 0U, 0U},
        {YEW_COLOR_DEFAULT, 0U, 0U, 0U},
        0U
    };

    if (fg != NULL && fg->fg.tag != YEW_COLOR_DEFAULT)
        style.fg = fg->fg;
    if (bg != NULL && bg->bg.tag != YEW_COLOR_DEFAULT)
        style.bg = bg->bg;
    return style;
}

static ThemeEnt menu_emphasis(ThemeEnt base, u16 attrs)
{
    base.attrs = (u16)(base.attrs | attrs);
    return base;
}

static CtxStyle menu_style(const Ed *ed)
{
    CtxStyle s;

    s.surface = menu_role_style(ed, "menu.surface", menu_base_style(ed));
    s.row = menu_role_style(ed, "menu.row", s.surface);
    s.hover = menu_role_style(ed, "menu.hover",
                              menu_emphasis(s.surface, YEW_ATTR_REVERSE));
    s.disabled = menu_role_style(ed, "menu.disabled",
                                 menu_emphasis(s.surface, YEW_ATTR_DIM));
    s.accel = menu_role_style(ed, "menu.accel",
                              menu_emphasis(s.surface, YEW_ATTR_DIM));
    s.sep = menu_role_style(ed, "menu.sep",
                            menu_emphasis(s.surface, YEW_ATTR_DIM));
    return s;
}

/* ---------------------------------------------------------------- */
/* Opening and closing                                              */
/* ---------------------------------------------------------------- */

/*
 * THE ONE CLOSE PATH.
 *
 * Any-motion reporting (DEC 1003) is armed only while a menu is up, and
 * "only" is a claim about EVERY way a menu can go away — a row fired, a
 * key, Esc, a click outside, a wheel, the mouse being disabled.  Five
 * close sites would be five chances to leave the terminal streaming
 * motion reports at an editor that has nothing to do with them, which
 * is invariant 6's failure mode in slow motion.  So there is one, and
 * `fuzz_mouse` asserts the flag is false whenever no menu is open.
 *
 * It is safe to call when nothing is open: `yew_ctx_close` is a memset
 * and `yew_tty_mouse_motion` is idempotent.
 */
static void menu_close(Ed *ed)
{
    yew_ctx_close();
    /* Disarmed HERE rather than inside yew_ctx_close() so the widget
     * stays editor- and terminal-ignorant. */
    yew_tty_mouse_motion(false);
    if (ed != NULL)
        ed->full_damage = true;
}

/*
 * The widget can also deactivate itself — Esc through `yew_ctx_key`, or
 * a row being chosen.  This is what keeps the 1003 claim true across
 * those paths without teaching ctxmenu.c about the terminal.
 */
static void menu_settle(Ed *ed)
{
    if (!yew_ctx_active())
        menu_close(ed);
}

/*
 * Opens the menu for `c` at the pointer.
 *
 * The clicked CELL is captured after the build, as the target rect: a
 * CtxContext carries the region's rectangle (the pane's, for DOC), and
 * a document row that places the cursor where the user pointed needs
 * the cell itself.  A 1x1 rect rather than two u16s because that is the
 * shape ctxmenu.c already stores.
 *
 * The ANCHOR is separate from that cell because the strip's menus hang
 * one row clear of the strip (Sprint 27's placement, and the goldens'):
 * everywhere else the two are the same cell.
 */
static bool menu_open_at(Ed *ed, const CtxContext *c, u16 anchor_x,
                        u16 anchor_y, u16 cell_x, u16 cell_y)
{
    menu_close(ed);
    yew_ctx_build(ed, c);
    yew_ctx_target_rect((Rect){cell_x, cell_y, 1U, 1U});
    if (!yew_ctx_show(anchor_x, anchor_y, menu_allowed(ed))) {
        /* Nothing opened: the build refused (its target went away) or
         * the box cannot fit even its priority-0 rows.  The close above
         * already left 1003 disarmed; say so explicitly rather than
         * relying on the reader to remember. */
        menu_close(ed);
        return false;
    }
    yew_tty_mouse_motion(true);
    ed->full_damage = true;
    return true;
}

bool yew_mouse_open_tab_menu(Ed *ed, u32 tab_id, u16 x, u16 y)
{
    CtxContext c;

    if (ed == NULL)
        return false;
    (void)memset(&c, 0, sizeof(c));
    c.kind = YEW_CTX_KIND_TAB;
    c.id = tab_id;
    return menu_open_at(ed, &c, x, y, x, y);
}

bool yew_mouse_open_group_menu(Ed *ed, u32 gid, u16 x, u16 y)
{
    CtxContext c;

    if (ed == NULL)
        return false;
    (void)memset(&c, 0, sizeof(c));
    c.kind = YEW_CTX_KIND_GROUP;
    c.id = gid;
    return menu_open_at(ed, &c, x, y, x, y);
}

/* ---------------------------------------------------------------- */
/* §3: what is under the pointer                                    */
/* ---------------------------------------------------------------- */

static bool rect_has(Rect r, u16 x, u16 y)
{
    return r.w != 0U && r.h != 0U && x >= r.x &&
           x < (u16)(r.x + r.w) && y >= r.y && y < (u16)(r.y + r.h);
}

/*
 * ONE PURE FUNCTION, unit-tested directly.
 *
 * Every surface's menu comes from here, so "right-click opens the wrong
 * menu over the FUSS drawer" is a test rather than a bug report.  It
 * reads the region table and the layout rectangles and nothing else:
 * no state changes, no allocation, and the same answer every time it is
 * asked about the same frame (invariant 5).
 *
 * A REGION HIT WINS, because the renderer registered it in the same
 * statement that drew it (Sprint 22's law).  Only a NONE hit falls
 * through to geometry, and then in the order the sprint fixes: the FUSS
 * drawer and its backdrop, the footer, the tab strip, the editor.  The
 * order matters where the rectangles overlap — a fullscreen drawer
 * covers cells the strip rectangle also claims, and the drawer is what
 * the user sees there.
 */
CtxContext yew_mouse_context_at(const Ed *ed, u16 x, u16 y)
{
    CtxContext c;
    Region hit;

    (void)memset(&c, 0, sizeof(c));
    c.kind = YEW_CTX_KIND_EDITOR;
    if (ed == NULL)
        return c;
    hit = yew_region_hit(x, y);
    c.payload = hit.payload;
    c.rect = hit.rect;
    /*
     * THE OPEN MENU ANSWERS FOR ITS OWN CELLS.  Checked before the
     * region kinds because the menu's BLOCK covers its border and its
     * gaps, and a right-click there must mean "the menu", not "whatever
     * the box is sitting on top of".  NONE is the answer: there is no
     * menu to open for a menu, and the router's close-and-reopen rule
     * (§3) is what actually handles the press.
     */
    if (yew_ctx_active() && rect_has(yew_ctx_box(), x, y)) {
        c.kind = YEW_CTX_KIND_NONE;
        c.rect = yew_ctx_box();
        return c;
    }
    switch (hit.kind) {
    case YEW_REGION_PANE:
        c.kind = YEW_CTX_KIND_DOC;
        c.id = (u32)hit.payload; /* the leaf index, for CTX_TGT_PANE */
        return c;
    case YEW_REGION_PANE_BORDER:
        c.kind = YEW_CTX_KIND_BORDER;
        c.id = (u32)hit.payload;
        return c;
    case YEW_REGION_TAB:
        /* The row-1 payload convention, once, here: >= 0 is a tab
         * index and < 0 is a negated gid (ui/region.h). */
        if (hit.payload < 0) {
            c.kind = YEW_CTX_KIND_GROUP;
            c.id = (u32)(-hit.payload);
        } else {
            const Tab *t = yew_tab_at_const(ed, hit.payload);

            /* IDENTITY, not the index: the strip can scroll and tabs
             * can close before a row fires. */
            c.kind = YEW_CTX_KIND_TAB;
            c.id = t != NULL ? t->tab_id : 0U;
        }
        return c;
    case YEW_REGION_TAB_SCROLL:
    case YEW_REGION_TAB_NEW:
        c.kind = YEW_CTX_KIND_STRIP;
        return c;
    case YEW_REGION_FUSS_ROW: {
        bool is_dir = false;

        c.id = (u32)hit.payload; /* the interned path id */
        /*
         * FILE or DIRECTORY is the tree's to say, not the payload's:
         * the two menus differ and a guess from the path text would be
         * wrong for an extensionless file and for a directory the tree
         * has not walked.  Unknown — a stripped FUSS, a path the tree
         * no longer holds — is neither, and the blank-drawer menu is
         * the honest answer.
         */
        if (!yew_fuss_path_is_dir(ed, (u32)hit.payload, &is_dir))
            c.kind = YEW_CTX_KIND_FUSS_BLANK;
        else
            c.kind = is_dir ? YEW_CTX_KIND_FUSS_DIR
                            : YEW_CTX_KIND_FUSS_FILE;
        return c;
    }
    case YEW_REGION_PICK_ROW:
        c.kind = YEW_CTX_KIND_PICK_ROW;
        c.id = (u32)hit.payload; /* the item payload, for CTX_TGT_PICK */
        return c;
    case YEW_REGION_COMPL_ROW:
        c.kind = YEW_CTX_KIND_COMPL_ROW;
        c.id = (u32)hit.payload;
        return c;
    case YEW_REGION_GP_ROW:
        c.kind = YEW_CTX_KIND_GP_ROW;
        c.id = (u32)hit.payload;
        return c;
    case YEW_REGION_GP_NAME:
        c.kind = YEW_CTX_KIND_GP;
        return c;
    case YEW_REGION_CTX_ROW:
        /* Handled by the box test above; reachable only if a stale row
         * region outlived its menu, and then it means nothing. */
        c.kind = YEW_CTX_KIND_NONE;
        return c;
    case YEW_REGION_BLOCK:
        /*
         * BLOCK is INERT BY DESIGN and shared by every modal, so which
         * one it belongs to is decided by what is open — innermost
         * first.  The panel additionally has to contain the cell,
         * because it is the one overlay that can be open beside
         * another.
         */
        if (ed->win != NULL && ed->win->panel.open &&
            rect_has(ed->win->panel.rect, x, y)) {
            c.kind = YEW_CTX_KIND_PANEL;
            return c;
        }
        if (yew_picker_active(ed)) {
            c.kind = YEW_CTX_KIND_PICKER;
            return c;
        }
        if (yew_gp_active()) {
            c.kind = YEW_CTX_KIND_GP;
            return c;
        }
        if (ed->win != NULL && ed->win->compl.open) {
            c.kind = YEW_CTX_KIND_COMPL_ROW;
            return c;
        }
        c.kind = YEW_CTX_KIND_EDITOR;
        return c;
    case YEW_REGION_MENU_ROW:
    case YEW_REGION_NONE:
    default:
        break;
    }
    /*
     * GEOMETRY, only for a cell no region claimed.  Each rectangle is
     * the one the layout computed, so "the pointer is in the footer"
     * cannot drift from where the footer was drawn.  A stripped FUSS
     * returns zero rectangles from the shim and falls straight through.
     */
    if (yew_fuss_active(ed)) {
        /*
         * GATED ON F MODE BEING UP, because the drawer's rectangle is
         * computed from the terminal width alone and is non-empty even
         * when nothing is drawn in it — an ungated read would claim the
         * left quarter of every screen for a drawer that is not there.
         */
        c.rect = yew_fuss_drawer_rect(ed);
        if (rect_has(c.rect, x, y)) {
            c.kind = YEW_CTX_KIND_FUSS_BLANK;
            return c;
        }
        c.rect = yew_fuss_backdrop_rect(ed);
        if (rect_has(c.rect, x, y)) {
            c.kind = YEW_CTX_KIND_FUSS_BLANK;
            return c;
        }
    }
    if (rect_has(ed->footer_rect, x, y)) {
        c.kind = YEW_CTX_KIND_FOOTER;
        c.rect = ed->footer_rect;
        return c;
    }
    if (rect_has(ed->tab_strip_rect, x, y)) {
        c.kind = YEW_CTX_KIND_STRIP;
        c.rect = ed->tab_strip_rect;
        return c;
    }
    c.kind = YEW_CTX_KIND_EDITOR;
    c.rect = (Rect){0U, 0U, ed->grid.cols, ed->grid.rows};
    return c;
}

static void invoke_mouse_named(Ed *ed, const char *name)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, strlen(name));

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = YEW_SRC_MOUSE;
    (void)yew_ed_invoke(ed, id, &cx);
}

static void invoke_fuss_open(Ed *ed, i32 payload)
{
    CmdCtx cx = {0};
    CmdId id;
    const char *path;
    size_t path_len;

    if (ed == NULL || payload <= 0)
        return;
    path = yew_intern_str(&ed->interner, (u32)payload);
    path_len = yew_intern_len(&ed->interner, (u32)payload);
    if (path == NULL || path_len == 0U || path_len > UINT32_MAX)
        return;
    id = yew_cmd_lookup("ed.git.open", (u32)(sizeof("ed.git.open") - 1U));
    if (id.v == 0U)
        return;
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.sarg = path;
    cx.sarg_len = (u32)path_len;
    cx.source = YEW_SRC_MOUSE;
    (void)yew_ed_invoke(ed, id, &cx);
}

/*
 * Puts the captured target in place, and says whether it is still
 * there.  False means the thing the menu was opened on is gone — a tab
 * closed by a job, a group dissolved by a script — and then NOTHING
 * runs: a row that fired against whatever inherited the identity would
 * act on a file the user never pointed at.
 */
static bool apply_target(Ed *ed, const CtxActionDesc *d, u32 id, Rect cell)
{
    switch (d->target) {
    case CTX_TGT_TAB: {
        /*
         * The target becomes active first, resolved from its ID.  A
         * right-click on a tab is an act of pointing at it, so acting
         * on it is what the user asked for — and it means the rows can
         * be the ordinary registry commands rather than a second
         * implementation that takes a tab argument.
         */
        int idx = yew_tab_index_of_id(ed, id);

        if (idx < 0)
            return false;
        yew_tab_switch(ed, idx);
        return true;
    }
    case CTX_TGT_GROUP:
        if (yew_group_at(ed, id) == NULL)
            return false;
        if (yew_active_group_id(ed) != id)
            yew_group_enter(ed, id);
        return true;
    case CTX_TGT_PANE:
    case CTX_TGT_LEAF: {
        Pane *leaf = yew_pane_leaf_by_index(ed, (i32)id);

        if (leaf == NULL)
            return false;
        /*
         * yew_pane_click is the keyboard-free route to the same place
         * and CANNOT be used here: it re-resolves the leaf from the
         * region table, which is frozen, and asking it anything while
         * frozen aborts.  The leaf index and the clicked cell were both
         * captured at open time precisely so this path needs neither.
         */
        yew_pane_refocus(ed, leaf);
        /*
         * CTX_TGT_LEAF STOPS HERE.  Placing the caret sets anchor =
         * pos, which collapses the selection — so the rows that exist
         * to act on a selection (`Cut`, `Copy`, `Delete`) would run on
         * an empty one.  The rows that mean "here" take CTX_TGT_PANE
         * and get the cell (ui/ctxrows.h).
         */
        if (d->target == CTX_TGT_PANE && leaf->win != NULL &&
            leaf->win->buf != NULL)
            yew_win_click_to_cursor(leaf->win, cell.x, cell.y);
        return true;
    }
    case CTX_TGT_PICK:
        if (!yew_picker_active(ed))
            return false;
        yew_picker_select_payload(ed, (i32)id);
        /* The accept IS the action for a picker row, and `iarg` is the
         * accept MODE (here / vsplit / hsplit) — the same three the
         * keyboard has. */
        return yew_picker_accept_how(ed, (u8)d->iarg);
    case CTX_TGT_COMPL:
        if (ed->win == NULL || !ed->win->compl.open)
            return false;
        yew_compl_select(ed, ed->win, (i32)id);
        return true;
    case CTX_TGT_GP_ROW:
        /*
         * The captured id is the group dialog's LISTING INDEX, and the
         * row it named can be gone — the dialog re-lists on every walk
         * — so a failed select means NOTHING RUNS.  `Toggle` against a
         * re-listed directory would tick whatever file inherited the
         * index, which is the one mistake a tick set cannot survive.
         */
        return yew_gp_select_row(ed, (int)id);
    case CTX_TGT_PATH:
    case CTX_TGT_FUSS_ROW:
        /*
         * The path travels as `cx.sarg` (CTX_TGT_PATH), and the FUSS
         * SELECTION MOVES TO IT FIRST — for the rows whose command
         * reads the selection rather than an argument (`Expand` /
         * `Collapse`, `Open as Group...`), and because a tree that
         * still points somewhere else after a menu acted on this row
         * would leave the next keystroke operating on a different file.
         * A no-op when F mode is down or the build has no FUSS.
         */
        yew_fuss_select_path(ed, id);
        return true;
    case CTX_TGT_NONE:
    default:
        return true;
    }
}

/*
 * The rows that close an overlay.
 *
 * No registry command owns "make this transient thing go away", and
 * inventing four whose only caller is a menu row would be four more
 * names in the palette for something the Esc key already does.  WHICH
 * overlay is not guessed from what happens to be open: it is the MENU'S
 * OWN KIND, captured when the pointer was over it.
 */
static void close_overlay_for(Ed *ed, u32 kind)
{
    switch ((CtxKind)kind) {
    case YEW_CTX_KIND_PANEL:
        if (ed->win != NULL && ed->win->panel.open)
            yew_panel_close(ed, &ed->win->panel);
        break;
    case YEW_CTX_KIND_PICK_ROW:
    case YEW_CTX_KIND_PICKER:
        if (yew_picker_active(ed))
            yew_picker_close(ed, false);
        break;
    case YEW_CTX_KIND_COMPL_ROW:
        if (ed->win != NULL && ed->win->compl.open)
            yew_compl_close(ed, ed->win);
        break;
    case YEW_CTX_KIND_GP_ROW:
    case YEW_CTX_KIND_GP:
        if (yew_gp_active())
            yew_gp_close(ed);
        break;
    default:
        break;
    }
}

static void invoke_desc(Ed *ed, const CtxActionDesc *d, const char *path)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(d->cmd, strlen(d->cmd));

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = d->iarg;
    /*
     * YEW_SRC_MOUSE, and Sprint 27's YEW_SRC_KEY here was simply wrong:
     * a command that asks where it came from — the recorder, a policy
     * gate — was told a menu row was a keystroke.
     */
    cx.source = YEW_SRC_MOUSE;
    if (d->target == CTX_TGT_PATH) {
        size_t n = path != NULL ? strlen(path) : 0U;

        if (n == 0U || n > UINT32_MAX)
            return; /* a path-addressed row with no path does nothing */
        cx.sarg = path;
        cx.sarg_len = (u32)n;
    }
    (void)yew_ed_invoke(ed, id, &cx);
}

/*
 * Runs whatever row was chosen, against the target the menu captured.
 *
 * TABLE-DRIVEN (57.11 §3).  Sprint 27 spent a switch per menu kind on
 * this, so each new kind grew a second switch somewhere else and the
 * two drifted; a row's meaning is now the data in `yew_ctx_actions` and
 * this is its only reader.
 *
 * The region table is FROZEN for the duration: a row handler that
 * reached for a payload would be re-resolving the target from cells
 * that may since have come to mean a different file, and freezing turns
 * that from a rule into an abort (ui/region.h).  IT IS UNFROZEN ON
 * EVERY PATH OUT — `fuzz_mouse` asserts the table is thawed before each
 * event, so a single early return between the two is a fuzz failure.
 */
static void apply_menu_action(Ed *ed)
{
    u32 action = yew_ctx_take();
    u32 kind = yew_ctx_kind();
    u32 target = yew_ctx_target_id();
    const char *path = yew_ctx_target_path();
    Rect cell = yew_ctx_target_rect_get();
    const CtxActionDesc *d;

    if (action == (u32)CTXA_NONE || action >= (u32)CTXA__N)
        return;
    d = &yew_ctx_actions[action];
    yew_region_freeze(true);
    if (apply_target(ed, d, target, cell)) {
        if (d->cmd != NULL)
            invoke_desc(ed, d, path);
        else if (d->target == CTX_TGT_NONE)
            close_overlay_for(ed, kind);
    }
    yew_region_freeze(false);
    ed->layout_dirty = true;
    ed->full_damage = true;
}

/* The menu is a keymap layer: it takes the key before any mode does,
 * and swallows what it does not use. */
bool yew_mouse_menu_key(Ed *ed, const Key *k)
{
    if (ed == NULL || !yew_ctx_active())
        return false;
    if (!yew_ctx_key(k))
        return false;
    apply_menu_action(ed);
    /* Esc and Enter both leave the widget inactive; this is what makes
     * 1003 follow them out. */
    menu_settle(ed);
    ed->full_damage = true;
    return true;
}

void yew_mouse_menu_draw(Ed *ed)
{
    CtxStyle style;

    if (ed == NULL)
        return;
    /*
     * The menu's own registrations go first, because a hover repaint
     * does NOT run yew_region_frame_begin — that clears the table, and
     * the panes, the strip and the FUSS rows were not redrawn, so
     * clearing it would leave the frame with nothing but a menu to
     * click on.  Re-adding without removing is the other half of the
     * trap: 1000 motion reports would push the table past its 256-entry
     * ceiling and the drop warning would be the only symptom.
     *
     * CTX_ROW by kind, because the menu is the only owner of it.  The
     * BLOCK by RECT, because BLOCK is shared with every other modal and
     * removing the kind would take the picker's swallow-the-gap
     * rectangle with it.
     */
    yew_region_remove_kind(YEW_REGION_CTX_ROW);
    yew_region_remove_rect(YEW_REGION_BLOCK, yew_ctx_box());
    /* Resolved ONCE per draw, from the theme — the widget knows no
     * colours of its own (57.11 §6). */
    style = menu_style(ed);
    yew_ctx_draw(&ed->grid, &style);
}

/* ---------------------------------------------------------------- */
/* §2: press                                                        */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* §6: double and triple click, through the unit engines            */
/* ---------------------------------------------------------------- */

/*
 * WHY THE ENGINES AND NOT A LOCAL SCAN: so the mouse and the keyboard
 * agree on what a word is.  A double-click that selects `foo` where
 * `W`+`H`+`→` selects `foo.bar` is a bug users cannot name and always
 * feel.  Routing through yew_unit_word.span also makes double-click
 * correct for CJK (per-ideograph, WB999), ZWJ emoji, `don't`,
 * `1,000.50` and `foo_bar` FOR FREE, because Sprint 16 already fought
 * those battles and has the conformance corpus.
 */
static Span unit_span_at(Win *w, const UnitOps *u, ByteOff at, bool alt)
{
    UnitCtx uc;

    (void)memset(&uc, 0, sizeof(uc));
    uc.tb = w->buf->tb;
    uc.buf = w->buf;
    uc.win = w;
    return u->span(&uc, at, alt);
}

/*
 * The multi-click counter.
 *
 * Resets on a different CELL — a cell, not a pixel radius, because
 * cells are the unit of everything here and there is nothing to tune.
 * Four clicks wrap to one: never a surprise paragraph selection.
 */
static u8 click_advance(Ed *ed, const Key *k, const Region *hit)
{
    MouseState *m = &ed->mouse;
    bool within = m->click_n != 0U &&
                  ed->now_ms - m->last_click_ms < YEW_CLICK_MULTI_MS &&
                  m->last_click_x == k->col && m->last_click_y == k->row &&
                  hit != NULL && m->last_click_kind == hit->kind &&
                  m->last_click_payload == hit->payload;

    m->click_n = within ? (u8)(m->click_n % 3U + 1U) : 1U;
    m->last_click_ms = ed->now_ms;
    m->last_click_x = k->col;
    m->last_click_y = k->row;
    m->last_click_kind = hit == NULL ? YEW_REGION_NONE : hit->kind;
    m->last_click_payload = hit == NULL ? 0 : hit->payload;
    return m->click_n;
}

static void press_pane(Ed *ed, const Region *hit, const Key *k)
{
    MouseState *m = &ed->mouse;
    Pane *leaf = yew_pane_leaf_by_index(ed, hit->payload);
    Win *w;
    /* The SAME `alt` the keyboard's A-← / A-→ pass (s16 §1), so
     * Alt+double-click selects the whitespace-delimited WORD and
     * Alt+triple-click the whole display line. */
    bool alt = (k->mods & (u16)YEW_MOD_ALT) != 0U;
    u8 clicks = click_advance(ed, k, hit);
    ByteOff at;

    if (leaf == NULL)
        return;
    (void)yew_pane_click(ed, k->col, k->row);
    w = leaf->win;
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len == 0U)
        return;
    m->sel_alt = alt;
    at = w->cs.curs.data[w->cs.primary].pos;
    if (clicks == 1U) {
        /*
         * The anchor for a drag-select is the grapheme just clicked,
         * read back from the cursor rather than recomputed — so the
         * selection can never start one cluster away from where the
         * caret visibly landed.
         */
        m->sel_unit = &yew_unit_char;
        m->sel_anchor_span = (Span){at.v, at.v};
        return;
    }
    m->sel_unit = clicks == 2U ? &yew_unit_word : &yew_unit_line;
    m->sel_anchor_span = unit_span_at(w, m->sel_unit, at, alt);
    /*
     * H MODE WITH THE CORRESPONDING UNIT BORROWED — exactly the state
     * `H` plus the unit-mode key would produce.  Consequence: every
     * H-mode key works on a mouse selection, the selection→multi-cursor
     * lift (s17) works, and the recorder (s35) sees ordinary commands
     * rather than a mouse-shaped side door.
     */
    (void)yew_mode_enter_highlight(ed, clicks == 2U ? YEW_MODE_W
                                                    : YEW_MODE_L,
                                   false);
    {
        Cursor *c = &w->cs.curs.data[w->cs.primary];

        c->anchor = BYTEOFF(m->sel_anchor_span.lo);
        c->pos = BYTEOFF(m->sel_anchor_span.hi);
    }
    ed->full_damage = true;
}

/*
 * §6: middle-click paste, OFF by default.
 *
 * X11 primary-selection semantics cannot be implemented correctly from
 * a terminal, and OSC 52 is write-only in most of them — shipping the
 * half-thing silently would be worse than the option.  Sprint 36 owns
 * the model that persists this; the default here is what ships.
 */
static bool middle_paste_on;

bool yew_mouse_middle_paste(void)
{
    return middle_paste_on;
}

void yew_mouse_set_middle_paste(bool on)
{
    middle_paste_on = on;
}

static void press_middle(Ed *ed, const Region *hit, const Key *k)
{
    Pane *leaf;

    if (!middle_paste_on || hit->kind != YEW_REGION_PANE)
        return;
    leaf = yew_pane_leaf_by_index(ed, hit->payload);
    if (leaf == NULL || leaf->win == NULL)
        return;
    /* At the CLICK position, so the paste lands where the user pointed
     * rather than wherever the caret happened to be. */
    (void)yew_pane_click(ed, k->col, k->row);
    {
        /* ONE undo transaction, through Sprint 10's edit choke point,
         * so undo takes the whole paste back in a single step rather
         * than unpicking it line by line. */
        EditCtx ec = yew_ed_edit_ctx(ed);

        (void)yew_reg_paste(&ed->regs, &ec, (u8)'"', false,
                            YEW_VP_TABWIDTH);
        yew_ed_finish_edit(ed, &ec);
    }
    ed->full_damage = true;
}

static void press_border(Ed *ed, const Region *hit)
{
    Pane *split = yew_pane_split_by_index(ed, hit->payload);

    if (split == NULL)
        return;
    yew_pane_drag_begin(ed, split, ed->mouse.press_x, ed->mouse.press_y);
    ed->mouse.phase = YEW_MP_DRAG_BORDER;
}

/*
 * A press on a tab entry ARMS a drag and does nothing else visible.
 * The switch happens at release, when we know the gesture was a click:
 * switching on press and then dragging would leave a tab activated that
 * the user only meant to move.
 */
static void press_tab(Ed *ed, const Region *hit)
{
    ed->mouse.tab_count_at_press = yew_tab_count(ed);
    if (hit->payload < 0) {
        ed->mouse.drag_gid = (u32)(-hit->payload);
        ed->mouse.drag_tab_id = 0U;
    } else {
        Tab *t = yew_tab_at(ed, hit->payload);

        ed->mouse.drag_gid = 0U;
        ed->mouse.drag_tab_id = t != NULL ? t->tab_id : 0U;
    }
}

static void press_pick_row(Ed *ed, const Region *hit)
{
    /* Selecting is not accepting.  The accept happens at release, and
     * only when the release lands on the same row — a press that slid
     * off the row was a mis-aim, not a choice. */
    yew_picker_select_payload(ed, hit->payload);
}

/*
 * THE MENU GESTURE: the right button, or the left one with Ctrl.
 *
 * Ctrl+left exists because a trackpad's second button is a setting and
 * a single-button mouse has none, and because an editor whose context
 * menu needs hardware the user may not own has made the mouse a
 * requirement rather than an accelerator (invariant 9, from the other
 * side).
 */
static bool press_opens_menu(const Key *k)
{
    return k->button == (u8)YEW_MB_RIGHT ||
           (k->button == (u8)YEW_MB_LEFT &&
            (k->mods & (u16)YEW_MOD_CTRL) != 0U);
}

static void press_menu(Ed *ed, const Key *k)
{
    CtxContext c;
    u16 anchor_y;

    if (yew_ctx_active()) {
        /*
         * Resolved BEFORE the close, because `yew_mouse_context_at`
         * answers NONE for the open menu's own cells and that is how
         * "the press landed on the menu" is told apart from "the press
         * landed on what the menu was covering".
         */
        CtxKind over = yew_mouse_context_at(ed, k->col, k->row).kind;

        menu_close(ed);
        /* On the menu itself — a row, its border, a gap — the press
         * DISMISSES and stops.  Re-opening a menu on top of itself is
         * what every menu everywhere refuses to do, and invoking the
         * row under a right-click would be the worst reading of it. */
        if (over == YEW_CTX_KIND_NONE)
            return;
    }
    c = yew_mouse_context_at(ed, k->col, k->row);
    if (c.kind == YEW_CTX_KIND_NONE)
        return;
    /* Sprint 27 hung the strip's menus one row below the strip, and the
     * goldens are drawn that way; every other surface anchors on the
     * cell the user clicked. */
    anchor_y = (c.kind == YEW_CTX_KIND_TAB || c.kind == YEW_CTX_KIND_GROUP)
                   ? (u16)(k->row + 1U) : k->row;
    (void)menu_open_at(ed, &c, k->col, anchor_y, k->col, k->row);
    ed->full_damage = true;
}

static void mouse_press(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    Region hit;

    /*
     * FIRST, and it never touches the phase machine.  Ctrl+left must
     * not arm a drag or enter H mode behind the menu it opens: a
     * selection the user never asked for, left live under a pop-up, is
     * the bug this ordering exists to make impossible.
     */
    if (press_opens_menu(k)) {
        press_menu(ed, k);
        return;
    }
    hit = yew_region_hit(k->col, k->row);
    if (k->button == (u8)YEW_MB_MIDDLE) {
        press_middle(ed, &hit, k);
        return;
    }
    if (k->button != (u8)YEW_MB_LEFT)
        return;
    if (yew_mouse_claimed_by_menu(ed, *k))
        return;

    m->held = (u8)(1U << (k->button - 1U));
    m->press_x = k->col;
    m->press_y = k->row;
    m->press_rgn = hit; /* CAPTURED — see mouse.h */
    m->phase = YEW_MP_ARMED;
    m->drag_tab_id = 0U;
    m->drag_gid = 0U;
    m->drag_to_slot = -1;
    m->drag_to_valid = false;
    m->dwell_gid = 0U;
    m->dwell_since_ms = 0;

    if (hit.kind != YEW_REGION_PANE && hit.kind != YEW_REGION_FUSS_ROW)
        m->click_n = 0U;

    switch (hit.kind) {
    case YEW_REGION_PANE:
        press_pane(ed, &hit, k);
        break;
    case YEW_REGION_PANE_BORDER:
        press_border(ed, &hit);
        break;
    case YEW_REGION_TAB:
        press_tab(ed, &hit);
        break;
    case YEW_REGION_TAB_SCROLL:
        strip_scroll(ed, hit.payload == 2 || hit.payload == -2,
                     hit.payload < 0 ? -1 : 1);
        break;
    case YEW_REGION_TAB_NEW:
        /* Armed only.  Release-in-the-same-region invokes the command,
         * so a press that slides away creates nothing. */
        break;
    case YEW_REGION_PICK_ROW:
        press_pick_row(ed, &hit);
        break;
    case YEW_REGION_FUSS_ROW:
        /*
         * 57.11 §3: A SINGLE CLICK SELECTS.  The survey found this
         * missing and it is the one thing that made the tree feel
         * broken: every other list in the program moves its cursor to
         * where you point.  Opening is still the DOUBLE click, counted
         * below — a tree where one click opened a file could not be
         * browsed at all.
         */
        yew_fuss_select_path(ed, (u32)hit.payload);
        (void)click_advance(ed, k, &hit);
        break;
    case YEW_REGION_GP_ROW:
    case YEW_REGION_GP_NAME:
        if (yew_gp_click(ed, k->col, k->row))
            yew_gp_apply(ed);
        break;
    case YEW_REGION_BLOCK:
        /* Swallowed by construction.  This is what makes a dialog modal
         * to the mouse without any dialog knowing the router exists. */
        break;
    case YEW_REGION_CTX_ROW:
        /* Highlighting, not invoking: the action fires at release, and
         * only when the release lands on the same row. */
        yew_ctx_hover(hit.payload);
        ed->full_damage = true;
        break;
    case YEW_REGION_MENU_ROW:
    case YEW_REGION_NONE:
        /* A left-click outside an open menu closes it, and is consumed
         * doing so — the click that dismisses a menu must not also do
         * whatever is underneath. */
        if (yew_ctx_active()) {
            menu_close(ed);
            m->phase = YEW_MP_IDLE;
            m->held = 0U;
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §2: motion                                                       */
/* ---------------------------------------------------------------- */

/*
 * Extends the selection to the pointer.  The anchor is whatever the
 * press established; §6 replaces the char unit with a word or line
 * engine after a multi-click.
 */
static void drag_select(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    Pane *leaf = yew_pane_leaf_by_index(ed, m->press_rgn.payload);
    Win *w = leaf != NULL ? leaf->win : NULL;
    Cursor *c;
    Span head;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len == 0U)
        return;
    yew_win_click_to_cursor(w, k->col, k->row);
    c = &w->cs.curs.data[w->cs.primary];
    if (m->sel_unit == NULL || m->sel_unit == &yew_unit_char) {
        /* click_to_cursor collapses the anchor onto the caret; the
         * drag's anchor is the press, so it is written back after. */
        c->anchor = BYTEOFF(m->sel_anchor_span.lo);
        ed->full_damage = true;
        return;
    }
    /*
     * EXTENDING BY WHOLE UNITS after a multi-click.  The anchor stays
     * at the initial unit's span and the head snaps to the boundary of
     * the unit under the pointer.
     *
     * Pitfall: extending by CHARACTERS after a word double-click is
     * what a naive implementation does, and it feels broken in a way
     * people describe as "the selection is fighting me".
     */
    head = unit_span_at(w, m->sel_unit, c->pos, m->sel_alt);
    if (head.lo < m->sel_anchor_span.lo) {
        c->anchor = BYTEOFF(m->sel_anchor_span.hi);
        c->pos = BYTEOFF(head.lo);
    } else {
        c->anchor = BYTEOFF(m->sel_anchor_span.lo);
        c->pos = BYTEOFF(head.hi);
    }
    ed->full_damage = true;
}

static void begin_drag(Ed *ed)
{
    MouseState *m = &ed->mouse;

    switch (m->press_rgn.kind) {
    case YEW_REGION_PANE:
        /*
         * A drag in a pane is a selection, and a selection in this
         * editor IS H mode.  Entering it here rather than at press is
         * the arming law: a plain click must not change the mode.
         *
         * A multi-click has ALREADY entered H with its own unit
         * borrowed, and re-entering with the char engine here would
         * throw that away — the drag would then extend by characters
         * after a word double-click, which is exactly the feel §6
         * forbids.
         */
        if (m->sel_unit == NULL || m->sel_unit == &yew_unit_char)
            (void)yew_mode_enter_highlight(ed, YEW_MODE_I, false);
        m->phase = YEW_MP_DRAG_SEL;
        break;
    case YEW_REGION_TAB:
        m->phase = m->press_rgn.payload < 0 ? YEW_MP_DRAG_GROUP
                                            : YEW_MP_DRAG_TAB;
        break;
    default:
        /* Nothing else has a drag; the gesture stays armed and the
         * release is still a click on the captured region. */
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §4: tab and group drag-reorder                                   */
/* ---------------------------------------------------------------- */

/* The group the held tab is currently a member of; 0 when ungrouped.
 * Resolved from the id, never from the press's index. */
static u32 held_tab_group(Ed *ed)
{
    int idx = yew_tab_index_of_id(ed, ed->mouse.drag_tab_id);
    Tab *t = yew_tab_at(ed, idx);

    return t != NULL ? t->group_id : 0U;
}

/*
 * PITFALL — the dwell target is not yew_region_hit.
 *
 * The region table describes the PREVIEWED strip, where the held entry
 * has been moved under the pointer, so hit-testing would always answer
 * "you are hovering the thing you are holding".  The pre-drag slot
 * table (s27 §4, ui/tabs.h) is the only thing that can answer what was
 * here before the drag started.
 *
 * Arming rather than opening: a drag that merely PASSES over a group on
 * its way somewhere else must not make that group's members flash open,
 * so the clock restarts every time the hovered group changes and the
 * open happens in yew_mouse_tick.
 */
static void drag_dwell(Ed *ed, int slot)
{
    MouseState *m = &ed->mouse;
    i32 pre = 0;
    u32 gid = 0U;

    /* Only a TAB dwells into a group.  A group dragged into another
     * group is not a thing this model has — groups do not nest. */
    if (m->phase == YEW_MP_DRAG_TAB && slot >= 0 &&
        yew_strip_pre_payload(slot, &pre) && pre < 0)
        gid = (u32)(-pre);
    /* A tab never dwells into the group it is already a member of:
     * there is nothing to join, and opening the strip would offer a
     * drop that means nothing. */
    if (gid != 0U && gid == held_tab_group(ed))
        gid = 0U;
    if (gid != m->dwell_gid) {
        m->dwell_gid = gid;
        m->dwell_since_ms = gid != 0U ? ed->now_ms : 0;
    }
}

static void drag_strip_motion(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    int slot;

    /*
     * The array is frozen for the drag's lifetime, so a changed count
     * means something ELSE mutated it — an async job closing a file, a
     * script — and the target the user aimed at no longer means what it
     * did.  Cancel outright rather than commit against a moved target.
     */
    if (yew_tab_count(ed) != m->tab_count_at_press) {
        yew_mouse_cancel(ed);
        return;
    }
    slot = yew_strip_slot_at(k->col, k->row);
    if (slot >= 0) {
        if (!m->drag_to_valid || m->drag_to_slot != slot || m->drag_to_tail) {
            m->drag_to_slot = slot;
            m->drag_to_valid = true;
            m->drag_to_tail = false;
            ed->full_damage = true;
        }
    } else if (yew_strip_slot_count() > 0 &&
               k->row == ed->tab_strip_rect.y &&
               k->col >= yew_strip_tail_x()) {
        /*
         * The blank tail past the last entry — row 1's, whatever row
         * the press came from: a member dragged UP out of row 2 aims at
         * row 1's empty space, and that gesture is the whole reason the
         * tail is a drop target.
         */
        if (!m->drag_to_tail) {
            m->drag_to_slot = yew_strip_slot_count() - 1;
            m->drag_to_valid = true;
            m->drag_to_tail = true;
            ed->full_damage = true;
        }
    }
    drag_dwell(ed, slot);
}

/* The tab-array index a row-1 slot names, resolved against the PRE-DRAG
 * list.  A group entry answers with its first member's index, which is
 * where the group's block starts. */
static int slot_to_tab_index(Ed *ed, int slot)
{
    i32 pre = 0;

    if (!yew_strip_pre_payload(slot, &pre))
        return -1;
    if (pre >= 0)
        return (int)pre;
    {
        int members[YEW_TAB_MAX];
        int n = yew_group_members(ed, (u32)(-pre), members,
                                  (int)YEW_ARRAY_LEN(members));
        int lowest = -1;
        int i;

        for (i = 0; i < n; i++) {
            if (lowest < 0 || members[i] < lowest)
                lowest = members[i];
        }
        return lowest;
    }
}

/* Where a group's members begin in the tab array. */
static int group_block_start(Ed *ed, u32 gid)
{
    int members[YEW_TAB_MAX];
    int n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    int lowest = -1;
    int i;

    for (i = 0; i < n; i++) {
        if (lowest < 0 || members[i] < lowest)
            lowest = members[i];
    }
    return lowest;
}

/*
 * Joining a group is Sprint 24's exact sequence, called and never
 * re-derived: the ordinal off-by-one is s24's pinned pitfall and
 * reinventing it here would put a second, subtly different answer in
 * the program.
 */
static void drop_into_group(Ed *ed, u32 gid, int pos)
{
    int tab_idx = yew_tab_index_of_id(ed, ed->mouse.drag_tab_id);
    Tab *t = yew_tab_at(ed, tab_idx);

    if (t == NULL || gid == 0U)
        return;
    if (t->group_id != 0U)
        yew_group_remove_member(ed, tab_idx); /* FIRST */
    /* The removal can dissolve an emptied group and does not move
     * anything, so the index still names this tab. */
    yew_group_add_member(ed, gid, tab_idx);
    yew_group_set_ordinal(ed, tab_idx, pos); /* pos counts in the FINAL list */
    {
        int start = group_block_start(ed, gid);

        if (start >= 0)
            yew_group_reorder_block(ed, gid, start); /* keep contiguous */
    }
    yew_state_mark_dirty(ed);
}

/*
 * Row 2 under the pointer: which group, and at which ordinal.
 *
 * The group is the one row 2 is SHOWING — the dwell's preview when
 * there is one, otherwise the pinned member strip.  Resolving it from
 * the member under the pointer instead would fail on the blank tail,
 * which is where "put it last" has to be expressible.
 */
static bool drop_target_row2(Ed *ed, const Key *k, u32 *gid, int *pos)
{
    Region hit;

    if (ed->tab_strip_rect.h < 2U ||
        k->row != (u16)(ed->tab_strip_rect.y + 1U))
        return false;
    *gid = ed->mouse.preview_gid != 0U ? ed->mouse.preview_gid
                                       : yew_active_group_id(ed);
    if (*gid == 0U)
        return false;
    hit = yew_region_hit(k->col, k->row);
    if (hit.kind == YEW_REGION_TAB && hit.payload >= 0) {
        Tab *t = yew_tab_at(ed, hit.payload);

        if (t != NULL && t->group_id == *gid) {
            *pos = (int)t->group_ordinal;
            return true;
        }
    }
    /* The blank tail of row 2: append. */
    *pos = yew_group_member_count(ed, *gid) + 1;
    return true;
}

static void drag_strip_drop(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    u32 gid = 0U;
    int pos = 0;
    int to;

    if (yew_tab_count(ed) != m->tab_count_at_press)
        return; /* cancelled; nothing was mutated on the way */
    if (m->phase == YEW_MP_DRAG_TAB && drop_target_row2(ed, k, &gid, &pos)) {
        drop_into_group(ed, gid, pos);
        ed->full_damage = true;
        return;
    }
    if (!m->drag_to_valid)
        return; /* released somewhere with no target: nothing changes */
    to = m->drag_to_tail ? (int)yew_tab_count(ed) - 1
                         : slot_to_tab_index(ed, m->drag_to_slot);
    if (to < 0)
        return;
    if (m->phase == YEW_MP_DRAG_GROUP) {
        yew_group_reorder_block(ed, m->drag_gid, to);
    } else {
        int from = yew_tab_index_of_id(ed, m->drag_tab_id);

        if (from < 0)
            return;
        /*
         * Dropping on the blank tail carries the tab OUT of its group —
         * the one gesture that can, when the group is the only row-1
         * entry left to aim at.
         */
        if (m->drag_to_tail && held_tab_group(ed) != 0U)
            yew_group_remove_member(ed, from);
        yew_tab_reorder(ed, from, to);
    }
    yew_state_mark_dirty(ed);
    ed->full_damage = true;
}

static void mouse_motion(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;

    if (k->button == (u8)YEW_MB_NONE) {
        /*
         * §3: HOVER.  Motion with no button held only exists while a
         * menu is open, because that is the only time yew asks the
         * terminal for it (mode 1003, §1).  With no menu the event is
         * DROPPED here — no render, no allocation, no phase change:
         * a pointer merely crossing the screen must cost nothing, and
         * tests/perf/mouse.c is the gate that says so.
         *
         * `yew_ctx_hover_at` maps the cell by the box's geometry rather
         * than through the region table, which may be mid-frame, and
         * returns true ONLY when the highlight moved — so a hundred
         * reports across one row mark no repaint at all.
         */
        if (yew_ctx_active() && yew_ctx_hover_at(k->col, k->row))
            ed->overlay_dirty = true;
        return;
    }
    if (m->phase == YEW_MP_IDLE)
        return;
    m->at_x = k->col;
    m->at_y = k->row;
    if (m->phase == YEW_MP_ARMED) {
        /* The pointer has to leave the pressed CELL.  Cells are the
         * unit of everything here, so there is no pixel radius to
         * tune. */
        if (k->col == m->press_x && k->row == m->press_y)
            return;
        begin_drag(ed);
        if (m->phase == YEW_MP_ARMED)
            return;
    }
    switch (m->phase) {
    case YEW_MP_DRAG_BORDER:
        yew_pane_drag_motion(ed, k->col, k->row);
        break;
    case YEW_MP_DRAG_SEL:
        drag_select(ed, k);
        break;
    case YEW_MP_DRAG_TAB:
    case YEW_MP_DRAG_GROUP:
        drag_strip_motion(ed, k);
        break;
    case YEW_MP_IDLE:
    case YEW_MP_ARMED:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §2: release                                                      */
/* ---------------------------------------------------------------- */

/*
 * A click on a tab entry, resolved from the identity captured at press.
 *
 * Re-reading the region here is exactly what the law forbids: the strip
 * can have scrolled between press and release (a chevron auto-scroll,
 * a job closing a tab), and the cells the pointer is over may now
 * belong to a different file.
 */
static void click_tab(Ed *ed)
{
    MouseState *m = &ed->mouse;

    if (m->drag_gid != 0U) {
        yew_group_note_position(ed);
        /* An EXPLICIT entry resumes where the user left off, unlike a
         * mid-walk arrival which enters from the side it came from. */
        yew_group_enter(ed, m->drag_gid);
        return;
    }
    if (m->drag_tab_id != 0U) {
        int idx = yew_tab_index_of_id(ed, m->drag_tab_id);

        if (idx >= 0)
            yew_tab_switch(ed, idx);
    }
}

static void mouse_release(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    MousePhase phase = m->phase;

    if (k->button != (u8)YEW_MB_LEFT)
        return;
    if (yew_mouse_claimed_by_menu(ed, *k)) {
        gesture_reset(m);
        return;
    }
    if (phase == YEW_MP_IDLE)
        return;
    switch (phase) {
    case YEW_MP_DRAG_BORDER:
        /* Cells become a ratio ONCE, here — a round-trip per motion
         * event accumulates float error and the border stutters against
         * the pointer (s22's pitfall). */
        yew_pane_drag_end(ed);
        break;
    case YEW_MP_ARMED:
        switch (m->press_rgn.kind) {
        case YEW_REGION_TAB:
            click_tab(ed);
            break;
        case YEW_REGION_TAB_NEW: {
            Region up = yew_region_hit(k->col, k->row);

            if (up.kind == YEW_REGION_TAB_NEW &&
                up.rect.x == m->press_rgn.rect.x &&
                up.rect.y == m->press_rgn.rect.y &&
                up.rect.w == m->press_rgn.rect.w &&
                up.rect.h == m->press_rgn.rect.h)
                invoke_mouse_named(ed, "ed.tab.new");
            break;
        }
        case YEW_REGION_PICK_ROW:
            /*
             * Accept only when the release is in the SAME row.  A press
             * that slid onto a neighbour before coming up was a mis-aim
             * and opening the neighbour is the worst possible reading
             * of it.
             */
            if (yew_region_hit(k->col, k->row).kind ==
                    YEW_REGION_PICK_ROW &&
                yew_region_hit(k->col, k->row).payload ==
                    m->press_rgn.payload)
                (void)yew_picker_accept(ed);
            break;
        case YEW_REGION_CTX_ROW: {
            Region up = yew_region_hit(k->col, k->row);

            /* Same row, or nothing: a press that slid onto a neighbour
             * before coming up was a mis-aim, and invoking the
             * neighbour is the worst possible reading of it. */
            if (up.kind == YEW_REGION_CTX_ROW &&
                up.payload == m->press_rgn.payload) {
                yew_ctx_invoke(up.payload);
                apply_menu_action(ed);
                menu_settle(ed);
            }
            ed->full_damage = true;
            break;
        }
        case YEW_REGION_FUSS_ROW: {
            Region up = yew_region_hit(k->col, k->row);

            if (m->click_n == 2U && up.kind == YEW_REGION_FUSS_ROW &&
                up.payload == m->press_rgn.payload)
                invoke_fuss_open(ed, m->press_rgn.payload);
            break;
        }
        default:
            break;
        }
        break;
    case YEW_MP_DRAG_SEL:
        /* The selection is already live; release only ends the
         * gesture.  H mode keeps it, which is what makes every H key,
         * the multi-cursor lift and the recorder work on it. */
        break;
    case YEW_MP_DRAG_TAB:
    case YEW_MP_DRAG_GROUP:
        drag_strip_drop(ed, k);
        break;
    case YEW_MP_IDLE:
    default:
        break;
    }
    if (m->preview_gid != 0U) {
        /* The dwell-opened strip goes away with the gesture that opened
         * it, and it changed the strip's row count, so the layout has to
         * be recomputed rather than merely repainted. */
        ed->layout_dirty = true;
        ed->full_damage = true;
    }
    gesture_reset(m);
}

/* ---------------------------------------------------------------- */
/* Cancellation                                                     */
/* ---------------------------------------------------------------- */

void yew_mouse_cancel(Ed *ed)
{
    if (ed == NULL)
        return;
    /*
     * The multi-click counter goes even when no gesture is in flight:
     * §6 resets it on focus-out, and by then the click that armed it
     * has long since released.
     */
    ed->mouse.click_n = 0U;
    ed->mouse.last_click_ms = 0;
    if (ed->mouse.phase == YEW_MP_IDLE)
        return;
    if (ed->mouse.phase == YEW_MP_DRAG_BORDER)
        yew_pane_drag_cancel(ed);
    /*
     * A tab or group drag needs nothing undone: Tabs.v was never
     * touched.  That is the whole reason the preview is a picture and
     * not a live mutation — cancelling here would otherwise mean
     * undoing an arbitrary number of moves.
     */
    if (ed->mouse.preview_gid != 0U) {
        ed->layout_dirty = true;
        ed->full_damage = true;
    } else if (ed->mouse.drag_to_valid) {
        ed->full_damage = true;
    }
    yew_mouse_init(&ed->mouse);
}

/* ---------------------------------------------------------------- */
/* §4: the clocks                                                   */
/* ---------------------------------------------------------------- */

static bool drag_over_chevron(Ed *ed, i32 *delta)
{
    Region hit = yew_region_hit(ed->mouse.at_x, ed->mouse.at_y);

    if (hit.kind != YEW_REGION_TAB_SCROLL)
        return false;
    *delta = hit.payload < 0 ? -1 : 1;
    return true;
}

void yew_mouse_tick(Ed *ed, i64 now_ms)
{
    MouseState *m;
    i32 delta = 0;

    if (ed == NULL)
        return;
    m = &ed->mouse;
    if (m->phase != YEW_MP_DRAG_TAB && m->phase != YEW_MP_DRAG_GROUP)
        return;
    ed->now_ms = now_ms;
    if (m->dwell_gid != 0U && m->preview_gid != m->dwell_gid &&
        now_ms - m->dwell_since_ms >= YEW_DRAG_DWELL_MS) {
        m->preview_gid = m->dwell_gid;
        /* The strip grew a row, so this is a layout change and not a
         * repaint — the pane tree below it has to give the row back. */
        ed->layout_dirty = true;
        ed->full_damage = true;
    }
    /*
     * Auto-scroll runs on THIS clock rather than per motion event: a
     * fast pointer emits far more motion reports than a slow one, and
     * a strip that scrolled per report would fly past the target at a
     * speed that depends on how the terminal batches its reports.
     */
    if (drag_over_chevron(ed, &delta) &&
        now_ms - m->autoscroll_ms >= YEW_DRAG_SCROLL_MS) {
        m->autoscroll_ms = now_ms;
        strip_scroll(ed, false, delta);
    }
}

i64 yew_mouse_deadline(const Ed *ed, i64 now_ms)
{
    const MouseState *m;
    i64 next = -1;

    if (ed == NULL)
        return -1;
    m = &ed->mouse;
    if (m->phase != YEW_MP_DRAG_TAB && m->phase != YEW_MP_DRAG_GROUP)
        return -1;
    if (m->dwell_gid != 0U && m->preview_gid != m->dwell_gid)
        next = m->dwell_since_ms + YEW_DRAG_DWELL_MS;
    if (yew_region_hit(m->at_x, m->at_y).kind == YEW_REGION_TAB_SCROLL) {
        i64 at = m->autoscroll_ms + YEW_DRAG_SCROLL_MS;

        if (next < 0 || at < next)
            next = at;
    }
    if (next < 0)
        return -1;
    return next <= now_ms ? 0 : next - now_ms;
}

bool yew_mouse_drag_preview(const Ed *ed, i32 *payload, int *to_slot)
{
    if (ed == NULL || payload == NULL || to_slot == NULL)
        return false;
    if (ed->mouse.phase != YEW_MP_DRAG_TAB &&
        ed->mouse.phase != YEW_MP_DRAG_GROUP)
        return false;
    if (!ed->mouse.drag_to_valid)
        return false;
    *payload = ed->mouse.press_rgn.payload;
    *to_slot = ed->mouse.drag_to_slot;
    return true;
}

u32 yew_mouse_preview_group(const Ed *ed)
{
    return ed != NULL ? ed->mouse.preview_gid : 0U;
}

/* ---------------------------------------------------------------- */
/* THE entry point                                                  */
/* ---------------------------------------------------------------- */

void yew_mouse_event(Ed *ed, const Key *k)
{
    if (ed == NULL || k == NULL || k->kind != (u16)YEW_EV_MOUSE)
        return;
    /*
     * §9: with the mouse off, events are DROPPED here rather than at
     * the terminal.  A terminal that keeps reporting after the disable
     * sequence — or one that never honoured it — must not be able to
     * move the cursor, and every action still has its keyboard path.
     */
    if (!yew_mouse_enabled())
        return;
    /*
     * BEFORE the phase machine, and never touching it.  A wheel event
     * has no release, so a state machine keyed on press-without-release
     * hangs on the first scroll — Sprint 4 pinned this and named this
     * sprint as the place it had to be honoured.
     */
    if (is_wheel(k->button)) {
        mouse_wheel(ed, k);
        return;
    }
    switch (k->ev) {
    case YEW_KEY_PRESS:
        mouse_press(ed, k);
        break;
    case YEW_KEY_REPEAT:
        mouse_motion(ed, k);
        break;
    case YEW_KEY_RELEASE:
        mouse_release(ed, k);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §5/§9: the registry commands this file owns                      */
/* ---------------------------------------------------------------- */

/*
 * Invariant 9's entry into the menu: the keyboard opens the SAME menu,
 * for whatever the keyboard is on, with no pointer anywhere.  Without
 * this every row would be a feature the keyboard could not reach.
 *
 * `iarg 1` is the TAB STRIP — Sprint 27's behaviour, kept addressable
 * so a binding can ask for it on purpose.  `iarg 0` or absent is the
 * FOCUS: the FUSS row when F mode is up, the picker row when a picker
 * is, else the document at the cursor cell.  `t m` is the latter, which
 * is what makes the document menu keyboard-reachable (57.11 §5).
 */
static CmdStatus open_strip_menu(Ed *ed)
{
    u32 gid;
    u16 y = (u16)(ed->tab_strip_rect.y + ed->tab_strip_rect.h);

    gid = yew_active_group_id(ed);
    if (gid != 0U) {
        if (!yew_mouse_open_group_menu(ed, gid, ed->tab_strip_rect.x, y))
            return YEW_CMD_ERR_STATE;
    } else {
        Tab *t = yew_tab_at(ed, ed->tabs.active);

        if (t == NULL ||
            !yew_mouse_open_tab_menu(ed, t->tab_id, ed->tab_strip_rect.x,
                                     y))
            return YEW_CMD_ERR_STATE;
    }
    return YEW_CMD_OK;
}

/*
 * The focused leaf's index in THIS frame's leaf table.
 *
 * The captured target is an index into that table, because the region
 * payloads are, and a menu row that re-found its pane any other way
 * would be a second derivation of the layout (Sprint 22's law).  -1
 * means no frame has registered a pane, and then there is nothing on
 * screen for a document menu to point at.
 */
static i32 focused_leaf_index(Ed *ed)
{
    u32 i;

    for (i = 0U; i < ed->nleaf_tab; i++) {
        if (yew_pane_leaf_by_index(ed, (i32)i) == ed->focus)
            return (i32)i;
    }
    return -1;
}

static CmdStatus open_focus_menu(Ed *ed)
{
    CtxContext c;
    u16 x = 0U;
    u16 y = 0U;
    u32 path_id = 0U;

    (void)memset(&c, 0, sizeof(c));
    if (yew_fuss_active(ed)) {
        bool is_dir = false;

        if (!yew_fuss_selected_anchor(ed, &path_id, &x, &y))
            return YEW_CMD_ERR_STATE;
        c.id = path_id;
        c.payload = (i32)path_id;
        if (!yew_fuss_path_is_dir(ed, path_id, &is_dir))
            c.kind = YEW_CTX_KIND_FUSS_BLANK;
        else
            c.kind = is_dir ? YEW_CTX_KIND_FUSS_DIR
                            : YEW_CTX_KIND_FUSS_FILE;
    } else if (yew_picker_active(ed)) {
        if (!yew_picker_sel_cell(ed, &x, &y))
            return YEW_CMD_ERR_STATE;
        c.kind = YEW_CTX_KIND_PICK_ROW;
        c.payload = yew_picker_selected(ed);
        c.id = (u32)c.payload;
    } else {
        i32 leaf = focused_leaf_index(ed);

        if (leaf < 0 || ed->focus == NULL)
            return YEW_CMD_ERR_STATE;
        c.kind = YEW_CTX_KIND_DOC;
        c.id = (u32)leaf;
        c.payload = leaf;
        c.rect = ed->focus->rect;
        /*
         * THE CURSOR CELL, as the renderer last placed it — the same
         * cell the user is looking at.  When it is hidden (a modal has
         * taken it) the pane's own corner is the honest fallback.
         */
        if (ed->grid.cur_vis) {
            x = ed->grid.cur_col;
            y = ed->grid.cur_row;
        } else {
            x = ed->focus->rect.x;
            y = ed->focus->rect.y;
        }
    }
    if (!menu_open_at(ed, &c, x, y, x, y))
        return YEW_CMD_ERR_STATE;
    return YEW_CMD_OK;
}

CmdStatus yew_ui_cmd_context_menu(CmdCtx *cx)
{
    Ed *ed;
    CmdStatus st;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    if (ed->tabs.active < 0)
        return YEW_CMD_ERR_STATE;
    st = cx->iarg == 1 ? open_strip_menu(ed) : open_focus_menu(ed);
    if (st != YEW_CMD_OK)
        return st;
    ed->full_damage = true;
    return YEW_CMD_OK;
}

/*
 * §9: the runtime toggle.  The option model that PERSISTS it is Sprint
 * 36; until then it lives for the session, which is what makes it
 * usable for "this terminal's mouse reporting is fighting me right
 * now".
 */
static bool mouse_enabled = true;
static bool mouse_resolved;

bool yew_mouse_enabled(void)
{
    if (!mouse_resolved) {
        const char *off = getenv("YEW_MOUSE");

        /* Resolved ONCE, and to the same answer term/input.c used when
         * it decided whether to ask the terminal to report at all — two
         * places reading the environment separately is how they come to
         * disagree. */
        mouse_resolved = true;
        mouse_enabled = off == NULL || off[0] != '0';
    }
    return mouse_enabled;
}

void yew_mouse_set_enabled(bool on)
{
    mouse_resolved = true;
    mouse_enabled = on;
}

CmdStatus yew_mouse_cmd_enable(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    mouse_enabled = true;
    yew_msg(cx->ed, YEW_MSG_INFO, "mouse on");
    return YEW_CMD_OK;
}

CmdStatus yew_mouse_cmd_disable(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    /* Any gesture in flight goes with it: a router that stopped
     * receiving events mid-drag would sit with the button logically
     * down forever. */
    yew_mouse_cancel(cx->ed);
    /* Through THE one close path, so the menu the disable dismisses
     * takes any-motion tracking with it (§1/§3). */
    menu_close(cx->ed);
    mouse_enabled = false;
    yew_msg(cx->ed, YEW_MSG_INFO, "mouse off");
    return YEW_CMD_OK;
}
