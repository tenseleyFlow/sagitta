/*
 * Sprint 26 §8: type-to-jump.
 *
 * DoD 12 names four rows and each is a different way to get this wrong.
 * The one worth stating is the third: an EXACT match under the cursor
 * must never be moved off.  Typing `README` while already sitting on
 * `README` has to stay there — jumping to `README.md` because it also
 * matched is the behaviour fuss's rule exists to forbid, and it is
 * maddening in practice because the thing you wanted was already
 * selected.
 *
 * The clock is passed in throughout, so the 500 ms window is tested at
 * its boundaries rather than by sleeping.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <string.h>

#include "term/input.h"
#include "ui/picker.h"
#include "ui/typejump.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

static Key tj_char(char c)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)(u8)c;
    k.text[0] = (u8)c;
    k.ntext = 1U;
    return k;
}

static Key tj_special(u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.code = code;
    return k;
}

static const PickItem tj_items[] = {
    {"alpha.c", NULL, 1, 0U},
    {"beta.c", NULL, 2, 0U},
    {"README", NULL, 3, 0U},
    {"README.md", NULL, 4, 0U},
    {"gamma.c", NULL, 5, 0U},
    {"sigma.c", NULL, 6, 0U}
};

/* ---------------------------------------------------------------- */
/* Rule 1: append inside the window, replace after                  */
/* ---------------------------------------------------------------- */

void test_typejump_appends_inside_the_window(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    k = tj_char('b');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(sel, 1U); /* beta.c */

    /* `e` 100 ms later, inside the 500 ms window: the pattern is `be`,
     * which still selects beta.c. */
    k = tj_char('e');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1100, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(sel, 1U);
    YEW_ASSERT_EQ_U64(tj.len, 2U);
    YEW_ASSERT_EQ_MEM(tj.pat, "be", 2U);
}

/*
 * The same key after the window REPLACES.  `s`, pause, `s` means "the
 * next thing starting with s", not "find `ss`".
 */
void test_typejump_replaces_after_the_window(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    k = tj_char('s');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 1U);

    /* Exactly at the deadline is OUTSIDE: the window is [t, t+500). */
    k = tj_char('g');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1500, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 1U);
    YEW_ASSERT_EQ_MEM(tj.pat, "g", 1U);
    YEW_ASSERT_EQ_U64(sel, 4U); /* gamma.c */
}

/* One millisecond earlier is INSIDE, which is the other side of the
 * same boundary. */
void test_typejump_window_boundary_is_exact(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    k = tj_char('s');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    k = tj_char('i');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1499, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 2U);
    YEW_ASSERT_EQ_MEM(tj.pat, "si", 2U);
    YEW_ASSERT_EQ_U64(sel, 5U); /* sigma.c */
}

/* ---------------------------------------------------------------- */
/* Rule 3: an exact match under the cursor does not move             */
/* ---------------------------------------------------------------- */

/*
 * fuss's rule verbatim, and the reason this function checks the current
 * selection before it ranks anything.
 *
 * `README` is an exact match for the item at index 2.  `README.md` also
 * matches — as a prefix — so a naive "take the best match" would jump
 * to whichever the scorer preferred.  Sitting on the exact one, nothing
 * moves.
 */
void test_typejump_exact_match_under_the_cursor_stays_put(void)
{
    TypeJump tj;
    u32 sel = 2U; /* README */
    Key k;
    const char *pat = "README";
    u32 i;
    i64 now = 1000;

    yew_typejump_clear(&tj);
    for (i = 0U; pat[i] != '\0'; i++) {
        k = tj_char(pat[i]);
        YEW_ASSERT(yew_typejump_key(&tj, &k, now, tj_items, 6U, &sel));
        now += 50;
    }
    /* Still on README, never on README.md. */
    YEW_ASSERT_EQ_U64(sel, 2U);
}

/* And from somewhere else, the same pattern DOES move — otherwise the
 * rule above would be indistinguishable from "never move". */
void test_typejump_moves_when_the_cursor_is_elsewhere(void)
{
    TypeJump tj;
    u32 sel = 0U; /* alpha.c */
    Key k;
    const char *pat = "README";
    u32 i;
    i64 now = 1000;

    yew_typejump_clear(&tj);
    for (i = 0U; pat[i] != '\0'; i++) {
        k = tj_char(pat[i]);
        YEW_ASSERT(yew_typejump_key(&tj, &k, now, tj_items, 6U, &sel));
        now += 50;
    }
    /* It landed on the exact match rather than the prefix one. */
    YEW_ASSERT_EQ_U64(sel, 2U);
}

