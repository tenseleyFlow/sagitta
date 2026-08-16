#include "ui/complmenu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "edit/theme_cmds.h"
#include "term/grid.h"
#include "text/edit.h"
#include "text/undo.h"
#include "ui/message.h"
#include "ui/region.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/intern.h"
#include "util/log.h"
#include "ws/symidx.h"
#include "ws/symwalk.h"

static const u8 detail_func[] = "fn";
static const u8 detail_type[] = "type";
static const u8 detail_macro[] = "macro";
static const u8 detail_keyword[] = "keyword";

static const u8 *kind_detail(u8 kind, u32 *len)
{
    switch (kind) {
    case YEW_SYMK_FUNC:
        *len = 2U;
        return detail_func;
    case YEW_SYMK_TYPE:
        *len = 4U;
        return detail_type;
    case YEW_SYMK_MACRO:
        *len = 5U;
        return detail_macro;
    case YEW_SYMK_KEYWORD:
        *len = 7U;
        return detail_keyword;
    default:
        *len = 0U;
        return NULL;
    }
}

static u8 kind_glyph(u8 kind)
{
    static const u8 glyphs[YEW_SYMK_NKIND] = {'w', 'f', 't', 'm', 'k'};

    return kind < YEW_SYMK_NKIND ? glyphs[kind] : (u8)'w';
}

static const char *kind_role(u8 kind)
{
    static const char *const roles[YEW_SYMK_NKIND] = {
        "compl.word", "compl.func", "compl.type", "compl.macro",
        "compl.keyword",
    };

    return kind < YEW_SYMK_NKIND ? roles[kind] : roles[YEW_SYMK_WORD];
}

