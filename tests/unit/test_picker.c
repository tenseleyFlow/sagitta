/*
 * Sprint 26 §5: the one list picker.
 *
 * THE TEST THAT MATTERS IS THE FIRST ONE.  Selection held by row index
 * instead of payload is the most common bug in hand-rolled pickers, and
 * it is silent: one more character reorders the list, the highlight
 * stays on row 2, and row 2 is now a different file.  The user presses
 * Enter, opens the wrong thing, and blames their own typing.
 *
 * So the fixture is built so that a row-index implementation would
 * PASS every other assertion here and fail only this one — the ordering
 * changes under the selection rather than around it.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "term/input.h"
#include "ui/layout.h"
#include "ui/picker.h"
#include "util/arena.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct PkFix {
    Ed ed;
    const PickItem *items;
    u32 n;
} PkFix;

static PkFix *g_fix;

static const PickItem *pk_items(void *ctx, u32 *n)
{
    PkFix *f = ctx;

    *n = f->n;
    return f->items;
}

static i32 g_accepted;
static u8 g_accept_how;

static bool pk_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    (void)ed;
    (void)ctx;
    g_accepted = payload;
    g_accept_how = how;
    return true;
}

static void pk_make(PkFix *f)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    SAG_ASSERT(sag_grid_init(&f->ed.grid, &f->ed.interner, 24U, 80U));
    f->ed.grid_ready = true;
    sag_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    g_fix = f;
    g_accepted = -1;
    g_accept_how = 0xFFU;
}

static void pk_remove(PkFix *f)
{
    sag_picker_close(&f->ed, false);
    sag_ed_free(&f->ed);
    g_fix = NULL;
}

static void pk_open(PkFix *f, const PickItem *items, u32 n, bool path_mode)
{
    PickerSpec spec;

    f->items = items;
    f->n = n;
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Test";
    spec.items = pk_items;
    spec.accept = pk_accept;
    spec.path_mode = path_mode;
    spec.ctx = f;
    sag_picker_open(&f->ed, &spec);
}

/* Types one printable byte into the filter line. */
static void pk_type(PkFix *f, char c)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)(u8)c;
    k.text[0] = (u8)c;
    k.ntext = 1U;
    SAG_ASSERT(sag_picker_key(&f->ed, &k));
}

static void pk_press(PkFix *f, u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = code;
    SAG_ASSERT(sag_picker_key(&f->ed, &k));
}

/* ---------------------------------------------------------------- */
/* THE law: selection by payload                                    */
/* ---------------------------------------------------------------- */

/*
 * Typing a character reorders the list; the selection follows the ITEM,
 * not the row.
 *
 * The fixture is chosen so a row-index picker looks fine: before the
 * keystroke `beta.c` is at row 1, and after it row 1 holds something
 * else entirely.  A row-index implementation would report the wrong
 * payload here and pass every other test in this file.
 */
void test_picker_selection_survives_a_refilter(void)
{
    static const PickItem items[] = {
        {"alpha.c", NULL, 100, 0U},
        {"beta.c", NULL, 200, 0U},
        {"gamma_b.c", NULL, 300, 0U},
        {"delta_b.c", NULL, 400, 0U}
    };
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 4U, true);
    SAG_ASSERT_EQ_U64(sag_picker_shown(&f.ed), 4U);

    /* Select `beta.c`, which is at row 1 with an empty filter. */
    pk_press(&f, SAG_KEY_DOWN);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 200);

    /* `b` reorders: beta.c scores as a prefix-basename match and rises
     * to row 0, while the two `_b` files rank below it. */
    pk_type(&f, 'b');
    SAG_ASSERT(sag_picker_shown(&f.ed) >= 1U);
    /* The SAME item is still selected, wherever it now sits. */
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 200);

    /* And Enter opens the item that was highlighted, not a row. */
    pk_press(&f, SAG_KEY_ENTER);
    SAG_ASSERT_EQ_I64(g_accepted, 200);
    pk_remove(&f);
}

/*
 * When the held item is filtered OUT, selection falls to row 0 —
 * visibly, at the top, rather than silently onto whatever slid into its
 * old position.
 */
void test_picker_selection_falls_to_row_zero_when_filtered_out(void)
{
    static const PickItem items[] = {
        {"alpha.c", NULL, 100, 0U},
        {"zulu.c", NULL, 200, 0U},
        {"alpine.c", NULL, 300, 0U}
    };
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 3U, true);
    pk_press(&f, SAG_KEY_DOWN);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 200);

    /* `alp` excludes zulu.c entirely. */
    pk_type(&f, 'a');
    pk_type(&f, 'l');
    pk_type(&f, 'p');
    SAG_ASSERT(sag_picker_shown(&f.ed) > 0U);
    /* Whatever is now at the top — never the vanished item. */
    SAG_ASSERT(sag_picker_selected(&f.ed) != 200);
    pk_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Movement                                                         */
