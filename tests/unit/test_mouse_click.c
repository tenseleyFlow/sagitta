/*
 * Sprint 27 §6: double and triple click, through the Sprint 16 unit
 * engines.
 *
 * THE POINT OF ROUTING THROUGH THE ENGINES is that the mouse and the
 * keyboard agree on what a word is.  A double-click that selects `foo`
 * where `W`+`H`+`→` selects `foo.bar` is a bug users cannot name and
 * always feel.
 *
 * So the central test here does not assert hand-written offsets: it
 * asserts that the selection a double-click produces EQUALS
 * `yew_unit_word.span` over Sprint 16's own conformance corpus — CJK, a
 * ZWJ family, `don't`, `foo_bar`, `1,000.50`, an invalid-byte run.  The
 * agreement IS the test, and it is what makes double-click correct for
 * all of those for free.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/pane_cmds.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/viewport.h"
#include "ui/win.h"

typedef struct ClickFixture {
    Ed ed;
    i32 leaf;
} ClickFixture;

static void ck_fixture(ClickFixture *f, const char *text)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_grid_init(&f->ed.grid, &f->ed.interner, 24U, 80U));
    f->ed.grid_ready = true;
    if (text != NULL && text[0] != '\0') {
        EditCtx ec = yew_ed_edit_ctx(&f->ed);

        (void)yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)text,
                              strlen(text));
        yew_ed_finish_edit(&f->ed, &ec);
    }
    yew_ed_layout(&f->ed);
    yew_ed_cursor(&f->ed)->pos = BYTEOFF(0U);
    yew_ed_cursor(&f->ed)->anchor = BYTEOFF(0U);
    f->ed.now_ms = 1000;
    yew_pane_tables_reset(&f->ed);
    f->leaf = yew_pane_table_add_leaf(&f->ed, f->ed.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, f->ed.pane_root->rect, f->leaf);
}

static Key ck_ev(u8 ev, u16 x, u16 y, u16 mods)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_MOUSE;
    k.button = (u8)YEW_MB_LEFT;
    k.ev = ev;
    k.col = x;
    k.row = y;
    k.mods = mods;
    return k;
}

/* One press+release at (x, y), at the fixture's current clock. */
static void ck_click(ClickFixture *f, u16 x, u16 y, u16 mods)
{
    Key press = ck_ev((u8)YEW_KEY_PRESS, x, y, mods);
    Key up = ck_ev((u8)YEW_KEY_RELEASE, x, y, mods);

    yew_mouse_event(&f->ed, &press);
    yew_mouse_event(&f->ed, &up);
}

/*
 * The screen column that shows text column `n`.
 *
 * NOT `n` itself: w->rect.x is the CONTENT origin and already includes
 * the gutter (the Sprint 22 law, and the bug that hid the cursor in
 * every pane but the leftmost for four sprints).  A test that clicked
 * at raw column n would land in the gutter and then assert against
 * offsets it never actually pointed at.
 */
static u16 ck_col(ClickFixture *f, u16 n)
{
    return (u16)(f->ed.win->rect.x + n);
}

static Span ck_selection(ClickFixture *f)
{
    Win *w = f->ed.win;

    return yew_sel_span(w, &w->cs.curs.data[w->cs.primary]);
}

/* ---------------------------------------------------------------- */
/* The counter                                                       */
/* ---------------------------------------------------------------- */

/*
 * The window is exclusive at its edge: 399 ms still counts, 401 ms does
 * not.  Driven with an injected clock, because a test that slept would
 * be a flake generator on a loaded machine.
 */
void test_mouse_click_window_is_399_yes_401_no(void)
{
    ClickFixture f;

    ck_fixture(&f, "alpha beta gamma\n");
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);

    f.ed.now_ms += 399;
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 2U);

    f.ed.now_ms += 401;
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    yew_ed_free(&f.ed);
}