/* ---------------------------------------------------------------- */
/* Rule 4: non-printables clear and are not consumed                */
/* ---------------------------------------------------------------- */

void test_typejump_non_printable_clears_and_is_not_consumed(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    k = tj_char('b');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 1U);

    /* An arrow ends the sequence AND still belongs to the host list. */
    k = tj_special(YEW_KEY_DOWN);
    YEW_ASSERT(!yew_typejump_key(&tj, &k, 1050, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 0U);

    /* A letter after it starts a NEW pattern, even though only 10 ms
     * passed — the arrow ended the sequence, not the clock. */
    k = tj_char('g');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1060, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 1U);
    YEW_ASSERT_EQ_U64(sel, 4U); /* gamma.c */
}

/*
 * A modified key is a BINDING, not a letter.  Consuming `C-n` here
 * would take the host list's navigation away.
 */
void test_typejump_ignores_modified_keys(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    (void)memset(&k, 0, sizeof(k));
    k.code = (u32)'n';
    k.text[0] = (u8)'n';
    k.ntext = 1U;
    k.mods = YEW_MOD_CTRL;
    YEW_ASSERT(!yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(tj.len, 0U);
    YEW_ASSERT_EQ_U64(sel, 0U);
}

/* ---------------------------------------------------------------- */
/* Rule 2: the hint expires on its own                              */
/* ---------------------------------------------------------------- */

/*
 * The accumulated pattern stops being active at the deadline with NO
 * further keys.  A hint that only cleared on the next keystroke would
 * sit on an idle editor promising something no longer true — which is
 * why the deadline belongs on the timer heap and not in a lazy check.
 */
void test_typejump_hint_expires_without_a_key(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k;

    yew_typejump_clear(&tj);
    YEW_ASSERT(!yew_typejump_active(&tj, 1000));
    k = tj_char('b');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    YEW_ASSERT(yew_typejump_active(&tj, 1000));
    YEW_ASSERT(yew_typejump_active(&tj, 1499));
    /* At the deadline, with nothing pressed. */
    YEW_ASSERT(!yew_typejump_active(&tj, 1500));
    YEW_ASSERT(!yew_typejump_active(&tj, 9999));
}

/* ---------------------------------------------------------------- */
/* Degenerate                                                       */
/* ---------------------------------------------------------------- */

/* A pattern matching nothing leaves the selection alone rather than
 * jumping to row 0. */
void test_typejump_no_match_leaves_the_selection(void)
{
    TypeJump tj;
    u32 sel = 3U;
    Key k;

    yew_typejump_clear(&tj);
    k = tj_char('z');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1000, tj_items, 6U, &sel));
    k = tj_char('q');
    YEW_ASSERT(yew_typejump_key(&tj, &k, 1050, tj_items, 6U, &sel));
    YEW_ASSERT_EQ_U64(sel, 3U);
}

void test_typejump_degenerate_inputs(void)
{
    TypeJump tj;
    u32 sel = 0U;
    Key k = tj_char('a');

    yew_typejump_clear(&tj);
    yew_typejump_clear(NULL);
    YEW_ASSERT(!yew_typejump_key(NULL, &k, 0, tj_items, 6U, &sel));
    YEW_ASSERT(!yew_typejump_key(&tj, NULL, 0, tj_items, 6U, &sel));
    YEW_ASSERT(!yew_typejump_key(&tj, &k, 0, NULL, 0U, &sel));
    YEW_ASSERT(!yew_typejump_key(&tj, &k, 0, tj_items, 0U, &sel));
    YEW_ASSERT(!yew_typejump_key(&tj, &k, 0, tj_items, 6U, NULL));
    YEW_ASSERT(!yew_typejump_active(NULL, 0));
}

/* A pattern longer than the buffer is truncated, not overflowed. */
void test_typejump_long_pattern_is_bounded(void)
{
    TypeJump tj;
    u32 sel = 0U;
    u32 i;

    yew_typejump_clear(&tj);
    for (i = 0U; i < 200U; i++) {
        Key k = tj_char('a');

        YEW_ASSERT(yew_typejump_key(&tj, &k, (i64)(1000 + i), tj_items, 6U,
                                    &sel));
    }
    YEW_ASSERT(tj.len < (u32)YEW_TYPEJUMP_PAT_MAX);
}