/* ---------------------------------------------------------------- */

void test_picker_movement_keys(void)
{
    static const PickItem items[] = {
        {"a", NULL, 1, 0U}, {"b", NULL, 2, 0U}, {"c", NULL, 3, 0U},
        {"d", NULL, 4, 0U}, {"e", NULL, 5, 0U}
    };
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 5U, false);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 1);
    pk_press(&f, SAG_KEY_DOWN);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 2);
    pk_press(&f, SAG_KEY_END);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 5);
    pk_press(&f, SAG_KEY_HOME);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 1);
    /* Past the ends: clamped, never wrapped — a list that wraps makes
     * "hold down the arrow" unable to reach an end. */
    pk_press(&f, SAG_KEY_UP);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 1);
    pk_press(&f, SAG_KEY_END);
    pk_press(&f, SAG_KEY_DOWN);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 5);
    pk_remove(&f);
}

/* C-n and C-p, for terminals that eat arrows (invariant 9). */
void test_picker_ctrl_chords_move(void)
{
    static const PickItem items[] = {
        {"a", NULL, 1, 0U}, {"b", NULL, 2, 0U}, {"c", NULL, 3, 0U}
    };
    PkFix f;
    Key k;

    pk_make(&f);
    pk_open(&f, items, 3U, false);
    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)'n';
    k.mods = SAG_MOD_CTRL;
    SAG_ASSERT(sag_picker_key(&f.ed, &k));
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 2);
    k.code = (u32)'p';
    SAG_ASSERT(sag_picker_key(&f.ed, &k));
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 1);
    pk_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Counts, filtering, lifecycle                                     */
/* ---------------------------------------------------------------- */

/* The footer's numbers: shown out of total. */
void test_picker_counts_shown_and_total(void)
{
    static const PickItem items[] = {
        {"alpha.c", NULL, 1, 0U},
        {"beta.c", NULL, 2, 0U},
        {"gamma.h", NULL, 3, 0U}
    };
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 3U, true);
    SAG_ASSERT_EQ_U64(sag_picker_shown(&f.ed), 3U);
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 3U);
    pk_type(&f, 'h');
    /* Total never changes with the filter — it is the candidate count,
     * which is what makes "3/1043" meaningful. */
    SAG_ASSERT_EQ_U64(sag_picker_total(&f.ed), 3U);
    SAG_ASSERT(sag_picker_shown(&f.ed) < 3U);
    pk_remove(&f);
}

/* An empty list is not a crash, and Enter on it does nothing. */
void test_picker_empty_list_is_safe(void)
{
    PkFix f;

    pk_make(&f);
    pk_open(&f, NULL, 0U, false);
    SAG_ASSERT_EQ_U64(sag_picker_shown(&f.ed), 0U);
    SAG_ASSERT_EQ_I64(sag_picker_selected(&f.ed), 0);
    pk_press(&f, SAG_KEY_DOWN);
    pk_press(&f, SAG_KEY_ENTER);
    /* accept was never called. */
    SAG_ASSERT_EQ_I64(g_accepted, -1);
    SAG_ASSERT(sag_picker_active(&f.ed));
    pk_remove(&f);
}

/* Escape cancels without accepting. */
void test_picker_escape_cancels(void)
{
    static const PickItem items[] = {{"a", NULL, 7, 0U}};
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 1U, false);
    SAG_ASSERT(sag_picker_active(&f.ed));
    pk_press(&f, SAG_KEY_ESCAPE);
    SAG_ASSERT(!sag_picker_active(&f.ed));
    SAG_ASSERT_EQ_I64(g_accepted, -1);
    pk_remove(&f);
}

/*
 * Law 3: an open picker SWALLOWS every key it does not use.
 *
 * A picker that let `d` through would delete a line behind the dialog,
 * which is the kind of bug nobody reports because nobody believes it.
 */
void test_picker_swallows_unhandled_keys(void)
{
    static const PickItem items[] = {{"a", NULL, 7, 0U}};
    PkFix f;
    Key k;

    pk_make(&f);
    pk_open(&f, items, 1U, false);
    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)SAG_KEY_F1;
    /* Consumed, even though the picker does nothing with it. */
    SAG_ASSERT(sag_picker_key(&f.ed, &k));
    pk_remove(&f);

    /* And a CLOSED picker claims nothing. */
    SAG_ASSERT(!sag_picker_key(&f.ed, &k));
}

/*
 * The filter line is the s18 widget, opened LAZILY on the first key or
 * draw.
 *
 * Not at open time: a picker is normally launched from `:find`, and
 * that prompt is still up while the command runs — so a filter line
 * opened inside the command was closed again the moment the command
 * returned, leaving a picker nobody could type into.  The guarantee is
 * therefore "by the time you can interact with it", not "immediately".
 */
