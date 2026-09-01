/* Sprint 57.6: bare filename characters search; actions use C-g chords. */
#include "harness.h"

#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "mod/git/fussmode.h"
#include "mod/git/fusstree.h"

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

static Key fj_ctrl(char c)
{
    Key key = fj_char(c);

    key.mods = YEW_MOD_CTRL;
    key.ntext = 0U;
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
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
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
    YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000, fj_items,
                                 YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_tick(&jump, 1499));
    YEW_ASSERT(yew_fuss_jump_tick(&jump, 1500));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));
    YEW_ASSERT_EQ_STR(yew_fuss_jump_pattern(&jump, &len), "");
    YEW_ASSERT_EQ_U64(len, 0U);
    YEW_ASSERT(!yew_fuss_jump_tick(&jump, 9999));
}

void test_fussjump_escape_and_enter_disarm_and_are_released(void)
{
    FussJump jump;
    Key key;
    u32 sel = 0U;

    yew_fuss_jump_init(&jump);
    yew_fuss_jump_arm(&jump, 1000);
    key = fj_special(YEW_KEY_ESCAPE);
    YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 1001, fj_items,
                                  YEW_ARRAY_LEN(fj_items), &sel));
    YEW_ASSERT(!yew_fuss_jump_armed(&jump));

    yew_fuss_jump_arm(&jump, 2000);
    key = fj_special(YEW_KEY_ENTER);
    YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 2001, fj_items,
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

    yew_fuss_jump_arm(&jump, 1500);
    key = fj_char(' ');
    YEW_ASSERT(!yew_fuss_jump_key(&jump, &key, 1501, fj_items,
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
    for (i = 0U; pat[i] != '\0'; i++) {
        Key key = fj_char(pat[i]);
        YEW_ASSERT(yew_fuss_jump_key(&jump, &key,
                                     1000 + (i64)i * 20, fj_items,
                                     YEW_ARRAY_LEN(fj_items), &sel));
    }
    YEW_ASSERT_EQ_U64(sel, 1U);
}

void test_fussjump_bare_filename_characters_start_search(void)
{
    static const char chars[] = "aZ0._-/qjk";
    FussJump jump;
    u32 sel = 0U;
    size_t i;

    for (i = 0U; chars[i] != '\0'; i++) {
        Key key = fj_char(chars[i]);
        u32 len = 0U;

        yew_fuss_jump_init(&jump);
        sel = 0U;
        YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000 + (i64)i,
                                     fj_items, YEW_ARRAY_LEN(fj_items),
                                     &sel));
        YEW_ASSERT(yew_fuss_jump_armed(&jump));
        (void)yew_fuss_jump_pattern(&jump, &len);
        YEW_ASSERT_EQ_U64(len, 1U);
    }
}