/* A different CELL is a different click — a cell, not a pixel radius,
 * because cells are the unit of everything here. */
void test_mouse_click_counter_resets_on_a_different_cell(void)
{
    ClickFixture f;

    ck_fixture(&f, "alpha beta gamma\n");
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    ck_click(&f, 3U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    ck_click(&f, 3U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 2U);
    /* Back to the first cell: a new run, not a continuation. */
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    yew_ed_free(&f.ed);
}

/* Four clicks wrap to one: never a surprise paragraph selection. */
void test_mouse_click_quad_wraps_to_one(void)
{
    ClickFixture f;
    int i;

    ck_fixture(&f, "alpha beta gamma\n");
    for (i = 0; i < 8; i++) {
        ck_click(&f, 2U, 0U, 0U);
        YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, (u64)(i % 3 + 1));
    }
    yew_ed_free(&f.ed);
}

/* Focus-out resets it, even though no gesture is in flight by then —
 * the click that armed the counter released long ago. */
void test_mouse_click_focus_out_resets_the_counter(void)
{
    ClickFixture f;

    ck_fixture(&f, "alpha beta gamma\n");
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    YEW_ASSERT(!yew_mouse_gesture_active(&f.ed));
    yew_mouse_cancel(&f.ed);
    ck_click(&f, 2U, 0U, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 1U);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Agreement with the engines                                        */
/* ---------------------------------------------------------------- */

/*
 * DoD 6, the central row.  Sprint 16's conformance corpus, clicked
 * grapheme by grapheme; the resulting selection must equal
 * yew_unit_word.span at the same offset, exactly.
 */
void test_mouse_double_click_equals_the_word_engine(void)
{
    static const char *const corpus[] = {
        "foo.bar()",
        "a  b",
        "\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
        "\xe6\xbc\xa2\xe5\xad\x97",
        "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
        "\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94",
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
        "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6x",
        "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba\xf0\x9f\x87\xa6\xf0\x9f\x87\xba",
        "don't",
        "well-known",
        "1,000.50",
        "foo_bar baz",
        "\xff" "A",
        "  leading"
    };
    size_t i;

    YEW_ASSERT(YEW_ARRAY_LEN(corpus) >= 12U);
    for (i = 0U; i < YEW_ARRAY_LEN(corpus); i++) {
        ClickFixture f;
        u16 x;

        ck_fixture(&f, corpus[i]);
        for (x = 0U; x < 12U; x++) {
            UnitCtx uc;
            Span want;
            Span got;
            ByteOff at;

            /* Click, then click again in the same cell inside the
             * window: that is a double-click by construction. */
            f.ed.mouse.click_n = 0U;
            ck_click(&f, x, f.ed.pane_root->rect.y, 0U);
            at = yew_ed_cursor(&f.ed)->pos;
            ck_click(&f, x, f.ed.pane_root->rect.y, 0U);
            YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 2U);

            (void)memset(&uc, 0, sizeof(uc));
            uc.tb = f.ed.win->buf->tb;
            uc.buf = f.ed.win->buf;
            uc.win = f.ed.win;
            want = yew_unit_word.span(&uc, at, false);
            got = ck_selection(&f);
            YEW_ASSERT_EQ_U64(got.lo, want.lo);
            YEW_ASSERT_EQ_U64(got.hi, want.hi);
            /* And the state is H mode with the WORD engine borrowed —
             * exactly what `H` plus the unit key would produce. */
            YEW_ASSERT_EQ_U64((u64)f.ed.mode, (u64)YEW_MODE_H);
            YEW_ASSERT(f.ed.win->h.unit == &yew_unit_word);
        }
        yew_ed_free(&f.ed);
    }
}