void test_picker_filter_line_is_the_cmdline(void)
{
    static const PickItem items[] = {{"alpha", NULL, 1, 0U}};
    PkFix f;

    pk_make(&f);
    SAG_ASSERT(!f.ed.cmdline.active);
    pk_open(&f, items, 1U, false);
    /* A draw is enough to bring it up... */
    sag_picker_draw(&f.ed, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(f.ed.cmdline.active);
    sag_picker_close(&f.ed, false);

    /* ...and so is a key, for a picker opened and typed into without an
     * intervening frame. */
    pk_open(&f, items, 1U, false);
    SAG_ASSERT(!f.ed.cmdline.active);
    pk_type(&f, 'a');
    SAG_ASSERT(f.ed.cmdline.active);
    /* Typing reaches it, and the text comes back out of it. */
    {
        Bytebuf text;

        bytebuf_init(&text);
        sag_cmdline_text(&f.ed, &text);
        SAG_ASSERT_EQ_U64(text.len, 1U);
        SAG_ASSERT_EQ_I64(text.data[0], 'a');
        bytebuf_free(&text);
    }
    sag_picker_close(&f.ed, false);
    SAG_ASSERT(!f.ed.cmdline.active);
    pk_remove(&f);
}

/*
 * A filter change of the same LENGTH still re-ranks.
 *
 * Comparing lengths instead of bytes would leave `ba` showing `ab`'s
 * results — the first version of this code did exactly that.
 */
void test_picker_same_length_filter_change_refilters(void)
{
    static const PickItem items[] = {
        {"ab_first", NULL, 1, 0U},
        {"ba_second", NULL, 2, 0U}
    };
    PkFix f;
    i32 with_ab;
    i32 with_ba;

    pk_make(&f);
    pk_open(&f, items, 2U, false);
    pk_type(&f, 'a');
    pk_type(&f, 'b');
    with_ab = sag_picker_selected(&f.ed);
    /* Backspace twice, then type the reverse. */
    pk_press(&f, SAG_KEY_BACKSPACE);
    pk_press(&f, SAG_KEY_BACKSPACE);
    pk_type(&f, 'b');
    pk_type(&f, 'a');
    with_ba = sag_picker_selected(&f.ed);
    SAG_ASSERT(with_ab != with_ba);
    pk_remove(&f);
}

/* Accepting in a split reports HOW, so the instance can open there. */
void test_picker_accept_in_split_reports_how(void)
{
    static const PickItem items[] = {{"a", NULL, 42, 0U}};
    PkFix f;
    Key k;

    pk_make(&f);
    pk_open(&f, items, 1U, false);
    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)'v';
    k.mods = SAG_MOD_CTRL;
    SAG_ASSERT(sag_picker_key(&f.ed, &k));
    SAG_ASSERT_EQ_I64(g_accepted, 42);
    SAG_ASSERT_EQ_I64(g_accept_how, SAG_PICK_ACCEPT_VSPLIT);
    /* The split really happened. */
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(f.ed.pane_root), 2U);
    pk_remove(&f);
}

/*
 * Refusal under a tiny terminal, with a message — a six-cell dialog is
 * not a degraded experience, it is a broken one.
 */
void test_picker_refuses_a_tiny_terminal(void)
{
    static const PickItem items[] = {{"a", NULL, 1, 0U}};
    PkFix f;
    PickerSpec spec;

    pk_make(&f);
    sag_grid_free(&f.ed.grid);
    SAG_ASSERT(sag_grid_init(&f.ed.grid, &f.ed.interner, 4U, 20U));
    f.items = items;
    f.n = 1U;
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Tiny";
    spec.items = pk_items;
    spec.ctx = &f;
    sag_picker_open(&f.ed, &spec);
    SAG_ASSERT(!sag_picker_active(&f.ed));
    SAG_ASSERT(f.ed.msg.active);
    pk_remove(&f);
}

/* Drawing does not crash, and stays inside the grid. */
void test_picker_draw_fits_the_grid(void)
{
    static const PickItem items[] = {
        {"src/ui/tabs.c", "src/ui", 1, SAG_PICK_MODIFIED},
        {"src/ui/tabs.h", "src/ui", 2, 0U},
        {"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.c", "cjk", 3, 0U}
    };
    PkFix f;

    pk_make(&f);
    pk_open(&f, items, 3U, true);
    sag_picker_draw(&f.ed, (Rect){0U, 0U, 80U, 24U});
    /* And in a narrow box, where the detail column is dropped. */
    sag_picker_draw(&f.ed, (Rect){0U, 0U, 30U, 12U});
    pk_remove(&f);
}
