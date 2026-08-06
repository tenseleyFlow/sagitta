#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 27 fuzz: the mouse router, fed through the real decoder.
 *
 * WHAT IS BEING DEFENDED.  A mouse event stream is written by the
 * terminal, not by us, and terminals send things the protocol does not
 * quite allow: a release with no press (after a focus change), a press
 * with no release (the window manager took the button), wheel bursts,
 * X10-encoded events from an emulator that never honoured 1006, and
 * coordinates outside a grid that has since been resized.  Three
 * failure modes matter and none of them announces itself:
 *
 *   THE PHASE MACHINE STICKS.  A drag that never sees its release
 *   leaves the router holding a button forever, and every subsequent
 *   click is read as the drop of a gesture the user abandoned minutes
 *   ago.  Sprint 4 pinned the wheel case specifically; this fuzzer
 *   asserts the general one, by driving the stream to a quiescent state
 *   and requiring SAG_MP_IDLE.
 *
 *   A MUTATION WITHOUT A RELEASE.  Nothing in Tabs.v may change while a
 *   drag is in flight — that is the whole reason the preview is a
 *   picture.  So the tab order is snapshotted at every press and
 *   compared at every motion event.
 *
 *   A CLICK RESOLVED AGAINST A FREED TAB.  Ids are monotonic and never
 *   reused, so a stale one must resolve to NOTHING.  The session closes
 *   tabs underneath live gestures and requires the active index to stay
 *   in range.
 *
 * The bytes go through sag_input_next rather than being hand-built into
 * Key structs: a fuzzer that skipped the decoder would be testing a
 * shape the terminal never produces.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "term/input.h"
#include "ui/ctxmenu.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/tabs.h"

typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

enum {
    FZ_ROWS = 24,
    FZ_COLS = 80,
    FZ_TABS = 6,
    /* Per session; four sessions per case gives the 100 000 the sprint
     * asks for across a default run. */
    FZ_EVENTS = 25000
};

/* ---------------------------------------------------------------- */
/* The event stream                                                 */
/* ---------------------------------------------------------------- */

/*
 * One SGR (1006) report.  `cb` carries the button and modifier bits
 * exactly as a terminal encodes them, including the 32 (motion) and 64
 * (wheel) offsets, so the decoder's own classification is what the
 * router sees.
 */
static void emit_sgr(Bytebuf *out, u32 cb, u32 x, u32 y, bool release)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "\x1b[<%u;%u;%u%c", cb, x, y,
                     release ? 'm' : 'M');

    if (n > 0)
        bytebuf_append(out, (const u8 *)buf, (size_t)n);
}

/*
 * An X10 report.  Still in the wild, and its coordinates saturate at
 * 223 — which is exactly the kind of edge that makes a hand-written
 * decoder read one byte past the end.
 */
static void emit_x10(Bytebuf *out, u32 cb, u32 x, u32 y)
{
    u8 buf[6];

    buf[0] = 0x1BU;
    buf[1] = (u8)'[';
    buf[2] = (u8)'M';
    buf[3] = (u8)(32U + (cb & 0xFFU));
    buf[4] = (u8)(32U + (x & 0xFFU));
    buf[5] = (u8)(32U + (y & 0xFFU));
    bytebuf_append(out, buf, sizeof(buf));
}

/* Coordinates: mostly on the grid, sometimes far outside it. */
static u32 fz_coord(Rng *r, u32 limit)
{
    u32 pick = rng_below(r, 16U);

    if (pick == 0U)
        return 0U;                       /* below the 1-based origin */
    if (pick == 1U)
        return limit + 1U + rng_below(r, 4U);
    if (pick == 2U)
        return 60000U + rng_below(r, 6000U); /* far outside */
    return 1U + rng_below(r, limit);
}