void test_fussjump_candidates_are_exactly_visible_tree_rows(void)
{
    static const char pattern[] = "hidden";
    GitEntry entries[2] = {0};
    GitSnapshot snap = {0};
    FussOpts opts = {false, false};
    FussTree tree;
    PickItem items[3] = {0};
    FussJump jump;
    u32 sel = 0U;
    size_t i;

    entries[0].path = (char *)"src/hidden-target.lu";
    entries[0].path_len = (u32)strlen(entries[0].path);
    entries[0].unstaged = true;
    entries[1].path = (char *)"visible.lu";
    entries[1].path_len = (u32)strlen(entries[1].path);
    entries[1].unstaged = true;
    snap.entries.data = entries;
    snap.entries.len = YEW_ARRAY_LEN(entries);
    snap.gen = 1U;

    yew_fuss_tree_init(&tree);
    yew_fuss_build(&tree, &snap, &opts);
    YEW_ASSERT_EQ_U64(tree.items.len, 2U);
    YEW_ASSERT_EQ_STR(tree.items.data[0].path, "src");
    for (i = 0U; i < tree.items.len; i++)
        items[i].label = tree.items.data[i].path;
    yew_fuss_jump_init(&jump);
    for (i = 0U; pattern[i] != '\0'; i++) {
        Key key = fj_char(pattern[i]);

        YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 1000 + (i64)i,
                                     items, (u32)tree.items.len, &sel));
    }
    YEW_ASSERT_EQ_U64(sel, 0U);
    YEW_ASSERT(yew_fuss_jump_tick(&jump,
                                  1000 + (i64)i - 1 +
                                      YEW_TYPEJUMP_RESET_MS));

    YEW_ASSERT(yew_fuss_nav_toggle(&tree, 0));
    YEW_ASSERT_EQ_U64(tree.items.len, 3U);
    (void)memset(items, 0, sizeof(items));
    for (i = 0U; i < tree.items.len; i++)
        items[i].label = tree.items.data[i].path;
    sel = 0U;
    for (i = 0U; pattern[i] != '\0'; i++) {
        Key key = fj_char(pattern[i]);

        YEW_ASSERT(yew_fuss_jump_key(&jump, &key, 2000 + (i64)i,
                                     items, (u32)tree.items.len, &sel));
    }
    YEW_ASSERT_EQ_U64(sel, 1U);
    YEW_ASSERT_EQ_STR(tree.items.data[sel].path, "src/hidden-target.lu");
    yew_fuss_tree_drop(&tree);
}

