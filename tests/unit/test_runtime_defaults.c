#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keymap.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "fl/record.h"
#include "util/buf.h"

/* Frozen immediately before the Sprint 36 migration from keys_default.c. */
static const BindRow frozen_L[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"A-<left>", "ed.move.unit.home_alt", 0, NULL},
    {"A-<right>", "ed.move.unit.end_alt", 0, NULL},
    {"A-<up>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-<down>", "ed.move.unit.next_alt", 0, NULL},
    {"<home>", "ed.move.unit.home", 0, NULL},
    {"<end>", "ed.move.unit.end", 0, NULL},
    {"<pgup>", "ed.view.page_up", 0, NULL},
    {"<pgdn>", "ed.view.page_down", 0, NULL},
    {"i", "ed.mode.enter", 0, "I"},
    {"a", "ed.edit.insert.after", 0, NULL},
    {"o", "ed.edit.line.open_below", 0, NULL},
    {"O", "ed.edit.line.open_above", 0, NULL},
    {"x", "ed.edit.delete.grapheme", 0, NULL},
    {"d d", "ed.edit.line.delete", 0, NULL},
    {"u", "ed.edit.undo", 0, NULL},
    {"C-r", "ed.edit.redo", 0, NULL},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"g g", "ed.move.buf.home", 0, NULL},
    /* Sprint 21 §5.  Ctrl-O/Ctrl-I are the jumplist's two directions;
     * g; / g, walk the changelist. */
    /* Sprint 21 §1: the search surface. */
    /* Sprint 22 §4: panes.  `s`-prefixed chords, keymap DATA — no key
     * calls a pane function directly (DoD 8). */
    {"C-w s", "ed.pane.split_h", 0, NULL},
    {"C-w v", "ed.pane.split_v", 0, NULL},
    {"C-w c", "ed.pane.close", 0, NULL},
    {"C-w <left>", "ed.pane.focus_left", 0, NULL},
    {"C-w <right>", "ed.pane.focus_right", 0, NULL},
    {"C-w <up>", "ed.pane.focus_up", 0, NULL},
    {"C-w <down>", "ed.pane.focus_down", 0, NULL},
    {"C-w w", "ed.pane.focus_next", 0, NULL},
    {"C-w =", "ed.pane.grow", 0, NULL},
    {"C-w -", "ed.pane.shrink", 0, NULL},
    /* Sprint 23 §5: tabs.  t-prefix family, keymap DATA. */
    {"t n", "ed.tab.next", 0, NULL},
    {"t p", "ed.tab.prev", 0, NULL},
    {"t t", "ed.tab.new", 0, NULL},
    {"t c", "ed.tab.close", 0, NULL},
    /*
     * Sprint 24 §6: the continuous line, and the group in/out pair.
     *
     * Kitty-protocol chords are the ENHANCEMENT, never the only path.
     * `super+ctrl+up` is exactly the chord a window manager is most
     * likely to eat, and a user who cannot leave a group is stuck — so
     * the t-prefix forms exist for every one of these, and walking
     * right out of a group works regardless (see groupnav.c).
     */
    {"C-<pgdn>", "ed.file.next", 0, NULL},
    {"C-<pgup>", "ed.file.prev", 0, NULL},
    {"t <right>", "ed.file.next", 0, NULL},
    {"t <left>", "ed.file.prev", 0, NULL},
    {"t <down>", "ed.group.enter", 0, NULL},
    {"t <up>", "ed.group.leave", 0, NULL},
    {"t g", "ed.group.new", 0, NULL},
    {"t e", "ed.group.edit", 0, NULL},
    {"t d", "ed.group.dissolve", 0, NULL},
    {"t r", "ed.group.remove_tab", 0, NULL},
    /*
     * Sprint 27 §5/§8.  Invariant 9: every context-menu row is reachable
     * without a pointer, and the menu itself is one keystroke away.
     */
    {"t m", "ed.ui.context_menu", 0, NULL},
    {"t l", "ed.group.rename", 0, NULL},
    {"t o", "ed.tab.close_others", 0, NULL},
    {"t y", "ed.tab.copy_path", 0, NULL},
    /*
     * Sprint 24 §7: alt+1..9,0 jump straight to a tab and arm the
     * 500 ms window, so `alt+1` `5` reaches tab 15.  `0` is the TENTH
     * key on the digit row, not the zeroth tab.
     */
    {"A-1", "ed.tab.goto", 1, NULL},
    {"A-2", "ed.tab.goto", 2, NULL},
    {"A-3", "ed.tab.goto", 3, NULL},
    {"A-4", "ed.tab.goto", 4, NULL},
    {"A-5", "ed.tab.goto", 5, NULL},
    {"A-6", "ed.tab.goto", 6, NULL},
    {"A-7", "ed.tab.goto", 7, NULL},
    {"A-8", "ed.tab.goto", 8, NULL},
    {"A-9", "ed.tab.goto", 9, NULL},
    {"A-0", "ed.tab.goto", 0, NULL},
    {"m", "ed.mark.set", 0, NULL},
    {"'", "ed.mark.jump", 0, NULL},
    {"/", "ed.search.open", 0, NULL},
    {"?", "ed.search.open_back", 0, NULL},
    {"n", "ed.search.next", 0, NULL},
    {"N", "ed.search.prev", 0, NULL},
    {"*", "ed.search.word_next", 0, NULL},
    {"#", "ed.search.word_prev", 0, NULL},
    {"C-o", "ed.jump.back", 0, NULL},
    {"C-i", "ed.jump.fwd", 0, NULL},
    {"g ;", "ed.change.older", 0, NULL},
    {"g ,", "ed.change.newer", 0, NULL},
    {"G", "ed.move.buf.end", 0, NULL},
    {"s", "ed.file.save", 0, NULL},
    {"q", "ed.macro.record", 0, NULL},
    {"@", "ed.macro.replay", 0, NULL},
    {"@ @", "ed.macro.replay_last", 0, NULL},
    {"Q", "ed.macro.list", 0, NULL},
    {"C-z", "ed.suspend", 0, NULL},
    {"C-l", "ed.redraw", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"0", "ed.move.unit.home", 0, NULL},
    {"w", "ed.mode.enter", 0, "W"},
    {"b", "ed.mode.enter", 0, "B"},
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"e", "ed.mode.enter", 0, "E"},
    {"f", "ed.mode.enter", 0, "F"},
};