static void emit_random_event(Rng *r, Bytebuf *out)
{
    u32 shape = rng_below(r, 100U);
    u32 x = fz_coord(r, FZ_COLS);
    u32 y = fz_coord(r, FZ_ROWS);
    u32 mods = rng_below(r, 8U) * 4U; /* shift/alt/ctrl bits */

    if (shape < 25U) {
        emit_sgr(out, 0U | mods, x, y, false); /* left press */
    } else if (shape < 45U) {
        emit_sgr(out, 32U | mods, x, y, false); /* motion, button held */
    } else if (shape < 60U) {
        /* A release, often with no matching press. */
        emit_sgr(out, rng_below(r, 3U) | mods, x, y, true);
    } else if (shape < 78U) {
        /* Wheel bursts: several notches with no release between. */
        u32 n = 1U + rng_below(r, 6U);
        u32 i;

        for (i = 0U; i < n; i++)
            emit_sgr(out, 64U + rng_below(r, 4U) + mods, x, y, false);
    } else if (shape < 86U) {
        emit_sgr(out, 2U | mods, x, y, false); /* right press */
    } else if (shape < 92U) {
        emit_sgr(out, 1U | mods, x, y, false); /* middle press */
    } else if (shape < 97U) {
        emit_x10(out, rng_below(r, 96U), x, y);
    } else {
        /* Malformed: a truncated SGR introducer, which the decoder has
         * to hold or reject rather than read past. */
        bytebuf_append(out, (const u8 *)"\x1b[<", 3U);
    }
}

/* ---------------------------------------------------------------- */
/* The editor under test                                            */
/* ---------------------------------------------------------------- */

typedef struct Snap {
    u32 id[FZ_TABS + 2];
    u32 n;
} Snap;

static Snap snap_take(Ed *ed)
{
    Snap s;
    u32 i;

    (void)memset(&s, 0, sizeof(s));
    s.n = sag_tab_count(ed);
    if (s.n > (u32)SAG_ARRAY_LEN(s.id))
        s.n = (u32)SAG_ARRAY_LEN(s.id);
    for (i = 0U; i < s.n; i++)
        s.id[i] = sag_tab_at(ed, (int)i)->tab_id;
    return s;
}

static bool snap_eq(const Snap *a, const Snap *b)
{
    return a->n == b->n && memcmp(a->id, b->id, sizeof(a->id)) == 0;
}

/* A frame, so the region table and the strip's slot table are live.  A
 * router queried against an empty table is not being tested. */