void test_mouse_triple_click_equals_the_line_engine(void)
{
    ClickFixture f;
    UnitCtx uc;
    Span want;
    Span got;
    ByteOff at;

    ck_fixture(&f, "alpha beta\nsecond line here\nthird\n");
    ck_click(&f, 4U, (u16)(f.ed.pane_root->rect.y + 1U), 0U);
    at = yew_ed_cursor(&f.ed)->pos;
    ck_click(&f, 4U, (u16)(f.ed.pane_root->rect.y + 1U), 0U);
    ck_click(&f, 4U, (u16)(f.ed.pane_root->rect.y + 1U), 0U);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 3U);

    (void)memset(&uc, 0, sizeof(uc));
    uc.tb = f.ed.win->buf->tb;
    uc.buf = f.ed.win->buf;
    uc.win = f.ed.win;
    want = yew_unit_line.span(&uc, at, false);
    got = ck_selection(&f);
    YEW_ASSERT_EQ_U64(got.lo, want.lo);
    YEW_ASSERT_EQ_U64(got.hi, want.hi);
    YEW_ASSERT_EQ_U64((u64)f.ed.mode, (u64)YEW_MODE_H);
    YEW_ASSERT(f.ed.win->h.unit == &yew_unit_line);
    yew_ed_free(&f.ed);
}

/*
 * Alt is the SAME modifier the keyboard's A-← / A-→ pass (s16 §1), so
 * Alt+double-click selects the whitespace-delimited WORD.  Asserted
 * against the engine with alt=true, again rather than against
 * hand-written offsets.
 */
void test_mouse_alt_double_click_selects_the_whitespace_word(void)
{
    ClickFixture f;
    UnitCtx uc;
    Span plain;
    Span alt;
    Span got;
    ByteOff at;
    u16 y;

    ck_fixture(&f, "foo.bar  baz\n");
    y = f.ed.pane_root->rect.y;
    ck_click(&f, ck_col(&f, 3U), y, 0U);
    at = yew_ed_cursor(&f.ed)->pos;
    (void)memset(&uc, 0, sizeof(uc));
    uc.tb = f.ed.win->buf->tb;
    uc.buf = f.ed.win->buf;
    uc.win = f.ed.win;
    plain = yew_unit_word.span(&uc, at, false);
    alt = yew_unit_word.span(&uc, at, true);
    /* The fixture is chosen so the two answers DIFFER — otherwise the
     * test would pass whichever flag the router passed through. */
    YEW_ASSERT(plain.hi != alt.hi);

    ck_click(&f, ck_col(&f, 3U), y, (u16)YEW_MOD_ALT);
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 2U);
    got = ck_selection(&f);
    YEW_ASSERT_EQ_U64(got.lo, alt.lo);
    YEW_ASSERT_EQ_U64(got.hi, alt.hi);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Word-wise drag extension                                          */
/* ---------------------------------------------------------------- */

/*
 * Dragging after a double-click extends BY WHOLE UNITS.  The anchor
 * stays at the initial word's span and the head snaps to the boundary
 * of the word under the pointer.
 *
 * Extending by CHARACTERS is what a naive implementation does, and it
 * feels broken in a way people describe as "the selection is fighting
 * me" — so the test asserts the head is exactly a word boundary, not
 * merely that the selection grew.
 */