void test_fussjump_f_mode_dispatch_table_is_complete(void)
{
    static const struct {
        const char *seq;
        const char *cmd;
    } rows[] = {
        {"<up>", "ed.git.nav.prev"}, {"<down>", "ed.git.nav.next"},
        {"<left>", "ed.git.nav.parent"}, {"<right>", "ed.git.nav.enter"},
        {"<space>", "ed.git.view"},
        {"C-w s", "ed.git.open_split_h"},
        {"C-w v", "ed.git.open_split_v"},
        {"C-<up>", "ed.git.nav.row_prev"},
        {"C-<down>", "ed.git.nav.row_next"},
        {"C-g a", "ed.git.stage"}, {"C-g u", "ed.git.unstage"},
        {"C-g S", "ed.git.stage.all"},
        {"C-g U", "ed.git.unstage.all"},
        {"C-g m", "ed.git.commit"},
        {"C-g M", "ed.git.commit.amend"},
        {"C-g p", "ed.git.push"}, {"C-g l", "ed.git.pull"},
        {"C-g f", "ed.git.fetch"}, {"C-g d", "ed.git.diff"},
        {"C-g D", "ed.git.diff.view"},
        {"C-g s", "ed.git.status"}, {"C-g w", "ed.git.blame"},
        {"C-g h", "ed.git.history"}, {"C-g L", "ed.git.reflog"},
        {"C-g c", "ed.git.view"},
        {"C-g b", "ed.git.branch.switch"},
        {"C-g n", "ed.git.branch.create"},
        {"C-g R", "ed.git.branch.delete"},
        {"C-g G", "ed.git.merge"}, {"C-g O", "ed.git.reset"},
        {"C-g I", "ed.git.rebase.interactive"},
        {"C-g y", "ed.git.cherry_pick"},
        {"C-g v", "ed.git.revert"},
        {"C-g z", "ed.git.stash.push"},
        {"C-g Z", "ed.git.stash.pop"},
        {"C-g t", "ed.git.tag"}, {"C-g x", "ed.git.discard"},
        {"C-g r", "ed.git.file.delete"},
        {"C-g N", "ed.git.file.rename"},
        {"<cr>", "ed.git.open"}, {"C-g g", "ed.group.from_dir"},
        {"C-g T", "ed.git.tree.all"},
        {"C-g .", "ed.git.tree.hidden"},
        {"C-r", "ed.git.refresh"}, {"C-g q", "ed.git.mode.leave"},
        {"<esc>", "ed.git.mode.leave"}
    };
    Ed ed = {0};
    u32 i;

    yew_dispatch_init(&ed);
    yew_dispatch_set_mode(&ed, YEW_MODE_F);
    YEW_ASSERT_EQ_U64(ed.keys.l[0], &ed.mode_keys[YEW_MODE_F]);
    YEW_ASSERT_EQ_U64(ed.chord.n, 0U);
    YEW_ASSERT_EQ_I64(ed.chord.layer, -1);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        KeyId keys[2];
        size_t key_count;
        const Binding *binding = NULL;
        const CmdDesc *desc;

        key_count = yew_key_parse_seq(rows[i].seq, keys,
                                      YEW_ARRAY_LEN(keys));
        YEW_ASSERT(key_count == 1U || key_count == 2U);
        YEW_ASSERT_EQ_I64(yew_keymap_lookup(ed.keys.l[0], keys, key_count,
                                            NULL, &binding),
                          YEW_MATCH_FULL);
        YEW_ASSERT_NOT_NULL(binding);
        desc = yew_cmd_desc(binding->cmd);
        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_EQ_STR(desc->name, rows[i].cmd);
        YEW_ASSERT((desc->flags & YEW_CMD_RECORDABLE) != 0U);
        YEW_ASSERT_NOT_NULL(desc->word);
        YEW_ASSERT(desc->word[0] != '\0');
        YEW_ASSERT(desc->help[0] != '\0');
        YEW_ASSERT_EQ_U64(ed.chord.n, 0U);
        YEW_ASSERT_EQ_I64(ed.chord.layer, -1);
        if (strncmp(rows[i].seq, "C-g ", 4U) == 0) {
            Key prefix = fj_ctrl('g');
            Key tail = fj_char(rows[i].seq[4]);

            ed.last_cmd.v = 0U;
            yew_dispatch_key(&ed, prefix, 1000 + (i64)i * 2);
            YEW_ASSERT_EQ_U64(ed.chord.n, 1U);
            yew_dispatch_key(&ed, tail, 1001 + (i64)i * 2);
            YEW_ASSERT_EQ_U64(ed.last_cmd.v, binding->cmd.v);
            YEW_ASSERT_EQ_U64(ed.chord.n, 0U);
        }
    }
    {
        static const char bare[] =
            "auSUmMplfdswhLcbnRGOIyvzZtxrNT.gq/?jk";

        for (i = 0U; bare[i] != '\0'; i++) {
            char seq[2] = {bare[i], '\0'};
            KeyId key;
            const Binding *binding = NULL;

            YEW_ASSERT_EQ_U64(yew_key_parse_seq(seq, &key, 1U), 1U);
            YEW_ASSERT_EQ_I64(yew_keymap_lookup(ed.keys.l[0], &key, 1U,
                                                NULL, &binding),
                              YEW_MATCH_NONE);
            YEW_ASSERT_NULL(binding);
        }
    }
    {
        KeyId keys[2];
        const Binding *binding = NULL;
        const CmdDesc *desc;

        YEW_ASSERT_EQ_U64(yew_key_parse_seq("C-g ?", keys,
                                            YEW_ARRAY_LEN(keys)), 2U);
        YEW_ASSERT_EQ_I64(yew_keymap_lookup(ed.keys.l[0], keys, 2U,
                                            NULL, &binding),
                          YEW_MATCH_FULL);
        YEW_ASSERT_NOT_NULL(binding);
        desc = yew_cmd_desc(binding->cmd);
        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_EQ_STR(desc->name, "ed.ui.message_expand");
    }
    yew_dispatch_free(&ed);
}

void test_fussjump_bare_former_verbs_start_search(void)
{
    static const char verbs[] =
        "auSUmMplfdswhLcbnRGOIyvzZtxrNT.gq";
    FussJump jump;
    size_t i;

    for (i = 0U; verbs[i] != '\0'; i++) {
        Key key = fj_char(verbs[i]);
        u32 sel = 0U;

        yew_fuss_jump_init(&jump);
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