static void fz_paint(Ed *ed)
{
    sag_region_frame_begin();
    if (ed->layout_dirty)
        sag_ed_layout(ed);
    sag_tab_strip_draw(ed, ed->tab_strip_rect);
    {
        i32 leaf;

        sag_pane_tables_reset(ed);
        leaf = sag_pane_table_add_leaf(ed, ed->pane_root);
        sag_region_add(SAG_REGION_PANE, ed->pane_root->rect, leaf);
    }
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Ed ed;
    In in;
    TtyCaps caps;
    Rng rng;
    Bytebuf stream;
    Snap held;
    bool holding = false;
    size_t k;
    u32 event = 0U;
    bool ok = true;
    int i;

    rng.s = 0x243F6A8885A308D3ULL;
    for (k = 0U; k < len; k++)
        rng.s = rng.s * 31U + data[k];

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed) ||
        !sag_grid_init(&ed.grid, &ed.interner, FZ_ROWS, FZ_COLS)) {
        (void)snprintf(why, why_cap, "could not build an editor");
        sag_ed_free(&ed);
        return false;
    }
    ed.grid_ready = true;
    for (i = 0; i < FZ_TABS; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/sag-fzmouse-%d.txt", i);
        (void)sag_tab_open(&ed, path);
    }
    /* A group, so the negative-payload path and the dwell are both
     * reachable from random coordinates. */
    {
        u32 g = sag_group_create(&ed, "/src", "grp");

        sag_group_add_member(&ed, g, 4);
        sag_group_add_member(&ed, g, 5);
    }
    sag_tab_switch(&ed, 0);
    sag_ed_layout(&ed);
    ed.now_ms = 1000;

    (void)memset(&caps, 0, sizeof(caps));
    sag_input_init(&in, &caps);
    bytebuf_init(&stream);

    while (ok && event < FZ_EVENTS) {
        Key key;

        stream.len = 0U;
        emit_random_event(&rng, &stream);
        sag_input_feed(&in, stream.data, stream.len);
        fz_paint(&ed);
        while (sag_input_next(&in, ed.now_ms, &key)) {
            if (key.kind != (u16)SAG_EV_MOUSE)
                continue;
            event++;
            /*
             * The freeze from §5 must never be left on: a row action
             * that returned early with the table frozen would abort the
             * next hit-test, and the abort would be blamed on the
             * click rather than on the row.
             */
            if (sag_region_frozen()) {
                (void)snprintf(why, why_cap,
                               "region table left frozen after event %u",
                               (unsigned)event);
                ok = false;
                break;
            }
            if (key.ev == (u8)SAG_KEY_PRESS &&
                key.button == (u8)SAG_MB_LEFT) {
                held = snap_take(&ed);
                holding = true;
            }
            sag_mouse_event(&ed, &key);
            /* Wheels are impulses: they never leave a button down. */
            if (key.button >= (u8)SAG_MB_WHEEL_UP &&
                key.button <= (u8)SAG_MB_WHEEL_RIGHT &&
                ed.mouse.phase == SAG_MP_IDLE && ed.mouse.held != 0U) {
                (void)snprintf(why, why_cap,
                               "a wheel notch left a button held");
                ok = false;
                break;
            }
            /* Nothing in Tabs.v moves while a drag is in flight. */
            if (holding && key.ev == (u8)SAG_KEY_REPEAT &&
                (ed.mouse.phase == SAG_MP_DRAG_TAB ||
                 ed.mouse.phase == SAG_MP_DRAG_GROUP)) {
                Snap now = snap_take(&ed);

                if (!snap_eq(&held, &now)) {
                    (void)snprintf(why, why_cap,
                                   "the tab array moved during a drag");
                    ok = false;
                    break;
                }
            }
            if (key.ev == (u8)SAG_KEY_RELEASE)
                holding = false;
            /* The active index stays in range, whatever the stream did
             * to the tab set. */
            if (ed.tabs.active < -1 ||
                ed.tabs.active >= (int)sag_tab_count(&ed)) {
                (void)snprintf(why, why_cap,
                               "active tab index %d out of range (%u tabs)",
                               ed.tabs.active,
                               (unsigned)sag_tab_count(&ed));
                ok = false;
                break;
            }
            /* Every Rect the layout produced stays inside the grid. */
            if ((u32)ed.tab_strip_rect.y + ed.tab_strip_rect.h >
                    (u32)ed.grid.rows ||
                (u32)ed.tab_strip_rect.x + ed.tab_strip_rect.w >
                    (u32)ed.grid.cols) {
                (void)snprintf(why, why_cap, "the strip left the grid");
                ok = false;
                break;
            }
            ed.now_ms += 1 + (i64)rng_below(&rng, 200U);
            sag_mouse_tick(&ed, ed.now_ms);
            /* Menus opened by right-clicks are closed again, so the
             * next iteration starts from a comparable state. */
            if (sag_ctx_active())
                sag_ctx_close();
            /* Occasionally close a tab under the gesture: a click
             * resolved against a freed tab must find nothing. */
            if (rng_below(&rng, 512U) == 0U && sag_tab_count(&ed) > 1U)
                (void)sag_tab_close(&ed, (int)rng_below(
                                             &rng, sag_tab_count(&ed)));
        }
    }

    /*
     * Quiescence.  Whatever the stream left behind, an explicit cancel
     * — which is what Esc and FOCUS_OUT do — must return the router to
     * idle with no button held.  A machine that could not be reset from
     * outside would be stuck for the session's lifetime.
     */
    if (ok) {
        sag_mouse_cancel(&ed);
        if (ed.mouse.phase != SAG_MP_IDLE || ed.mouse.held != 0U) {
            (void)snprintf(why, why_cap,
                           "cancel left phase=%d held=%u",
                           (int)ed.mouse.phase,
                           (unsigned)ed.mouse.held);
            ok = false;
        }
    }
    if (ok && event < FZ_EVENTS / 4U) {
        /* The fuzzer must not be able to pass by doing nothing. */
        (void)snprintf(why, why_cap,
                       "only %u events reached the router", (unsigned)event);
        ok = false;
    }

    bytebuf_free(&stream);
    sag_input_free(&in);
    sag_ctx_close();
    sag_ed_free(&ed);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_mouse", NULL, run_session);
}