static const BindRow frozen_W[] = {
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"A-<left>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-<right>", "ed.move.unit.next_alt", 0, NULL},
    {"C-<left>", "ed.move.word.sub_prev", 0, NULL},
    {"C-<right>", "ed.move.word.sub_next", 0, NULL},
    {"<home>", "ed.move.unit.home", 0, NULL},
    {"<end>", "ed.move.unit.end", 0, NULL},
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"q", "ed.macro.record", 0, NULL},
    {"@", "ed.macro.replay", 0, NULL},
    {"@ @", "ed.macro.replay_last", 0, NULL},
    {"Q", "ed.macro.list", 0, NULL},
};

static const BindRow frozen_B[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"A-<left>", "ed.move.block.match_prev", 0, NULL},
    {"A-<right>", "ed.move.block.match_next", 0, NULL},
    {"A-<up>", "ed.sel.unit.expand", 0, NULL},
    {"A-<down>", "ed.sel.unit.contract", 0, NULL},
    {"h", "ed.mode.enter", 0, "H"},
    {":", "ed.mode.enter", 0, "E"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"q", "ed.macro.record", 0, NULL},
    {"@", "ed.macro.replay", 0, NULL},
    {"@ @", "ed.macro.replay_last", 0, NULL},
    {"Q", "ed.macro.list", 0, NULL},
};