static bool text_copy(const TextBuf *tb, Span span, u8 *dst)
{
    TextIter it;
    u64 copied = 0U;

    if (span.hi < span.lo || (span.hi != span.lo && dst == NULL))
        return false;
    if (span.lo == span.hi)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return false;
    while (span.lo + copied < span.hi) {
        const u8 *bytes;
        u64 len;
        u64 remain = span.hi - span.lo - copied;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
            return false;
        take = len < remain ? len : remain;
        if (take > (u64)SIZE_MAX)
            return false;
        (void)memcpy(dst + (size_t)copied, bytes, (size_t)take);
        copied += take;
        if (span.lo + copied < span.hi && !yew_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

static bool compl_stem(Win *w, Span *replace, u8 **stem, u32 *slen)
{
    UnitCtx unit;
    Cursor *cursor;
    Span word;
    u64 len;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len)
        return false;
    cursor = &w->cs.curs.data[w->cs.primary];
    unit = (UnitCtx){w->buf->tb, w->buf, w};
    word = yew_unit_word.span(&unit, cursor->pos, false);
    /* A cursor is a gap.  At the right edge of an identifier the unit at
     * the gap can be the following whitespace, while completion owns the
     * identifier immediately to its left. */
    if (word.lo >= cursor->pos.v && cursor->pos.v != 0U) {
        ByteOff previous = yew_grapheme_prev_boundary(w->buf->tb,
                                                       cursor->pos);

        word = yew_unit_word.span(&unit, previous, false);
    }
    if (word.lo > cursor->pos.v || word.hi < cursor->pos.v)
        word = (Span){cursor->pos.v, cursor->pos.v};
    word.hi = cursor->pos.v;
    len = word.hi - word.lo;
    if (len > UINT32_MAX)
        return false;
    *stem = len == 0U ? NULL : yew_xmalloc((size_t)len);
    if (!text_copy(w->buf->tb, word, *stem)) {
        free(*stem);
        *stem = NULL;
        return false;
    }
    *replace = word;
    *slen = (u32)len;
    return true;
}

static u32 index_enumerate(Ed *ed, Win *w, const u8 *stem, u32 slen,
                           Vec_ComplItem *out, void *ctx)
{
    SymHit hits[YEW_SYM_QUERY_MAX];
    SymQuery query;
    u32 n;
    u32 i;

    (void)ctx;
    if (ed == NULL || w == NULL || w->buf == NULL || stem == NULL ||
        slen == 0U)
        return 0U;
    query = (SymQuery){(const char *)stem, slen, w->buf->id,
                       w->cs.curs.data[w->cs.primary].pos,
                       YEW_SYM_QUERY_MAX, true};
    n = yew_symidx_query(&ed->ws, &query, hits, YEW_ARRAY_LEN(hits));
    for (i = 0U; i < n; i++) {
        const char *label = yew_intern_str(&ed->interner, hits[i].name);
        size_t label_len = yew_intern_len(&ed->interner, hits[i].name);
        ComplItem item = {0};

        if (label == NULL || label_len > UINT32_MAX)
            continue;
        item.label = (const u8 *)label;
        item.label_len = (u32)label_len;
        item.insert = item.label;
        item.insert_len = item.label_len;
        item.detail = kind_detail(hits[i].kind, &item.detail_len);
        item.kind = hits[i].kind;
        item.score = hits[i].rank;
        item.m = hits[i].m;
        Vec_ComplItem_push(out, item);
    }
    return (u32)out->len;
}

const ComplSource yew_compl_source_index = {
    .name = "index",
    .enumerate = index_enumerate,
};

static void compl_items_clear(ComplMenu *menu)
{
    size_t i;

    if (menu == NULL)
        return;
    if (menu->src != NULL && menu->src->discard != NULL)
        for (i = 0U; i < menu->items.len; i++)
            menu->src->discard(&menu->items.data[i], menu->src->ctx);
    menu->items.len = 0U;
}

static u16 u16_min(u16 a, u16 b)
{
    return a < b ? a : b;
}

void yew_compl_resize(Ed *ed, Win *w)
{
    ComplMenu *menu;
    TextBuf *tb;
    LineNo line;
    Span line_span;
    CCol col;
    u16 row;
    u16 x;
    u16 pane_right;
    u16 pane_bottom;
    u16 below;
    u16 above;
    u16 visible;
    u16 box_w;
    u16 panel_w;
    u16 need_h;
    u16 y;

    if (ed == NULL || w == NULL || !w->compl.open || w->buf == NULL ||
        w->buf->tb == NULL || w->rect.w < 4U || w->rect.h < 4U)
        return;
    menu = &w->compl;
    tb = w->buf->tb;
    line = yew_textbuf_line_of(tb, BYTEOFF(menu->replace.lo));
    if (!yew_vp_row_of_line(w, line, 0U, &row))
        row = 0U;
    line_span = yew_textbuf_line_span(tb, line);
    col = yew_off_to_ccol(tb, line_span, BYTEOFF(menu->replace.lo),
                          w->buf->tabwidth == 0U ? YEW_VP_TABWIDTH :
                                                  w->buf->tabwidth);
    x = yew_vp_gridx_of_ccol(w, col);
    pane_right = (u32)w->rect.x + w->rect.w > UINT16_MAX ? UINT16_MAX :
                 (u16)(w->rect.x + w->rect.w);
    pane_bottom = (u32)w->rect.y + w->rect.h > UINT16_MAX ? UINT16_MAX :
                  (u16)(w->rect.y + w->rect.h);
    box_w = u16_min((u16)(YEW_COMPL_W + 2U), w->rect.w);
    panel_w = u16_min((u16)28U,
                      pane_right > box_w ? (u16)(pane_right - box_w) : 0U);
    if (menu->panel_open && panel_w < 12U)
        panel_w = 0U;
    if ((u32)x + box_w + (menu->panel_open ? panel_w : 0U) > pane_right)
        x = pane_right > box_w + (menu->panel_open ? panel_w : 0U)
                ? (u16)(pane_right - box_w -
                        (menu->panel_open ? panel_w : 0U))
                : w->rect.x;
    below = pane_bottom > (u16)(w->rect.y + row + 1U)
                ? (u16)(pane_bottom - (u16)(w->rect.y + row + 1U)) : 0U;
    above = row;
    visible = menu->items.len < YEW_COMPL_ROWS ? (u16)menu->items.len :
                                                YEW_COMPL_ROWS;
    if (visible == 0U)
        visible = 1U;
    need_h = (u16)(visible + 3U); /* borders plus footer */
    if (below >= 4U) {
        if (need_h > below)
            visible = below > 3U ? (u16)(below - 3U) : 1U;
        y = (u16)(w->rect.y + row + 1U);
    } else {
        if (need_h > above)
            visible = above > 3U ? (u16)(above - 3U) : 1U;
        need_h = (u16)(visible + 3U);
        y = w->rect.y + row >= need_h ?
                (u16)(w->rect.y + row - need_h) : w->rect.y;
    }
    menu->box = (Rect){x, y, box_w, (u16)(visible + 2U)};
    menu->panel = (Rect){(u16)(x + box_w), y, panel_w,
                         menu->box.h};
    if (menu->sel >= 0) {
        u32 selected = (u32)menu->sel;

        if (selected < menu->top)
            menu->top = selected;
        else if (selected >= menu->top + visible)
            menu->top = selected - visible + 1U;
    }
    if (menu->top + visible > menu->items.len)
        menu->top = menu->items.len > visible ?
                        (u32)menu->items.len - visible : 0U;
}

static void compl_fill(Ed *ed, Win *w, const ComplSource *src,
                       const u8 *stem, u32 slen)
{
    ComplMenu *menu = &w->compl;

    compl_items_clear(menu);
    menu->sel = -1;
    menu->top = 0U;
    menu->src = src;
    if (src->enumerate != NULL)
        (void)src->enumerate(ed, w, stem, slen, &menu->items, src->ctx);
    if (menu->items.len != 0U)
        menu->sel = 0;
}

bool yew_compl_open_source(Ed *ed, Win *w, const ComplSource *src)
{
    u8 *stem = NULL;
    u32 slen = 0U;
    Span replace;

    if (ed == NULL || w == NULL || src == NULL || src->name == NULL ||
        w->buf == NULL || w->buf->tb == NULL ||
        !compl_stem(w, &replace, &stem, &slen) ||
        (slen == 0U &&
         (src->flags & YEW_COMPL_SRC_EMPTY_STEM) == 0U)) {
        free(stem);
        return false;
    }
    if (w->compl.open)
        yew_compl_close_result(ed, w, false);
    w->compl.open = true;
    w->compl.panel_open = false;
    w->compl.replace = replace;
    w->compl.buf_gen = w->buf->tb->gen;
    yew_shadow_dismiss(ed, w);
    w->shadow.suppressed = true;
    compl_fill(ed, w, src, stem, slen);
    free(stem);
    if (w->compl.items.len == 0U &&
        (src->flags & YEW_COMPL_SRC_ASYNC) == 0U) {
        yew_compl_close_result(ed, w, false);
        return false;
    }
    yew_compl_resize(ed, w);
    ed->full_damage = true;
    return true;
}

static bool compl_has_suffix(Win *w, const char *want, u32 len)
{
    Cursor *cursor;
    u8 *got;
    bool equal;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL || want == NULL ||
        len == 0U || w->cs.curs.len != 1U ||
        w->cs.primary >= w->cs.curs.len)
        return false;
    cursor = &w->cs.curs.data[w->cs.primary];
    if (cursor->pos.v < len)
        return false;
    got = yew_xmalloc(len);
    equal = text_copy(w->buf->tb,
                      (Span){cursor->pos.v - len, cursor->pos.v}, got) &&
            memcmp(got, want, len) == 0;
    free(got);
    return equal;
}

bool yew_compl_maybe_auto_trigger(Ed *ed, Win *w)
{
    OptVal enabled;
    OptVal triggers;
    u32 at = 0U;

    if (ed == NULL || w == NULL || w->compl.open || ed->mode != YEW_MODE_I ||
        !yew_opt_get(ed, w->buf, w, "compl.auto_trigger", 18U, &enabled) ||
        enabled.type != (u8)YEW_OPT_BOOL || !enabled.as.b ||
        !yew_opt_get(ed, w->buf, w, "compl.trigger_chars", 19U, &triggers) ||
        triggers.type != (u8)YEW_OPT_STR)
        return false;
    while (at < triggers.as.str.len) {
        u32 lo;

        while (at < triggers.as.str.len &&
               (triggers.as.str.s[at] == ' ' ||
                triggers.as.str.s[at] == '\t'))
            at++;
        lo = at;
        while (at < triggers.as.str.len &&
               triggers.as.str.s[at] != ' ' &&
               triggers.as.str.s[at] != '\t')
            at++;
        if (at != lo && compl_has_suffix(w, triggers.as.str.s + lo,
                                         at - lo))
            return yew_compl_open_source(ed, w, &yew_compl_source_index);
    }
    return false;
}

void yew_compl_close_result(Ed *ed, Win *w, bool accepted)
{
    (void)accepted;
    if (w == NULL)
        return;
    if (w->compl.src != NULL && w->compl.src->close != NULL)
        w->compl.src->close(ed, w, w->compl.src->ctx);
    compl_items_clear(&w->compl);
    w->compl.open = false;
    w->compl.sel = -1;
    w->compl.top = 0U;
    w->compl.panel_open = false;
    w->compl.src = NULL;
    w->compl.box = (Rect){0U, 0U, 0U, 0U};
    w->compl.panel = (Rect){0U, 0U, 0U, 0U};
    w->shadow.suppressed = false;
    yew_shadow_arm(ed, w);
    if (ed != NULL)
        ed->full_damage = true;
}

void yew_compl_open(Ed *ed, Win *w)
{
    if (w == NULL)
        return;
    yew_shadow_dismiss(ed, w);
    w->shadow.suppressed = true;
    w->compl.open = true;
    if (ed != NULL)
        ed->full_damage = true;
}

void yew_compl_close(Ed *ed, Win *w)
{
    yew_compl_close_result(ed, w, false);
}

void yew_compl_free(ComplMenu *menu)
{
    if (menu == NULL)
        return;
    compl_items_clear(menu);
    Vec_ComplItem_free(&menu->items);
    (void)memset(menu, 0, sizeof(*menu));
}

void yew_compl_push(Ed *ed, Win *w, const ComplItem *it, u32 n)
{
    u32 i;

    if (ed == NULL || w == NULL || !w->compl.open || it == NULL)
        return;
    for (i = 0U; i < n; i++)
        Vec_ComplItem_push(&w->compl.items, it[i]);
    if (w->compl.sel < 0 && w->compl.items.len != 0U)
        w->compl.sel = 0;
    yew_compl_resize(ed, w);
    ed->full_damage = true;
}

static void compl_move(Ed *ed, Win *w, i32 delta, bool page)
{
    ComplMenu *menu;
    i64 n;
    i64 next;
    i32 step;

    if (ed == NULL || w == NULL || !w->compl.open ||
        w->compl.items.len == 0U)
        return;
    menu = &w->compl;
    n = (i64)menu->items.len;
    step = page ? (i32)(menu->box.h > 2U ? menu->box.h - 2U : 1U) : 1;
    next = menu->sel < 0 ? (delta < 0 ? n - 1 : 0) :
                           (i64)menu->sel + (i64)delta * step;
    while (next < 0)
        next += n;
    next %= n;
    menu->sel = (i32)next;
    if (menu->panel_open && menu->src != NULL &&
        menu->src->resolve != NULL &&
        menu->items.data[menu->sel].doc == NULL)
        menu->src->resolve(ed, w, &menu->items.data[menu->sel],
                           menu->src->ctx);
    yew_compl_resize(ed, w);
    ed->full_damage = true;
}

void yew_compl_select(Ed *ed, Win *w, i32 item)
{
    if (ed == NULL || w == NULL || !w->compl.open || item < 0 ||
        (size_t)item >= w->compl.items.len)
        return;
    w->compl.sel = item;
    if (w->compl.panel_open && w->compl.src != NULL &&
        w->compl.src->resolve != NULL &&
        w->compl.items.data[item].doc == NULL)
        w->compl.src->resolve(ed, w, &w->compl.items.data[item],
                              w->compl.src->ctx);
    yew_compl_resize(ed, w);
    ed->full_damage = true;
}

static bool compl_accept(Ed *ed, Win *w)
{
    ComplMenu *menu;
    ComplItem item;
    EditCtx edit;
    bool own_txn;
    bool ok;

    if (ed == NULL || w == NULL || !w->compl.open ||
        w->compl.sel < 0 || (size_t)w->compl.sel >= w->compl.items.len ||
        w->buf == NULL || w->buf->tb == NULL)
        return false;
    if (w->buf->tb->gen != w->compl.buf_gen) {
        yew_compl_close_result(ed, w, false);
        return false;
    }
    menu = &w->compl;
    item = menu->items.data[menu->sel];
    if (item.insert == NULL)
        return false;
    if (menu->src != NULL && menu->src->accept != NULL) {
        ok = menu->src->accept(ed, w, menu->replace, &item,
                               menu->src->ctx);
        if (ok)
            yew_compl_close_result(ed, w, true);
        return ok;
    }
    edit = yew_ed_edit_ctx_for(ed, w);
    own_txn = edit.undo != NULL && edit.undo->depth == 0U;
    if (own_txn)
        yew_undo_begin(&edit, YEW_TXN_PASTE);
    ok = menu->replace.lo == menu->replace.hi ||
         yew_edit_delete(&edit, menu->replace);
    if (ok)
        ok = yew_edit_insert(&edit, BYTEOFF(menu->replace.lo), item.insert,
                             item.insert_len);
    if (own_txn) {
        if (ok)
            yew_undo_end(&edit);
        else
            yew_undo_abort(&edit);
        yew_ed_finish_edit(ed, &edit);
    }
    if (!ok)
        return false;
    yew_compl_close_result(ed, w, true);
    return true;
}

static bool is_printable(const Key *k)
{
    const u16 command = YEW_MOD_CTRL | YEW_MOD_ALT | YEW_MOD_SUPER;

    return k->ev != YEW_KEY_RELEASE && k->ntext != 0U &&
           (k->mods & command) == 0U;
}

static void compl_expect_stem_edit(Win *w, const Key *k)
{
    ComplMenu *menu;
    Cursor *cursor;
    u64 next_hi;

    if (w == NULL || k == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len)
        return;
    menu = &w->compl;
    cursor = &w->cs.curs.data[w->cs.primary];
    if (w->buf->tb->gen != menu->buf_gen ||
        cursor->pos.v != menu->replace.hi || menu->buf_gen == UINT64_MAX)
        return;
    next_hi = menu->replace.hi;
    if (is_printable(k)) {
        if ((u64)k->ntext > UINT64_MAX - next_hi)
            return;
        next_hi += (u64)k->ntext;
    } else if (k->code == YEW_KEY_BACKSPACE) {
        if (next_hi <= menu->replace.lo)
            return;
        next_hi = yew_grapheme_prev_boundary(w->buf->tb,
                                              BYTEOFF(next_hi)).v;
        if (next_hi < menu->replace.lo)
            return;
    } else {
        return;
    }
    menu->replace.hi = next_hi;
    menu->buf_gen++;
}

bool yew_compl_key(Ed *ed, Win *w, const Key *k)
{
    bool ctrl;

    if (ed == NULL || w == NULL || k == NULL || !w->compl.open ||
        k->ev == YEW_KEY_RELEASE)
        return false;
    ctrl = (k->mods & YEW_MOD_CTRL) != 0U;
    if (k->code == YEW_KEY_UP || (ctrl && (k->code == 'p' || k->code == 16U))) {
        compl_move(ed, w, -1, false);
        return true;
    }
    if (k->code == YEW_KEY_DOWN || (ctrl && (k->code == 'n' || k->code == 14U))) {
        compl_move(ed, w, 1, false);
        return true;
    }
    if (k->code == YEW_KEY_PAGE_UP) {
        compl_move(ed, w, -1, true);
        return true;
    }
    if (k->code == YEW_KEY_PAGE_DOWN) {
        compl_move(ed, w, 1, true);
        return true;
    }
    if (k->code == YEW_KEY_TAB ||
        (k->code == YEW_KEY_ENTER && w->compl.sel >= 0)) {
        (void)compl_accept(ed, w);
        return true;
    }
    if (k->code == YEW_KEY_ESCAPE) {
        yew_compl_close_result(ed, w, false);
        return true;
    }
    if (ctrl && (k->code == ' ' || k->code == 0U)) {
        w->compl.panel_open = !w->compl.panel_open;
        if (w->compl.panel_open && w->compl.sel >= 0 &&
            w->compl.src != NULL && w->compl.src->resolve != NULL &&
            w->compl.items.data[w->compl.sel].doc == NULL)
            w->compl.src->resolve(ed, w,
                &w->compl.items.data[w->compl.sel], w->compl.src->ctx);
        yew_compl_resize(ed, w);
        ed->full_damage = true;
        return true;
    }
    if (is_printable(k) || k->code == YEW_KEY_BACKSPACE) {
        compl_expect_stem_edit(w, k);
        return false;
    }
    yew_compl_close_result(ed, w, false);
    return false;
}

void yew_compl_after_key(Ed *ed, Win *w)
{
    ComplMenu *menu;
    u8 *stem = NULL;
    u32 slen = 0U;
    Span replace;

    if (ed == NULL || w == NULL || !w->compl.open || w->compl.src == NULL ||
        w->buf == NULL || w->buf->tb == NULL)
        return;
    menu = &w->compl;
    if (w->buf->tb->gen != menu->buf_gen ||
        !compl_stem(w, &replace, &stem, &slen) ||
        (slen == 0U &&
         (menu->src->flags & YEW_COMPL_SRC_EMPTY_STEM) == 0U) ||
        replace.lo != menu->replace.lo || replace.hi != menu->replace.hi) {
        free(stem);
        yew_compl_close_result(ed, w, false);
        return;
    }
    compl_fill(ed, w, menu->src, stem, slen);
    free(stem);
    if (menu->items.len == 0U &&
        (menu->src->flags & YEW_COMPL_SRC_ASYNC) == 0U) {
        yew_compl_close_result(ed, w, false);
        return;
    }
    yew_compl_resize(ed, w);
    ed->full_damage = true;
}

static Cell compl_style(const Ed *ed, const Grid *g, const char *role,
                        bool selected)
{
    const ThemeEnt *theme = yew_theme_ui_tab(ed, role);
    const ThemeEnt *sel = selected ? yew_theme_ui_tab(ed, "sel") : NULL;
    Cell out = g->blank;

    if (theme != NULL) {
        out.fg = theme->fg;
        out.bg = theme->bg;
        out.attrs = theme->attrs;
    }
    if (sel != NULL) {
        out.fg = sel->fg;
        out.bg = sel->bg;
        out.attrs |= sel->attrs;
    } else if (selected) {
        out.attrs |= YEW_ATTR_REVERSE;
    }
    return out;
}

static void put_ascii(Grid *g, u16 row, u16 col, u8 ch, Cell style)
{
    (void)yew_grid_put(g, row, col, &ch, 1U, style.fg, style.bg,
                       style.attrs);
}

static void draw_border(Grid *g, Rect r, Cell style)
{
    u16 x;

    if (r.w < 2U || r.h < 2U)
        return;
    put_ascii(g, r.y, r.x, '+', style);
    put_ascii(g, r.y, (u16)(r.x + r.w - 1U), '+', style);
    put_ascii(g, (u16)(r.y + r.h - 1U), r.x, '+', style);
    put_ascii(g, (u16)(r.y + r.h - 1U),
              (u16)(r.x + r.w - 1U), '+', style);
    for (x = 1U; x + 1U < r.w; x++) {
        put_ascii(g, r.y, (u16)(r.x + x), '-', style);
        put_ascii(g, (u16)(r.y + r.h - 1U), (u16)(r.x + x), '-', style);
    }
    for (x = 1U; x + 1U < r.h; x++) {
        put_ascii(g, (u16)(r.y + x), r.x, '|', style);
        put_ascii(g, (u16)(r.y + x), (u16)(r.x + r.w - 1U), '|', style);
    }
}

static bool match_at(const FzMatch *m, u32 at)
{
    u16 i;

    for (i = 0U; i < m->n_pos; i++)
        if (m->pos[i] == at)
            return true;
    return false;
}

static void draw_label(Grid *g, u16 row, u16 col, u16 width,
                       const ComplItem *item, Cell style)
{
    size_t at = 0U;
    size_t keep = yew_str_clip(item->label, item->label_len, width, NULL);
    u16 x = col;

    while (at < keep && x < (u16)(col + width)) {
        size_t next = yew_gb_next_bytes(item->label, keep, at);
        u16 attrs = style.attrs;

        if (next <= at || next > keep)
            break;
        if (match_at(&item->m, (u32)at))
            attrs |= YEW_ATTR_BOLD;
        x = yew_grid_put(g, row, x, item->label + at, next - at,
                         style.fg, style.bg, attrs);
        at = next;
    }
}

static void draw_panel_body(Grid *g, Rect panel, const ComplItem *item,
                            Cell style)
{
    static const u8 none[] = "(no documentation)";
    const u8 *text = item != NULL && item->doc != NULL ? item->doc : none;
    u32 len = item != NULL && item->doc != NULL ? item->doc_len :
                                                  (u32)sizeof(none) - 1U;
    u32 at = 0U;
    u16 row;
    u16 width = panel.w > 2U ? (u16)(panel.w - 2U) : 0U;

    for (row = 1U; row + 1U < panel.h && at < len; row++) {
        u32 line_end = at;
        size_t keep;

        while (line_end < len && text[line_end] != '\n')
            line_end++;
        keep = yew_str_clip(text + at, line_end - at, width, NULL);
        if (keep == 0U && line_end > at)
            break;
        (void)yew_grid_puts(g, (u16)(panel.y + row),
                            (u16)(panel.x + 1U), text + at, keep,
                            style.fg, style.bg, style.attrs);
        at += (u32)keep;
        while (at < line_end && text[at] == ' ')
            at++;
        if (at == line_end && at < len && text[at] == '\n')
            at++;
    }
}

void yew_compl_draw(Ed *ed, Win *w, Grid *g)
{
    ComplMenu *menu;
    Cell base;
    u16 visible;
    u16 row;
    char footer[96];
    int footer_n;

    if (ed == NULL || w == NULL || g == NULL || !w->compl.open)
        return;
    menu = &w->compl;
    base = compl_style(ed, g, "fg", false);
    draw_border(g, menu->box, base);
    /* Added before rows so last-added-wins leaves the item rectangles
     * clickable while every gap still blocks the document beneath. */
    yew_region_add(YEW_REGION_BLOCK, menu->box, 0);
    visible = menu->box.h > 2U ? (u16)(menu->box.h - 2U) : 0U;
    for (row = 0U; row < visible; row++) {
        u32 index = menu->top + row;
        Rect rr = {(u16)(menu->box.x + 1U),
                   (u16)(menu->box.y + 1U + row),
                   menu->box.w > 2U ? (u16)(menu->box.w - 2U) : 0U, 1U};
        Cell style;
        u8 glyph;
        u16 label_w;

        if (index >= menu->items.len)
            break;
        style = compl_style(ed, g, kind_role(menu->items.data[index].kind),
                            (i32)index == menu->sel);
        yew_grid_fill(g, rr.y, rr.x, (u16)(rr.x + rr.w), style);
        glyph = kind_glyph(menu->items.data[index].kind);
        put_ascii(g, rr.y, rr.x, glyph, style);
        label_w = rr.w > YEW_COMPL_DETAIL_W + 2U ?
                    (u16)(rr.w - YEW_COMPL_DETAIL_W - 2U) :
                    (rr.w > 2U ? (u16)(rr.w - 2U) : 0U);
        draw_label(g, rr.y, (u16)(rr.x + 2U), label_w,
                   &menu->items.data[index], style);
        if (menu->items.data[index].detail != NULL &&
            rr.w > YEW_COMPL_DETAIL_W) {
            size_t keep = yew_str_clip(menu->items.data[index].detail,
                                       menu->items.data[index].detail_len,
                                       YEW_COMPL_DETAIL_W, NULL);

            (void)yew_grid_puts(g, rr.y,
                (u16)(rr.x + rr.w - YEW_COMPL_DETAIL_W),
                menu->items.data[index].detail, keep,
                style.fg, style.bg, style.attrs);
        }
        yew_region_add(YEW_REGION_COMPL_ROW, rr, (i32)index);
    }
    footer_n = snprintf(footer, sizeof(footer), "%d of %zu   %s",
                        menu->sel >= 0 ? menu->sel + 1 : 0,
                        menu->items.len,
                        menu->src == NULL ? "" : menu->src->name);
    if (footer_n > 0 && menu->box.y + menu->box.h < g->rows) {
        size_t n = (size_t)footer_n < sizeof(footer) ? (size_t)footer_n :
                                                       sizeof(footer) - 1U;
        n = yew_str_clip((const u8 *)footer, n, menu->box.w, NULL);
        (void)yew_grid_puts(g, (u16)(menu->box.y + menu->box.h),
                            menu->box.x, (const u8 *)footer, n,
                            base.fg, base.bg, base.attrs);
    }
    if (menu->panel_open && menu->panel.w >= 2U) {
        const ComplItem *item = menu->sel >= 0 &&
                                (size_t)menu->sel < menu->items.len ?
                                &menu->items.data[menu->sel] : NULL;

        draw_border(g, menu->panel, base);
        draw_panel_body(g, menu->panel, item, base);
        yew_region_add(YEW_REGION_BLOCK, menu->panel, 0);
    }
}

CmdStatus yew_compl_cmd_open(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    if (!yew_compl_open_source(cx->ed, cx->win,
                               &yew_compl_source_index)) {
        yew_msg(cx->ed, YEW_MSG_INFO, "no completions");
        return YEW_CMD_ERR_STATE;
    }
    return YEW_CMD_OK;
}

CmdStatus yew_compl_cmd_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    compl_move(cx->ed, cx->win, 1, false);
    return cx->win->compl.open ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    compl_move(cx->ed, cx->win, -1, false);
    return cx->win->compl.open ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_page_next(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    compl_move(cx->ed, cx->win, 1, true);
    return cx->win->compl.open ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_page_prev(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    compl_move(cx->ed, cx->win, -1, true);
    return cx->win->compl.open ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_accept(CmdCtx *cx)
{
    return cx != NULL && compl_accept(cx->ed, cx->win) ? YEW_CMD_OK :
                                                        YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_cancel(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        !cx->win->compl.open)
        return YEW_CMD_ERR_STATE;
    yew_compl_close_result(cx->ed, cx->win, false);
    return YEW_CMD_OK;
}

CmdStatus yew_compl_cmd_doc_toggle(CmdCtx *cx)
{
    Key key = {0};

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    key.code = ' ';
    key.mods = YEW_MOD_CTRL;
    key.ev = YEW_KEY_PRESS;
    return yew_compl_key(cx->ed, cx->win, &key) ? YEW_CMD_OK :
                                                 YEW_CMD_ERR_STATE;
}

CmdStatus yew_compl_cmd_stats(CmdCtx *cx)
{
    u64 buffer_symbols = 0U;
    u64 buffer_bytes = 0U;
    u64 total_bytes;
    bool buffer_capped = false;
    size_t i;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    for (i = 0U; i < cx->ed->ws.sym_buf.len; i++) {
        buffer_symbols += cx->ed->ws.sym_buf.data[i].idx.e.len;
        buffer_bytes += cx->ed->ws.sym_buf.data[i].idx.bytes;
        if (cx->ed->ws.sym_buf.data[i].idx.capped)
            buffer_capped = true;
    }
    total_bytes = yew_symidx_workspace_bytes(&cx->ed->ws);
    yew_msg(cx->ed, YEW_MSG_INFO,
            "completion: buffers=%zu symbols=%llu bytes=%llu; "
            "workspace files=%llu/%llu symbols=%zu bytes=%llu; "
            "caps files=%u file-bytes=%u line-bytes=%u symbols/file=%u "
            "memory=%llu/%u long-line-files=%llu "
            "buffer-indexes=%s workspace=%s walk=%s",
            cx->ed->ws.sym_buf.len,
            (unsigned long long)buffer_symbols,
            (unsigned long long)buffer_bytes,
            (unsigned long long)cx->ed->ws.sym_walk.files_done,
            (unsigned long long)cx->ed->ws.sym_walk.files_total,
            cx->ed->ws.sym_ws.e.len,
            (unsigned long long)cx->ed->ws.sym_ws.bytes,
            YEW_SYMWALK_MAX_FILES, YEW_SYMWALK_MAX_FILE_BYTES,
            YEW_SYMWALK_MAX_LINE_BYTES, YEW_SYMWALK_MAX_SYMS_PER_FILE,
            (unsigned long long)total_bytes, YEW_SYMIDX_BYTES_MAX,
            (unsigned long long)cx->ed->ws.sym_walk.long_files_skipped,
            buffer_capped ? "capped" : "ok",
            cx->ed->ws.sym_ws.capped ? "capped" : "ok",
            cx->ed->ws.sym_walk.capped ? "capped" : "ok");
    return YEW_CMD_OK;
}

CmdStatus yew_compl_cmd_reindex(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_symwalk_start(cx->ed);
    return YEW_CMD_OK;
}
