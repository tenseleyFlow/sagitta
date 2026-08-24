#include "mod/plug/overlay.h"

#include <limits.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/handle.h"
#include "fl/vm.h"
#include "mod/plug/internal.h"
#include "syn/attr.h"
#include "text/piece.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/log.h"

typedef struct OverlayRow {
    i64 lo;
    i64 hi;
    i64 attr;
    u8 seen;
} OverlayRow;

enum {
    OVERLAY_SEEN_LO = 1U << 0,
    OVERLAY_SEEN_HI = 1U << 1,
    OVERLAY_SEEN_ATTR = 1U << 2,
    OVERLAY_SEEN_ALL = OVERLAY_SEEN_LO | OVERLAY_SEEN_HI |
                       OVERLAY_SEEN_ATTR
};

static bool key_is(const FlValue *key, const char *text)
{
    const FlStr *str;
    size_t len = strlen(text);

    if (key->t != (u8)FL_STR)
        return false;
    str = (const FlStr *)key->as.o;
    return str->len == len && memcmp(str->b, text, len) == 0;
}

static bool row_read(const FlValue value, OverlayRow *out)
{
    const FlMap *map;
    u32 i;

    if (value.t != (u8)FL_MAP || out == NULL)
        return false;
    *out = (OverlayRow){0};
    map = (const FlMap *)value.as.o;
    for (i = 0U; i < map->n; i++) {
        const FlMapEnt *entry = &map->ent[i];
        i64 *slot;
        u8 bit;

        if (entry->dead)
            continue;
        if (key_is(&entry->k, "lo")) {
            slot = &out->lo;
            bit = OVERLAY_SEEN_LO;
        } else if (key_is(&entry->k, "hi")) {
            slot = &out->hi;
            bit = OVERLAY_SEEN_HI;
        } else if (key_is(&entry->k, "attr")) {
            slot = &out->attr;
            bit = OVERLAY_SEEN_ATTR;
        } else {
            return false;
        }
        if ((out->seen & bit) != 0U || entry->v.t != (u8)FL_INT)
            return false;
        *slot = entry->v.as.i;
        out->seen |= bit;
    }
    return out->seen == OVERLAY_SEEN_ALL;
}

static void overlay_contract_error(Ed *ed, u32 origin, const char *message)
{
    FlVm *vm = yew_fl_vm(ed);

    if (vm == NULL)
        return;
    (void)fl_raise(vm, "type", "%s", message);
    yew_plug_hook_error(ed, origin, vm->err);
}

static i64 line_arg(u64 line)
{
    return line > (u64)INT64_MAX ? INT64_MAX : (i64)line;
}

static bool visible_bounds(const Win *win, LineNo lo_line, LineNo hi_line,
                           u64 *out_lo, u64 *out_hi)
{
    const TextBuf *tb;
    u64 lines;

    if (win == NULL || win->buf == NULL || win->buf->tb == NULL ||
        out_lo == NULL || out_hi == NULL)
        return false;
    tb = win->buf->tb;
    lines = yew_textbuf_line_count(tb);
    if (lo_line.v >= lines || hi_line.v <= lo_line.v)
        return false;
    if (hi_line.v > lines)
        hi_line.v = lines;
    *out_lo = yew_textbuf_line_start(tb, lo_line).v;
    *out_hi = hi_line.v == lines ? yew_textbuf_len(tb) :
                                   yew_textbuf_line_start(tb, hi_line).v;
    return true;
}

static bool parse_result(Ed *ed, PlugValueReg *reg, Win *win,
                         LineNo lo_line, LineNo hi_line, FlValue result,
                         YewPlugOverlayVisit visit, void *ctx)
{
    FlList *list;
    u64 clip_lo;
    u64 clip_hi;
    u32 i;
    bool clipped = false;

    if (result.t != (u8)FL_LIST) {
        overlay_contract_error(ed, reg->origin_id,
                               "plugin overlay must return a list of "
                               "{lo, hi, attr} maps");
        return false;
    }
    if (!visible_bounds(win, lo_line, hi_line, &clip_lo, &clip_hi))
        return true;
    list = (FlList *)result.as.o;
    for (i = 0U; i < list->n; i++) {
        OverlayRow row;
        u64 lo;
        u64 hi;

        if (!row_read(list->v[i], &row) || row.attr < 0 ||
            row.attr >= (i64)YEW_ATTR__COUNT) {
            overlay_contract_error(ed, reg->origin_id,
                                   "plugin overlay entries must be maps "
                                   "with integer lo, hi, and valid attr");
            return false;
        }
        lo = row.lo < 0 ? 0U : (u64)row.lo;
        hi = row.hi < 0 ? 0U : (u64)row.hi;
        if (lo < clip_lo) {
            lo = clip_lo;
            clipped = true;
        }
        if (hi > clip_hi) {
            hi = clip_hi;
            clipped = true;
        }
        if (lo >= hi) {
            clipped = true;
            continue;
        }
        if (!yew_is_grapheme_boundary(win->buf->tb, BYTEOFF(lo)) ||
            !yew_is_grapheme_boundary(win->buf->tb, BYTEOFF(hi))) {
            clipped = true;
            continue;
        }
        visit(ctx, (Span){lo, hi}, (u8)row.attr);
    }
    if (clipped) {
        Plug *plug = yew_plug_by_origin(ed, reg->origin_id);

        yew_log(YEW_LOG_WARN, "plugin \"%s\" overlay span clipped or dropped",
                plug == NULL || plug->mf.name_text == NULL ? "?" :
                                                            plug->mf.name_text);
    }
    return true;
}

void yew_plug_overlay_run(Ed *ed, Win *win, LineNo lo_line, LineNo hi_line,
                          YewPlugOverlayVisit visit, void *ctx)
{
    PlugSys *sys;
    FlVm *vm;
    FlValue args[4];
    u32 nregs;
    u32 i;

    if (ed == NULL || win == NULL || win->buf == NULL || visit == NULL ||
        ed->plug == NULL || (vm = yew_fl_vm(ed)) == NULL)
        return;
    sys = ed->plug;
    args[0] = fl_h_win_make(ed, win);
    args[1] = fl_h_buf_make(ed, win->buf);
    args[2] = FL_INT_V(line_arg(lo_line.v));
    args[3] = FL_INT_V(line_arg(hi_line.v));
    nregs = sys->nregs;
    for (i = 0U; i < nregs; i++) {
        PlugValueReg *reg = &sys->regs[i];
        PlugValueReg snapshot;
        Plug *plug;
        FlValue result = FL_NIL_V;

        if (!reg->active || reg->kind != (u8)REG_OVERLAY ||
            fl_origin_masked(ed, reg->origin_id))
            continue;
        plug = yew_plug_by_origin(ed, reg->origin_id);
        if (plug == NULL || plug->st != PLUG_ENABLED)
            continue;
        snapshot = *reg;
        if (!fl_call_value_args(ed->fl, snapshot.value, args, 4U,
                                YEW_SRC_FLETCH, &result)) {
            yew_plug_hook_error(ed, snapshot.origin_id, vm->err);
            if (sys->pending_disable_origin != 0U)
                break;
            continue;
        }
        (void)parse_result(ed, &snapshot, win, lo_line, hi_line, result,
                           visit, ctx);
        if (sys->pending_disable_origin != 0U)
            break;
    }
    yew_plug_drain_pending(ed);
}