static const BindRow frozen_I[] = {
    {"A-h", "ed.mode.enter", 0, "H"},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"<cr>", "ed.edit.insert.newline", 0, NULL},
    {"<tab>", "ed.edit.insert.tab", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
};

static const BindRow frozen_E[] = {
    {"<left>", "ed.move.char.prev", 0, NULL},
    /*
     * Sprint 18.5 §7.  Right accepts the inline suggestion when one is
     * showing and otherwise moves one grapheme, so the key never stops
     * being a motion.  C-e deliberately stays on end-of-line: its
     * fallback would have to differ, and one command cannot carry two.
     */
    {"<right>", "ed.cmdline.ghost.accept", 0, NULL},
    {"<home>", "ed.move.line.home", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
    {"C-a", "ed.move.line.home", 0, NULL},
    {"C-e", "ed.move.line.end", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"C-w", "ed.del.word_prev", 0, NULL},
    {"C-u", "ed.del.to_home", 0, NULL},
    {"C-k", "ed.del.to_end", 0, NULL},
    {"<up>", "ed.cmdline.hist_prev", 0, NULL},
    {"<down>", "ed.cmdline.hist_next", 0, NULL},
    {"<tab>", "ed.cmdline.complete_next", 0, NULL},
    {"S-<tab>", "ed.cmdline.complete_prev", 0, NULL},
    /*
     * Sprint 18.5 §6.  The menu moves on Tab/S-Tab and C-n/C-p, and NOT
     * on the arrow keys: a live menu is open the whole time a command
     * name is being typed, so giving Up to the menu would make history
     * unreachable exactly when it is most wanted -- you reach for it
     * blind, at the start of a line, which is when the list is fullest.
     * Keymap data, so flipping this is two lines plus goldens.
     */
    {"C-n", "ed.cmdline.complete_next", 0, NULL},
    {"C-p", "ed.cmdline.complete_prev", 0, NULL},
    {"<pgdn>", "ed.cmdline.menu.page_next", 0, NULL},
    {"<pgup>", "ed.cmdline.menu.page_prev", 0, NULL},
    {"C-r", "ed.cmdline.insert_register", 0, NULL},
    {"C-v", "ed.cmdline.literal_next", 0, NULL},
    {"C-z", "ed.edit.undo", 0, NULL},
    {"C-y", "ed.edit.redo", 0, NULL},
    {"<cr>", "ed.cmdline.accept", 0, NULL},
    {"<esc>", "ed.cmdline.cancel", 0, NULL},
    {"C-g", "ed.cmdline.cancel", 0, NULL},
};

static const BindRow frozen_F[] = {
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
};


static void read_runtime(Bytebuf *out)
{
    FILE *fp = fopen("runtime/init.fl", "rb");
    u8 chunk[4096];
    size_t n;

    YEW_ASSERT_NOT_NULL(fp);
    bytebuf_init(out);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) != 0U)
        bytebuf_append(out, chunk, n);
    YEW_ASSERT(!ferror(fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    bytebuf_push_u8(out, 0U);
}

static const Binding *effective_binding(const Ed *ed, Mode mode,
                                        const char *seq)
{
    KeyId keys[YEW_CHORD_MAX];
    const Binding *binding = NULL;
    KeyMatch match;
    u32 n = yew_key_parse_seq(seq, keys, YEW_CHORD_MAX);

    YEW_ASSERT(n != 0U);
    match = yew_keymap_lookup(&ed->bind_keys[mode], keys, n, NULL,
                              &binding);
    if (match != YEW_MATCH_FULL && match != YEW_MATCH_FULL_PREFIX) {
        match = yew_keymap_lookup(&ed->mode_keys[mode], keys, n, NULL,
                                  &binding);
    }
    YEW_ASSERT(match == YEW_MATCH_FULL || match == YEW_MATCH_FULL_PREFIX);
    YEW_ASSERT_NOT_NULL(binding);
    return binding;
}

static void assert_frozen_mode(const Ed *ed, Mode mode,
                               const BindRow *rows, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        const Binding *actual = effective_binding(ed, mode, rows[i].seq);
        const CmdDesc *desc = yew_cmd_desc(actual->cmd);

        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_EQ_STR(desc->name, rows[i].cmd);
        YEW_ASSERT_EQ_I64(actual->iarg, rows[i].iarg);
        YEW_ASSERT_EQ_STR(actual->sarg, rows[i].sarg);
    }
}