void test_mouse_drag_after_a_double_click_extends_by_words(void)
{
    ClickFixture f;
    UnitCtx uc;
    Span first;
    Span third;
    Span got;
    u16 y;

    ck_fixture(&f, "alpha beta gamma delta\n");
    y = f.ed.pane_root->rect.y;
    ck_click(&f, ck_col(&f, 1U), y, 0U);
    {
        Key press = ck_ev((u8)YEW_KEY_PRESS, ck_col(&f, 1U), y, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.click_n, 2U);
    (void)memset(&uc, 0, sizeof(uc));
    uc.tb = f.ed.win->buf->tb;
    uc.buf = f.ed.win->buf;
    uc.win = f.ed.win;
    first = yew_unit_word.span(&uc, BYTEOFF(1U), false);

    /* Drag into the middle of `gamma`. */
    {
        Key motion = ck_ev((u8)YEW_KEY_REPEAT, ck_col(&f, 13U), y, 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    third = yew_unit_word.span(&uc, BYTEOFF(13U), false);
    got = ck_selection(&f);
    YEW_ASSERT_EQ_U64(got.lo, first.lo);
    /* The WHOLE third word, not the six characters the pointer passed. */
    YEW_ASSERT_EQ_U64(got.hi, third.hi);

    /* Dragging back BEFORE the anchor word flips the head to the left
     * boundary, and the anchor word stays wholly selected. */
    {
        Key motion = ck_ev((u8)YEW_KEY_REPEAT, ck_col(&f, 0U), y, 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    got = ck_selection(&f);
    YEW_ASSERT_EQ_U64(got.hi, first.hi);
    YEW_ASSERT_EQ_U64(got.lo, yew_unit_word.span(&uc, BYTEOFF(0U),
                                                 false).lo);
    yew_ed_free(&f.ed);
}

/* A single-click drag stays character-wise: the unit engines are what a
 * multi-click borrows, not what every drag uses. */
void test_mouse_single_click_drag_is_character_wise(void)
{
    ClickFixture f;
    Span got;
    u16 y;

    ck_fixture(&f, "alpha beta gamma\n");
    y = f.ed.pane_root->rect.y;
    {
        Key press = ck_ev((u8)YEW_KEY_PRESS, ck_col(&f, 1U), y, 0U);
        Key motion = ck_ev((u8)YEW_KEY_REPEAT, ck_col(&f, 8U), y, 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    got = ck_selection(&f);
    YEW_ASSERT_EQ_U64(got.lo, 1U);
    /* Exactly where the pointer is, not the end of `beta`. */
    YEW_ASSERT_EQ_U64(got.hi, 8U);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Middle-click paste                                                */
/* ---------------------------------------------------------------- */

/*
 * OFF by default.  X11 primary-selection semantics cannot be
 * implemented correctly from a terminal and OSC 52 is write-only in
 * most of them, so shipping the half-thing silently would be worse than
 * the option.
 */
void test_mouse_middle_click_paste_is_off_by_default(void)
{
    ClickFixture f;
    u64 before;

    YEW_ASSERT(!yew_mouse_middle_paste());
    ck_fixture(&f, "alpha\n");
    {
        RegVal v;

        yew_regval_init(&v);
        bytebuf_append(&v.bytes, (const u8 *)"XYZ", 3U);
        v.type = (u8)YEW_REG_CHARWISE;
        yew_reg_yank(&f.ed.regs, 0U, &v);
        yew_regval_free(&v);
    }
    before = yew_textbuf_len(f.ed.win->buf->tb);
    {
        Key press = ck_ev((u8)YEW_KEY_PRESS, 2U, f.ed.pane_root->rect.y,
                          0U);

        press.button = (u8)YEW_MB_MIDDLE;
        yew_mouse_event(&f.ed, &press);
    }
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.win->buf->tb), before);

    /* Enabled, it pastes at the CLICK position as one transaction. */
    yew_mouse_set_middle_paste(true);
    {
        Key press = ck_ev((u8)YEW_KEY_PRESS, 2U, f.ed.pane_root->rect.y,
                          0U);

        press.button = (u8)YEW_MB_MIDDLE;
        yew_mouse_event(&f.ed, &press);
    }
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.win->buf->tb), before + 3U);
    /* One undo step takes the whole paste back. */
    {
        CmdCtx cx = {0};
        CmdId id = yew_cmd_lookup("ed.edit.undo", 12U);

        cx.ed = &f.ed;
        cx.win = f.ed.win;
        cx.count = 1U;
        (void)yew_ed_invoke(&f.ed, id, &cx);
    }
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.win->buf->tb), before);
    yew_mouse_set_middle_paste(false);
    yew_ed_free(&f.ed);
}
