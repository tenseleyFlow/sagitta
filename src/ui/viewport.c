#include "ui/viewport.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "text/cursor.h"
#include "ui/win.h"
#include "unicode/grapheme.h"
#include "util/base.h"
#include "util/log.h"

/* A linked diff view uses row-aligned scratch buffers, so equal viewport
 * line numbers are equal display rows.  The guard prevents A -> B -> A. */
static bool git_scroll_syncing;

static void git_scroll_sync(Win *source)
{
    Ed *ed;
    Pane *leaves[YEW_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (source == NULL || source->scroll_link == 0U || git_scroll_syncing ||
        source->buf == NULL || source->buf->owner == NULL)
        return;
    ed = source->buf->owner;
    git_scroll_syncing = true;
    yew_pane_collect_leaves(ed->pane_root, leaves, YEW_PANE_MAX_LEAVES, &n);
    for (i = 0U; i < n; i++) {
        Win *peer = leaves[i]->win;

        if (peer != source && peer->scroll_link == source->scroll_link) {
            peer->vp.top = source->vp.top;
            peer->vp.top_sub = source->vp.top_sub;
            yew_vp_clamp(peer);
        }
    }
    git_scroll_syncing = false;
}

static TextBuf *vp_text(const Win *w)
{
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        YEW_BUG("viewport: missing window buffer");
    return w->buf->tb;
}

static Cursor *vp_cursor(Win *w)
{
    if (w->cs.curs.len == 0U || (size_t)w->cs.primary >= w->cs.curs.len)
        YEW_BUG("viewport: missing primary cursor");
    return &w->cs.curs.data[w->cs.primary];
}

static u64 sat_add(u64 a, u64 b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static u8 text_byte_at(const TextBuf *tb, u64 off)
{
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (!yew_textiter_begin(&it, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&it, tb, &bytes, &len) || len == 0U)
        YEW_BUG("viewport: cannot read buffer byte");
    return bytes[0];
}

static u64 line_content_hi(const TextBuf *tb, LineNo line, Span span)
{
    if (line.v + 1U >= yew_textbuf_line_count(tb) || span.lo == span.hi ||
        text_byte_at(tb, span.hi - 1U) != '\n')
        return span.hi;
    span.hi--;
    if (span.hi > span.lo && text_byte_at(tb, span.hi - 1U) == '\r')
        span.hi--;
    return span.hi;
}

static bool cp_is_cjk(u32 cp)
{
    return (cp >= 0x2E80U && cp <= 0xA4CFU) ||
           (cp >= 0xAC00U && cp <= 0xD7A3U) ||
           (cp >= 0xF900U && cp <= 0xFAFFU) ||
           (cp >= 0x20000U && cp <= 0x3FFFFU);
}

static bool cp_is_space(u32 cp)
{
    return cp == (u32)' ' || cp == (u32)'\t';
}

static bool cp_is_dash_or_slash(u32 cp)
{
    return cp == (u32)'-' || cp == (u32)'/' || cp == 0x2013U ||
           cp == 0x2014U;
}

static u32 cluster_cells(const YewTextCluster *cl, CCol at)
{
    if (cl->tab)
        return yew_tab_cells(at, YEW_VP_TABWIDTH);
    return cl->cells;
}

/* Returns the byte end of one row.  An over-wide first cluster is consumed
 * as a row of its own: this is the width-one progress guarantee. */
static u64 wrap_next(const TextBuf *tb, Span line, u64 start, u16 cols)
{
    u64 pos = start;
    u64 opportunity = start;
    CCol used = {0U};
    u64 limit = cols == 0U ? 1U : (u64)cols;

    while (pos < line.hi) {
        u64 before = pos;
        YewTextCluster cl;
        u32 cells;

        if (!yew_text_cluster_next(tb, line, BYTEOFF(pos), &cl))
            YEW_BUG("viewport: cluster scan ended early");
        pos = cl.bytes.hi;
        if (cp_is_cjk(cl.base_cp) && before > start)
            opportunity = before;
        cells = cluster_cells(&cl, used);
        if (sat_add(used.v, cells) > limit) {
            if (cl.base_cp == (u32)' ') {
                while (pos < line.hi) {
                    YewTextCluster next;

                    if (!yew_text_cluster_next(tb, line, BYTEOFF(pos),
                                               &next))
                        YEW_BUG("viewport: space scan ended early");
                    if (next.base_cp != (u32)' ')
                        break;
                    pos = next.bytes.hi;
                }
                return pos;
            }
            if (opportunity > start)
                return opportunity;
            if (before > start)
                return before;
            return pos;
        }
        used.v += cells;

        if (cp_is_space(cl.base_cp) || cp_is_cjk(cl.base_cp)) {
            opportunity = pos;
        } else if (cp_is_dash_or_slash(cl.base_cp) && pos < line.hi) {
            YewTextCluster next;

            if (!yew_text_cluster_next(tb, line, BYTEOFF(pos), &next))
                YEW_BUG("viewport: dash lookahead ended early");
            if (!cp_is_space(next.base_cp))
                opportunity = pos;
        }
    }
    return line.hi;
}

static u32 wrap_count_raw(const TextBuf *tb, LineNo line, u16 cols)
{
    Span span = yew_textbuf_line_span(tb, line);
    u64 start;
    u32 rows = 0U;

    span.hi = line_content_hi(tb, line, span);
    start = span.lo;
    if (start == span.hi)
        return 1U;
    while (start < span.hi) {
        u64 end = wrap_next(tb, span, start, cols);

        if (end <= start)
            YEW_BUG("viewport: wrap made no progress");
        start = end;
        if (rows == UINT32_MAX)
            YEW_BUG("viewport: too many display rows");
        rows++;
    }
    return rows;
}

static Span wrap_row_raw(const TextBuf *tb, LineNo line, u32 sub,
                         u16 cols)
{
    Span content = yew_textbuf_line_span(tb, line);
    u64 start;
    u32 row = 0U;

    content.hi = line_content_hi(tb, line, content);
    if (content.lo == content.hi)
        return sub == 0U ? content : (Span){content.hi, content.hi};
    start = content.lo;
    while (start < content.hi) {
        u64 end = wrap_next(tb, content, start, cols);

        if (end <= start)
            YEW_BUG("viewport: raw wrap made no progress");
        if (row == sub)
            return (Span){start, end};
        start = end;
        if (row == UINT32_MAX)
            YEW_BUG("viewport: too many raw display rows");
        row++;
    }
    return (Span){content.hi, content.hi};
}

static u32 wrap_subrow_raw(const TextBuf *tb, LineNo line, ByteOff pos,
                           u16 cols, Span *row_span)
{
    Span content = yew_textbuf_line_span(tb, line);
    u64 start;
    u32 sub = 0U;

    content.hi = line_content_hi(tb, line, content);
    if (content.lo == content.hi) {
        *row_span = content;
        return 0U;
    }
    start = content.lo;
    while (start < content.hi) {
        u64 end = wrap_next(tb, content, start, cols);

        if (end <= start)
            YEW_BUG("viewport: raw wrap made no progress");
        if (pos.v < end || end == content.hi) {
            *row_span = (Span){start, end};
            return sub;
        }
        start = end;
        if (sub == UINT32_MAX)
            YEW_BUG("viewport: too many raw display rows");
        sub++;
    }
    YEW_BUG("viewport: position did not map to a raw display row");
}

static void cache_reserve(WrapCache *cache, size_t need)
{
    if (cache->cap >= need)
        return;
    cache->rows = yew_xreallocarray(cache->rows, need, sizeof(*cache->rows));
    cache->cap = need;
}

static void span_cache_reserve(WrapCache *cache, size_t need)
{
    if (cache->spans_cap >= need)
        return;
    cache->spans = yew_xreallocarray(cache->spans, need,
                                     sizeof(*cache->spans));
    cache->spans_cap = need;
}

static void span_cache_fill(Win *w, LineNo line, u32 first)
{
    TextBuf *tb = vp_text(w);
    WrapCache *cache = &w->wrap_cache;
    Span content = yew_textbuf_line_span(tb, line);
    size_t wanted = (size_t)w->vp.rows + 1U;
    u64 start;
    u32 sub = 0U;

    content.hi = line_content_hi(tb, line, content);
    if (wanted == 0U)
        YEW_BUG("viewport: span cache size overflow");
    span_cache_reserve(cache, wanted);
    cache->spans_len = 0U;
    cache->spans_line = line;
    cache->spans_first = first;
    cache->spans_cols = w->vp.cols;
    cache->spans_generation = tb->gen;
    cache->spans_valid = true;
    if (content.lo == content.hi) {
        if (first == 0U) {
            cache->spans[0] = content;
            cache->spans_len = 1U;
        }
        return;
    }
    start = content.lo;
    while (start < content.hi && cache->spans_len < wanted) {
        u64 end = wrap_next(tb, content, start, w->vp.cols);

        if (end <= start)
            YEW_BUG("viewport: wrap made no progress");
        if (sub >= first)
            cache->spans[cache->spans_len++] = (Span){start, end};
        start = end;
        if (sub == UINT32_MAX)
            YEW_BUG("viewport: too many display rows");
        sub++;
    }
}

static void cache_fill(Win *w, LineNo requested)
{
    TextBuf *tb = vp_text(w);
    WrapCache *cache = &w->wrap_cache;
    u64 lines = yew_textbuf_line_count(tb);
    u64 first = w->vp.top.v;
    u64 wanted;
    u64 available;
    size_t i;

    if (requested.v < first || requested.v >= sat_add(first,
            (u64)w->vp.rows + YEW_VP_WRAP_SLACK + 1U))
        first = requested.v;
    if (first >= lines)
        first = lines - 1U;
    wanted = (u64)w->vp.rows + YEW_VP_WRAP_SLACK + 1U;
    available = lines - first;
    if (wanted > available)
        wanted = available;
    if (wanted > (u64)SIZE_MAX)
        YEW_BUG("viewport: wrap cache exceeds address space");
    cache_reserve(cache, (size_t)wanted);
    cache->first = LINENO(first);
    cache->len = (size_t)wanted;
    cache->cols = w->vp.cols;
    cache->tabwidth = YEW_VP_TABWIDTH;
    cache->generation = tb->gen;
    for (i = 0U; i < cache->len; i++) {
        cache->rows[i] = wrap_count_raw(tb, LINENO(first + (u64)i),
                                        w->vp.cols);
    }
    cache->valid = true;
}

void yew_vp_init(Win *w)
{
    if (w == NULL)
        YEW_BUG("viewport init: missing window");
    w->vp.top = LINENO(0U);
    w->vp.top_sub = 0U;
    w->vp.left = (CCol){0U};
    w->vp.rows = 0U;
    w->vp.cols = 0U;
    w->vp.scrolloff = 3U;
    w->vp.sidescrolloff = 5U;
    w->vp.wrap = false;
    w->number_style = YEW_NUM_HYBRID;
    w->gutter_width = 0U;
    memset(&w->wrap_cache, 0, sizeof(w->wrap_cache));
    w->wrap_goal = (CCol){0U};
    w->wrap_goal_valid = false;
}

void yew_vp_free(Win *w)
{
    if (w == NULL)
        return;
    yew_xfree(w->wrap_cache.rows);
    yew_xfree(w->wrap_cache.spans);
    memset(&w->wrap_cache, 0, sizeof(w->wrap_cache));
}

void yew_vp_invalidate(Win *w)
{
    if (w == NULL)
        YEW_BUG("viewport invalidate: missing window");
    w->wrap_cache.valid = false;
    w->wrap_cache.len = 0U;
    w->wrap_cache.spans_valid = false;
    w->wrap_cache.spans_len = 0U;
}

void yew_vp_invalidate_from(Win *w, LineNo line)
{
    WrapCache *cache;
    u64 end;

    if (w == NULL)
        YEW_BUG("viewport invalidate: missing window");
    cache = &w->wrap_cache;
    end = sat_add(cache->first.v, (u64)cache->len);
    if (!cache->valid || line.v < end)
        yew_vp_invalidate(w);
    else if (cache->spans_valid && line.v <= cache->spans_line.v) {
        cache->spans_valid = false;
        cache->spans_len = 0U;
    }
}

u32 yew_wrap_rows(Win *w, LineNo line)
{
    TextBuf *tb = vp_text(w);
    WrapCache *cache = &w->wrap_cache;
    u64 index;

    if (line.v >= yew_textbuf_line_count(tb))
        YEW_BUG("viewport: wrap line out of range");
    if (!w->vp.wrap)
        return 1U;
    if (!cache->valid || cache->cols != w->vp.cols ||
        cache->tabwidth != YEW_VP_TABWIDTH || cache->generation != tb->gen ||
        line.v < cache->first.v ||
        line.v >= sat_add(cache->first.v, (u64)cache->len))
        cache_fill(w, line);
    index = line.v - cache->first.v;
    return cache->rows[(size_t)index];
}

Span yew_wrap_row(Win *w, LineNo line, u32 sub)
{
    TextBuf *tb = vp_text(w);
    WrapCache *cache = &w->wrap_cache;
    Span span;
    u32 count;

    if (line.v >= yew_textbuf_line_count(tb))
        YEW_BUG("viewport: wrap line out of range");
    span = yew_textbuf_line_span(tb, line);
    span.hi = line_content_hi(tb, line, span);
    if (!w->vp.wrap)
        return sub == 0U ? span : (Span){span.hi, span.hi};
    count = yew_wrap_rows(w, line);
    if (sub >= count)
        return (Span){span.hi, span.hi};
    if (!cache->spans_valid || cache->spans_line.v != line.v ||
        cache->spans_cols != w->vp.cols ||
        cache->spans_generation != tb->gen || sub < cache->spans_first ||
        (u64)(sub - cache->spans_first) >= (u64)cache->spans_len)
        span_cache_fill(w, line, sub);
    if (sub < cache->spans_first ||
        (u64)(sub - cache->spans_first) >= (u64)cache->spans_len)
        YEW_BUG("viewport: requested wrap row was not cached");
    return cache->spans[(size_t)(sub - cache->spans_first)];
}

static void top_step_down(Win *w)
{
    u64 lines = yew_textbuf_line_count(vp_text(w));
    u32 count = yew_wrap_rows(w, w->vp.top);

    if (w->vp.top_sub + 1U < count) {
        w->vp.top_sub++;
    } else if (w->vp.top.v + 1U < lines) {
        w->vp.top = LINENO(w->vp.top.v + 1U);
        w->vp.top_sub = 0U;
    }
}

static void top_step_up(Win *w)
{
    if (w->vp.top_sub != 0U) {
        w->vp.top_sub--;
    } else if (w->vp.top.v != 0U) {
        w->vp.top = LINENO(w->vp.top.v - 1U);
        w->vp.top_sub = yew_wrap_rows(w, w->vp.top) - 1U;
    }
}

static void shift_top(Win *w, i64 rows)
{
    while (rows > 0) {
        LineNo old_line = w->vp.top;
        u32 old_sub = w->vp.top_sub;
        top_step_down(w);
        if (old_line.v == w->vp.top.v && old_sub == w->vp.top_sub)
            break;
        rows--;
    }
    while (rows < 0) {
        LineNo old_line = w->vp.top;
        u32 old_sub = w->vp.top_sub;
        top_step_up(w);
        if (old_line.v == w->vp.top.v && old_sub == w->vp.top_sub)
            break;
        rows++;
    }
}

static u32 rows_from_top(Win *w, u32 limit)
{
    u64 lines = yew_textbuf_line_count(vp_text(w));
    LineNo line = w->vp.top;
    u32 sub = w->vp.top_sub;
    u32 total = 0U;

    while (line.v < lines && total < limit) {
        u32 add = yew_wrap_rows(w, line) - sub;
        if (add > limit - total)
            return limit;
        total += add;
        line = LINENO(line.v + 1U);
        sub = 0U;
    }
    return total;
}

bool yew_vp_row_of_line(Win *w, LineNo line, u32 sub, u16 *row)
{
    u64 screen = 0U;
    LineNo scan;
    u32 scan_sub;

    if (row == NULL)
        YEW_BUG("viewport row conversion: missing output");
    if (line.v < w->vp.top.v ||
        (line.v == w->vp.top.v && sub < w->vp.top_sub))
        return false;
    scan = w->vp.top;
    scan_sub = w->vp.top_sub;
    while (scan.v < line.v) {
        screen += (u64)yew_wrap_rows(w, scan) - scan_sub;
        if (screen >= w->vp.rows)
            return false;
        scan = LINENO(scan.v + 1U);
        scan_sub = 0U;
    }
    screen += (u64)sub - scan_sub;
    if (screen >= w->vp.rows || screen > UINT16_MAX)
        return false;
    *row = (u16)screen;
    return true;
}

bool yew_vp_line_of_row(Win *w, u16 row, LineNo *line, u32 *sub)
{
    u64 lines = yew_textbuf_line_count(vp_text(w));
    LineNo scan;
    u32 scan_sub;
    u32 left = row;

    if (line == NULL || sub == NULL)
        YEW_BUG("viewport line conversion: missing output");
    scan = w->vp.top;
    scan_sub = w->vp.top_sub;
    while (scan.v < lines) {
        u32 available = yew_wrap_rows(w, scan) - scan_sub;
        if (left < available) {
            *line = scan;
            *sub = scan_sub + left;
            return true;
        }
        left -= available;
        scan = LINENO(scan.v + 1U);
        scan_sub = 0U;
    }
    return false;
}

/*
 * ABSOLUTE grid x of a content column.
 *
 * `w->rect.x` is the CONTENT origin: yew_layout_win sets it to
 * `leaf->rect.x + gutter`, so the gutter is already inside it and must
 * not be added again.
 *
 * This returned `gutter + relative` until Sprint 25 — right only by
 * coincidence, because with one full-width window leaf->rect.x is 0 and
 * therefore rect.x == gutter.  The moment Sprint 22 gave panes an x
 * offset the coincidence broke, and the one caller, yew_draw_cursor,
 * compares the result against w->rect.x and row_right(), which are
 * absolute.  For any pane not at column 0 the cursor was judged to be
 * outside its own rect and hidden: SPLIT THE WINDOW AND THE CURSOR
 * DISAPPEARS, frozen into s22_split_h.golden and five siblings as
 * `cursor=0,0 vis=0`.
 */
u16 yew_vp_gridx_of_ccol(const Win *w, CCol col)
{
    u64 relative;
    u64 x;

    if (w == NULL)
        YEW_BUG("viewport grid conversion: missing window");
    if (col.v < w->vp.left.v)
        return w->rect.x;
    relative = col.v - w->vp.left.v;
    /* A wrapped row may absorb more trailing whitespace than it has cells.
     * Project those legal byte positions onto the last visible cell so the
     * cursor never disappears beyond the row boundary. */
    if (w->vp.wrap && w->vp.cols != 0U && relative >= w->vp.cols)
        relative = (u64)w->vp.cols - 1U;
    x = (u64)w->rect.x + relative;
    return x > UINT16_MAX ? UINT16_MAX : (u16)x;
}

/*
 * The inverse, and it has to use the same origin.
 *
 * `grid_x` arrives ABSOLUTE — yew_win_click_to_cursor gets it from the
 * mouse and checks grid_y against w->rect.y — so the column to subtract
 * is w->rect.x, which already contains the gutter.  Subtracting
 * gutter_width instead was the same coincidence as its counterpart
 * above: correct only while leaf->rect.x was 0.  In a right-hand pane
 * it made every click land the cursor rect.x - gutter columns too far
 * to the right.
 */
CCol yew_vp_ccol_of_gridx(const Win *w, u16 grid_x)
{
    if (w == NULL)
        YEW_BUG("viewport cell conversion: missing window");
    if (grid_x <= w->rect.x)
        return w->vp.left;
    return (CCol){sat_add(w->vp.left.v, (u64)(grid_x - w->rect.x))};
}

u32 yew_vp_cursor_subrow(Win *w)
{
    TextBuf *tb = vp_text(w);
    WrapCache *cache = &w->wrap_cache;
    Cursor *cursor = vp_cursor(w);
    LineNo line = yew_textbuf_line_of(tb, cursor->pos);
    Span content = yew_textbuf_line_span(tb, line);
    u64 start;
    u32 sub = 0U;
    size_t i;

    if (!w->vp.wrap)
        return 0U;
    content.hi = line_content_hi(tb, line, content);
    if (cache->spans_valid && cache->spans_line.v == line.v &&
        cache->spans_cols == w->vp.cols &&
        cache->spans_generation == tb->gen) {
        for (i = 0U; i < cache->spans_len; i++) {
            Span span = cache->spans[i];

            if (cursor->pos.v >= span.lo &&
                (cursor->pos.v < span.hi || span.hi == content.hi))
                return cache->spans_first + (u32)i;
        }
    }
    start = content.lo;
    if (start == content.hi)
        return 0U;
    while (start < content.hi) {
        u64 end = wrap_next(tb, content, start, w->vp.cols);

        if (cursor->pos.v < end || end == content.hi)
            return sub;
        start = end;
        if (sub == UINT32_MAX)
            YEW_BUG("viewport: too many display rows");
        sub++;
    }
    YEW_BUG("viewport: cursor did not map to a display row");
}

static void follow_vertical(Win *w, LineNo line, u32 sub)
{
    u16 row;
    u16 margin;
    u16 bottom;
    bool visible;

    if (w->vp.rows == 0U) {
        w->vp.top = line;
        w->vp.top_sub = sub;
        return;
    }
    margin = w->vp.scrolloff;
    visible = yew_vp_row_of_line(w, line, sub, &row);
    if ((u32)w->vp.rows <= 2U * (u32)margin) {
        u16 center = (u16)(w->vp.rows / 2U);

        if (!visible) {
            w->vp.top = line;
            w->vp.top_sub = sub;
            shift_top(w, -(i64)center);
        } else if (row < center) {
            shift_top(w, -(i64)(center - row));
        } else if (row > center) {
            shift_top(w, (i64)(row - center));
        }
        return;
    }
    bottom = (u16)(w->vp.rows - 1U - margin);
    if (!visible) {
        bool above = line.v < w->vp.top.v ||
                     (line.v == w->vp.top.v && sub < w->vp.top_sub);
        w->vp.top = line;
        w->vp.top_sub = sub;
        shift_top(w, -(i64)(above ? margin : bottom));
    } else if (row < margin) {
        shift_top(w, -(i64)(margin - row));
    } else if (row > bottom) {
        shift_top(w, (i64)(row - bottom));
    }
}

static void follow_horizontal(Win *w, Span line, ByteOff pos)
{
    TextBuf *tb = vp_text(w);
    CCol start = yew_off_to_ccol(tb, line, pos, YEW_VP_TABWIDTH);
    ByteOff next = pos.v < line_content_hi(tb, yew_textbuf_line_of(tb, pos),
                                           line)
                       ? yew_grapheme_next_boundary(tb, pos)
                       : pos;
    CCol end = yew_off_to_ccol(tb, line, next, YEW_VP_TABWIDTH);
    u64 side = w->vp.sidescrolloff;
    u64 inner;

    if (w->vp.cols == 0U) {
        w->vp.left = start;
        return;
    }
    if (2U * side >= w->vp.cols)
        side = w->vp.cols / 2U;
    if (start.v < sat_add(w->vp.left.v, side))
        w->vp.left.v = start.v > side ? start.v - side : 0U;
    inner = (u64)w->vp.cols - side;
    if (end.v > sat_add(w->vp.left.v, inner))
        w->vp.left.v = end.v > inner ? end.v - inner : 0U;
    if (start.v < w->vp.left.v)
        w->vp.left = start;
}

void yew_vp_follow(Win *w)
{
    TextBuf *tb = vp_text(w);
    Cursor *cursor = vp_cursor(w);
    LineNo line = yew_textbuf_line_of(tb, cursor->pos);
    Span span = yew_textbuf_line_span(tb, line);

    yew_vp_clamp(w);
    follow_vertical(w, line, w->vp.wrap ? yew_vp_cursor_subrow(w) : 0U);
    yew_vp_clamp(w);
    if (w->vp.wrap) {
        w->vp.left = (CCol){0U};
    } else {
        follow_horizontal(w, span, cursor->pos);
    }
}

void yew_vp_scroll(Win *w, i32 rows)
{
    (void)vp_text(w);
    shift_top(w, rows);
    yew_vp_clamp(w);
    git_scroll_sync(w);
}

static void cursor_to_row(Win *w, u16 target)
{
    TextBuf *tb = vp_text(w);
    Cursor *cursor = vp_cursor(w);
    LineNo old_line = yew_textbuf_line_of(tb, cursor->pos);
    u32 old_sub = w->vp.wrap ? yew_vp_cursor_subrow(w) : 0U;
    LineNo line;
    u32 sub;
    Span span;
    ByteOff pos;
    bool unselected = cursor->anchor.v == cursor->pos.v;

    while (!yew_vp_line_of_row(w, target, &line, &sub)) {
        if (target == 0U)
            return;
        target--;
    }
    span = yew_textbuf_line_span(tb, line);
    if (w->vp.wrap) {
        Span old_row = yew_wrap_row(w, old_line, old_sub);
        Span target_row = yew_wrap_row(w, line, sub);
        u32 target_rows = yew_wrap_rows(w, line);

        if (!w->wrap_goal_valid) {
            w->wrap_goal = yew_off_to_ccol(tb, old_row, cursor->pos,
                                           YEW_VP_TABWIDTH);
            w->wrap_goal_valid = true;
        }
        pos = yew_ccol_to_off(tb, target_row, w->wrap_goal,
                              YEW_VP_TABWIDTH);
        if (pos.v == target_row.hi && sub + 1U < target_rows &&
            target_row.lo < target_row.hi)
            pos = yew_grapheme_prev_boundary(tb, pos);
    } else {
        pos = yew_gcol_to_off(tb, span, cursor->goal_col);
    }
    cursor->pos = pos;
    if (w->vp.wrap)
        cursor->goal_col = yew_off_to_gcol(tb, span, pos);
    if (unselected)
        cursor->anchor = pos;
}

void yew_vp_push_cursor(Win *w)
{
    TextBuf *tb = vp_text(w);
    Cursor *cursor = vp_cursor(w);
    LineNo line = yew_textbuf_line_of(tb, cursor->pos);
    u32 sub = w->vp.wrap ? yew_vp_cursor_subrow(w) : 0U;
    u16 row;
    u16 target;
    u16 low;
    u16 high;
    bool visible;

    if (w->vp.rows == 0U)
        return;
    visible = yew_vp_row_of_line(w, line, sub, &row);
    if ((u32)w->vp.rows <= 2U * (u32)w->vp.scrolloff) {
        target = (u16)(w->vp.rows / 2U);
        if (!visible || row != target)
            cursor_to_row(w, target);
        return;
    }
    low = w->vp.scrolloff;
    high = (u16)(w->vp.rows - 1U - w->vp.scrolloff);
    if (visible && row >= low && row <= high)
        return;
    if (!visible) {
        bool above = line.v < w->vp.top.v ||
                     (line.v == w->vp.top.v && sub < w->vp.top_sub);

        target = above ? low : high;
    } else {
        target = row < low ? low : high;
    }
    cursor_to_row(w, target);
}

void yew_vp_page(Win *w, i32 pages)
{
    i64 step = w->vp.rows > 2U ? (i64)w->vp.rows - 2 : 1;
    i64 amount = step * (i64)pages;
    if (amount > INT32_MAX)
        amount = INT32_MAX;
    if (amount < INT32_MIN)
        amount = INT32_MIN;
    yew_vp_scroll(w, (i32)amount);
}

static void place_cursor(Win *w, u16 row)
{
    Cursor *cursor = vp_cursor(w);
    TextBuf *tb = vp_text(w);
    LineNo line = yew_textbuf_line_of(tb, cursor->pos);
    u32 sub = w->vp.wrap ? yew_vp_cursor_subrow(w) : 0U;
    w->vp.top = line;
    w->vp.top_sub = sub;
    shift_top(w, -(i64)row);
}

void yew_vp_center(Win *w)
{
    place_cursor(w, w->vp.rows / 2U);
}

void yew_vp_top(Win *w)
{
    place_cursor(w, 0U);
}

void yew_vp_bottom(Win *w)
{
    place_cursor(w, w->vp.rows == 0U ? 0U : (u16)(w->vp.rows - 1U));
}

void yew_vp_clamp(Win *w)
{
    TextBuf *tb = vp_text(w);
    u64 lines = yew_textbuf_line_count(tb);
    u32 count;

    if (w->vp.top.v >= lines)
        w->vp.top = LINENO(lines - 1U);
    count = yew_wrap_rows(w, w->vp.top);
    if (w->vp.top_sub >= count)
        w->vp.top_sub = count - 1U;
    if (!w->vp.wrap) {
        w->vp.top_sub = 0U;
    } else {
        w->vp.left = (CCol){0U};
    }
    if (w->vp.rows != 0U) {
        u32 available = rows_from_top(w, w->vp.rows);
        if (available < w->vp.rows)
            shift_top(w, -(i64)(w->vp.rows - available));
    }
}

bool yew_vp_move_display(Win *w, i32 rows)
{
    TextBuf *tb = vp_text(w);
    Cursor *cursor = vp_cursor(w);
    LineNo line = yew_textbuf_line_of(tb, cursor->pos);
    u32 sub = yew_vp_cursor_subrow(w);
    Span current = yew_wrap_row(w, line, sub);
    CCol cursor_col = yew_off_to_ccol(tb, current, cursor->pos,
                                      YEW_VP_TABWIDTH);
    i64 remain = rows;
    ByteOff old = cursor->pos;
    bool unselected = cursor->anchor.v == cursor->pos.v;

    if (!w->wrap_goal_valid) {
        w->wrap_goal = cursor_col;
        w->wrap_goal_valid = true;
    }
    while (remain > 0) {
        if (sub + 1U < yew_wrap_rows(w, line))
            sub++;
        else if (line.v + 1U < yew_textbuf_line_count(tb)) {
            line = LINENO(line.v + 1U);
            sub = 0U;
        } else
            break;
        remain--;
    }
    while (remain < 0) {
        if (sub != 0U)
            sub--;
        else if (line.v != 0U) {
            line = LINENO(line.v - 1U);
            sub = yew_wrap_rows(w, line) - 1U;
        } else
            break;
        remain++;
    }
    current = yew_wrap_row(w, line, sub);
    cursor->pos = yew_ccol_to_off(tb, current, w->wrap_goal,
                                  YEW_VP_TABWIDTH);
    if (cursor->pos.v == current.hi &&
        sub + 1U < yew_wrap_rows(w, line) && current.lo < current.hi)
        cursor->pos = yew_grapheme_prev_boundary(tb, cursor->pos);
    if (unselected)
        cursor->anchor = cursor->pos;
    yew_vp_follow(w);
    return cursor->pos.v != old.v;
}

CCol yew_vp_display_col(const Win *w, ByteOff pos)
{
    const TextBuf *tb = vp_text(w);
    LineNo line = yew_textbuf_line_of(tb, pos);
    Span row;

    if (w->vp.wrap)
        (void)wrap_subrow_raw(tb, line, pos, w->vp.cols, &row);
    else
        row = yew_textbuf_line_span(tb, line);
    return yew_off_to_ccol(tb, row, pos, YEW_VP_TABWIDTH);
}

ByteOff yew_vp_display_target(const Win *w, ByteOff pos, i32 rows)
{
    const TextBuf *tb = vp_text(w);
    u64 line_count = yew_textbuf_line_count(tb);
    LineNo line = yew_textbuf_line_of(tb, pos);
    Span current;
    u32 sub = wrap_subrow_raw(tb, line, pos, w->vp.cols, &current);
    CCol goal = w->wrap_goal_valid
                    ? w->wrap_goal
                    : yew_off_to_ccol(tb, current, pos, YEW_VP_TABWIDTH);
    i64 remain = rows;
    Span target;
    ByteOff result;
    u32 target_rows;

    while (remain > 0) {
        u32 count = wrap_count_raw(tb, line, w->vp.cols);

        if (sub + 1U < count) {
            sub++;
        } else if (line.v + 1U < line_count) {
            line = LINENO(line.v + 1U);
            sub = 0U;
        } else {
            break;
        }
        remain--;
    }
    while (remain < 0) {
        if (sub != 0U) {
            sub--;
        } else if (line.v != 0U) {
            line = LINENO(line.v - 1U);
            sub = wrap_count_raw(tb, line, w->vp.cols) - 1U;
        } else {
            break;
        }
        remain++;
    }
    target = wrap_row_raw(tb, line, sub, w->vp.cols);
    target_rows = wrap_count_raw(tb, line, w->vp.cols);
    result = yew_ccol_to_off(tb, target, goal, YEW_VP_TABWIDTH);
    if (result.v == target.hi && sub + 1U < target_rows &&
        target.lo < target.hi)
        result = yew_grapheme_prev_boundary(tb, result);
    return result;
}

LineNo yew_vp_last_visible_line(Win *w)
{
    LineNo line;
    u32 sub;
    u16 row;

    if (w->vp.rows == 0U)
        return w->vp.top;
    row = (u16)(w->vp.rows - 1U);
    if (yew_vp_line_of_row(w, row, &line, &sub))
        return line;
    return LINENO(yew_textbuf_line_count(vp_text(w)) - 1U);
}