static void assert_default_value(const OptVal *actual, const OptVal *want)
{
    YEW_ASSERT_EQ_U64(actual->type, want->type);
    if (want->type == (u8)YEW_OPT_BOOL) {
        YEW_ASSERT_EQ_U64(actual->as.b, want->as.b);
    } else if (want->type == (u8)YEW_OPT_INT) {
        YEW_ASSERT_EQ_I64(actual->as.i, want->as.i);
    } else {
        YEW_ASSERT_EQ_U64(actual->as.str.len, want->as.str.len);
        YEW_ASSERT_EQ_MEM(actual->as.str.s, want->as.str.s,
                          want->as.str.len);
    }
}

void test_runtime_defaults_rebuild_frozen_keymap(void)
{
    static const BindRow *const rows[YEW_MODE__N] = {
        frozen_L, frozen_W, frozen_B, NULL, frozen_I, frozen_E, frozen_F,
    };
    static const u32 counts[YEW_MODE__N] = {
        YEW_ARRAY_LEN(frozen_L), YEW_ARRAY_LEN(frozen_W),
        YEW_ARRAY_LEN(frozen_B), 0U, YEW_ARRAY_LEN(frozen_I),
        YEW_ARRAY_LEN(frozen_E), YEW_ARRAY_LEN(frozen_F),
    };
    Bytebuf source;
    Ed ed;
    u32 mode;
    u32 panic_rows = 0U;
    u32 rebuilds;

    read_runtime(&source);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    rebuilds = yew_bind_rebuild_count(&ed);
    yew_bind_batch_begin(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, (const char *)source.data,
                                  (u32)(source.len - 1U)), YEW_CMD_OK);
    yew_bind_batch_end(&ed);
    YEW_ASSERT_EQ_U64(yew_bind_rebuild_count(&ed), rebuilds + 1U);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 160U);
    for (mode = 0U; mode < (u32)YEW_MODE__N; mode++) {
        if (mode != (u32)YEW_MODE_H)
            panic_rows += yew_keymap_binding_count(&ed.mode_keys[mode]);
        assert_frozen_mode(&ed, (Mode)mode, rows[mode], counts[mode]);
    }
    YEW_ASSERT_EQ_U64(panic_rows, 6U);
    yew_ed_free(&ed);
    bytebuf_free(&source);
}

void test_runtime_defaults_parse_run_style_and_options(void)
{
    Bytebuf source;
    Ed ed;
    u32 i;

    read_runtime(&source);
    YEW_ASSERT(strncmp((const char *)source.data,
                       "# yew default configuration.", 28U) == 0);
    YEW_ASSERT(strstr((const char *)source.data, "# Budget:") != NULL);
    YEW_ASSERT(strstr((const char *)source.data, "\nimport ") == NULL);
    YEW_ASSERT(strstr((const char *)source.data, "# -- 8. functions") !=
               NULL);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_bind_batch_begin(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, (const char *)source.data,
                                  (u32)(source.len - 1U)), YEW_CMD_OK);
    yew_bind_batch_end(&ed);
    for (i = 0U; i < yew_opts_len; i++) {
        OptVal actual;

        YEW_ASSERT(yew_opt_get(&ed, ed.win->buf, ed.win, yew_opts[i].name,
                               (u32)strlen(yew_opts[i].name), &actual));
        assert_default_value(&actual, &yew_opts[i].dflt);
    }
    yew_ed_free(&ed);
    bytebuf_free(&source);
}
