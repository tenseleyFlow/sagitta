/* Sprint 52: `/` arms letters as search; disarmed letters remain verbs. */
#include "harness.h"

#include <string.h>

#include "mod/git/fussmode.h"

static const PickItem fj_items[] = {
    {"alpha.c", NULL, 1, 0U},
    {"README", NULL, 2, 0U},
    {"README.md", NULL, 3, 0U},
    {"src", NULL, 4, 0U},
    {"src/main.c", NULL, 5, 0U},
    {"sigma.c", NULL, 6, 0U}
};

static Key fj_char(char c)
{
    Key key;

    (void)memset(&key, 0, sizeof(key));
    key.code = (u32)(u8)c;
    key.text[0] = (u8)c;
    key.ntext = 1U;
    return key;
}

static Key fj_special(u32 code)
{
    Key key;

    (void)memset(&key, 0, sizeof(key));
    key.code = code;
    return key;
}

void test_fussjump_arm_starts_an_empty_five_hundred_ms_window(void)
{
    FussJump jump;
    u32 len = 99U;

    yew_fuss_jump_init(&jump);
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "");
    YEW_ASSERT_EQ_U64(len, 0U);
    yew_fuss_jump_arm(&jump, 1000);
    YEW_ASSERT(yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "");
    YEW_ASSERT_EQ_U64(len, 0U);
    YEW_ASSERT(!yew_fuss_jump_tick(&jump, 1499));
    YEW_ASSERT(yew_fuss_jump_armed(&jump));
}

void test_fussjump_letters_append_inside_the_window(void)
{
    FussJump jump;
    Key key;
    u32 sel = 0U;
    u32 len;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_char('s');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT_EQ_U64(sel, 3U);
    key = fj_char('r');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1100, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "sr");
    YEW_ASSERT_EQ_U64(len, 2U);
    YEW_ASSERT_EQ_U64(sel, 3U);
    YEW_ASSERT(yew_fuss_jump_armed(&jump));
}

void test_fussjump_letter_after_the_window_replaces_the_pattern(void)
{
    FussJump jump;
    Key key;
    u32 sel = 0U;
    u32 len;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_char('a');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    key = fj_char('s');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1500, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "s");
    YEW_ASSERT_EQ_U64(len, 1U);
    YEW_ASSERT(yew_fuss_jump_armed(&jump));
}

void test_fussjump_tick_self_clears_without_another_key(void)
{
    FussJump jump;
    Key key = fj_char('s');
    u32 sel = 0U;
    u32 len;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_tick(&jump, 1499));
    YEW_ASSERT(yew_fuss_jump_tick(&jump, 1500));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "");
    YEW_ASSERT_EQ_U64(len, 0U);
    YEW_ASSERT(!yew_fuss_jump_tick(&jump, 9999));
}

void test_fussjump_escape_and_enter_disarm_and_are_consumed(void)
{
    FussJump jump;
    Key key;
    u32 sel = 0U;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_special(YEW_KEY_ESCAPE);
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1001, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));

    yew_fuss_jump_arm(&jump, 2000);
    key = fj_special(YEW_KEY_ENTER);
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 2001, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
}

void test_fussjump_nonprintable_disarms_then_returns_to_dispatch(void)
{
    FussJump jump;
    Key key;
    u32 sel = 2U;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_special(YEW_KEY_DOWN);
    YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 1001, fj_items,
                                  YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_U64(sel, 2U);

    yew_fuss_jump_arm(&jump, 2000);
    key = fj_char('n');
    key.mods = YEW_MOD_CTRL;
    YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 2001, fj_items,
                                  YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_U64(sel, 2U);
}

void test_fussjump_backspace_shortens_and_rejumps(void)
{
    FussJump jump;
    Key key;
    u32 sel = 0U;
    u32 len;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_char('s');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    key = fj_char('i');
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1050, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT_EQ_U64(sel, 5U);
    key = fj_special(YEW_KEY_BACKSPACE);
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1100, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "s");
    YEW_ASSERT_EQ_U64(len, 1U);
    YEW_ASSERT_EQ_U64(sel, 3U);
}

void test_fussjump_exact_match_under_cursor_does_not_move(void)
{
    FussJump jump;
    const char *pat = "README";
    u32 sel = 1U;
    size_t i;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    for (i = 0U; pat[i] != '\0'; i++) {
        Key key = fj_char(pat[i]);
        YEW_ASSERT(yew_fuss_jump_key(&jump, &key,
                                     1000 + (i64)i * 20, fj_items,
                                     YEW_ARRAY_LEN(fj_items), &sel));
    }
    YEW_ASSERT_EQ_U64(sel, 1U);
}

void test_fussjump_disarmed_letters_are_released_as_verbs(void)
{
    static const char verbs[] =
        "auSUmMplfdswhLcbnRGOIyvzZtxrNT.g/q";
    FussJump jump;
    u32 sel = 0U;
    size_t i;

    yew_fuss_jump_init(&jump);
    for (i = 0U; verbs[i] != '\0'; i++) {
        Key key = fj_char(verbs[i]);
        YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 1000 + (i64)i,
                                      fj_items, YEW_ARRAY_LEN(fj_items),
                                      &sel));
        YEW_ASSERT(!yew_fuss_jump_armed(&jump));
        YEW_ASSERT_EQ_U64(sel, 0U);
    }
}

void test_fussjump_armed_letters_are_swallowed_before_verbs(void)
{
    static const char verbs[] =
        "auSUmMplfdswhLcbnRGOIyvzZtxrNT.g/q";
    FussJump jump;
    size_t i;

    yew_fuss_jump_init(&jump);
    for (i = 0U; verbs[i] != '\0'; i++) {
        Key key = fj_char(verbs[i]);
        u32 sel = 0U;
        yew_fuss_jump_arm(&jump, 1000 + (i64)i);
        YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000 + (i64)i,
                                     fj_items, YEW_ARRAY_LEN(fj_items),
                                     &sel));
        YEW_ASSERT(yew_fuss_jump_armed(&jump));
    }
}

void test_fussjump_degenerate_inputs_do_not_mutate_selection(void)
{
    FussJump jump;
    Key key = fj_char('a');
    u32 sel = 4U;
    u32 len = 99U;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_init(NULL);
    yew_fuss_jump_arm(NULL, 0);
    YEW_ASSERT(!yew_fuss_jump_key(NULL, &key, 0, fj_items,
                                  YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_key(&jump, NULL, 0, fj_items,
                                  YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT_EQ_U64(sel, 4U);
    YEW_ASSERT(!yew_fuss_jump_tick(NULL, 0));
    YEW_ASSERT(!yew_fuss_jump_armed(NULL));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(NULL, &len), "");
    YEW_ASSERT_EQ_U64(len, 0U);
}
