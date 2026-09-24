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

/* Frozen default keymap, extended by Sprint 53's hunk motions. */
static const BindRow frozen_L[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"S-<left>", "ed.sel.extend.left", 0, NULL},
    {"S-<right>", "ed.sel.extend.right", 0, NULL},
    {"S-<up>", "ed.sel.extend.up", 0, NULL},
    {"S-<down>", "ed.sel.extend.down", 0, NULL},
    {"C-v", "ed.clip.paste", 0, NULL},
    {"A-<left>", "ed.move.unit.home_alt", 0, NULL},
    {"A-<right>", "ed.shadow.accept_word", 0, NULL},
    {"A-S-<right>", "ed.shadow.accept_word_alt", 0, NULL},
    {"A-<up>", "ed.git.hunk.prev", 0, NULL},
    {"A-<down>", "ed.git.hunk.next", 0, NULL},
    {"A-S-<up>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-S-<down>", "ed.shadow.accept_line", 0, NULL},
    {"A-<cr>", "ed.shadow.accept_all", 0, NULL},
    {"A-]", "ed.shadow.next", 0, NULL},
    {"A-[", "ed.shadow.prev", 0, NULL},
    /* `<home>` is the indent/column-0 toggle in every mode that binds
     * it.  `A-<left>` keeps ed.move.unit.home_alt, and L's bare `0`
     * keeps ed.move.unit.home. */
    {"<home>", "ed.move.line.home_toggle", 0, NULL},
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
    /* Sprint 57.13 §4: `Select All` needs a key; `g a` was free. */
    {"g a", "ed.sel.all", 0, NULL},
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
    /*
     * Sprint 57.10: ctrl+1..9,0 address row 1 from anywhere — the way
     * between groups — while alt+N counts the row the active tab is on.
     */
    {"C-1", "ed.tab.goto_bar", 1, NULL},
    {"C-2", "ed.tab.goto_bar", 2, NULL},
    {"C-3", "ed.tab.goto_bar", 3, NULL},
    {"C-4", "ed.tab.goto_bar", 4, NULL},
    {"C-5", "ed.tab.goto_bar", 5, NULL},
    {"C-6", "ed.tab.goto_bar", 6, NULL},
    {"C-7", "ed.tab.goto_bar", 7, NULL},
    {"C-8", "ed.tab.goto_bar", 8, NULL},
    {"C-9", "ed.tab.goto_bar", 9, NULL},
    {"C-0", "ed.tab.goto_bar", 0, NULL},
    {"m", "ed.mark.set", 0, NULL},
    {"'", "ed.mark.jump", 0, NULL},
    {"/", "ed.search.open", 0, NULL},
    {"?", "ed.search.open_back", 0, NULL},
    {"n", "ed.search.next", 0, NULL},
    {"N", "ed.search.prev", 0, NULL},
    {"*", "ed.search.word_next", 0, NULL},
    {"#", "ed.search.word_prev", 0, NULL},
    {"K", "ed.lsp.hover", 0, NULL},
    {"g d", "ed.lsp.goto_def", 0, NULL},
    {"g D", "ed.lsp.goto_decl", 0, NULL},
    {"g y", "ed.lsp.goto_type", 0, NULL},
    {"g i", "ed.lsp.goto_impl", 0, NULL},
    {"g r", "ed.lsp.references", 0, NULL},
    {"g R", "ed.lsp.rename", 0, NULL},
    {"g s", "ed.lsp.symbols", 0, NULL},
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
    {"!", "ed.shell.open", 0, NULL},
    {"e", "ed.mode.enter", 0, "E"},
    {"f", "ed.mode.enter", 0, "F"},
};

static const BindRow frozen_W[] = {
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"S-<left>", "ed.sel.extend.left", 0, NULL},
    {"S-<right>", "ed.sel.extend.right", 0, NULL},
    {"S-<up>", "ed.sel.extend.up", 0, NULL},
    {"S-<down>", "ed.sel.extend.down", 0, NULL},
    {"C-v", "ed.clip.paste", 0, NULL},
    {"A-<left>", "ed.move.unit.prev_alt", 0, NULL},
    {"A-<right>", "ed.shadow.accept_word", 0, NULL},
    {"A-S-<right>", "ed.shadow.accept_word_alt", 0, NULL},
    {"A-<up>", "ed.git.hunk.prev", 0, NULL},
    {"A-<down>", "ed.git.hunk.next", 0, NULL},
    {"A-S-<down>", "ed.shadow.accept_line", 0, NULL},
    {"A-<cr>", "ed.shadow.accept_all", 0, NULL},
    {"A-]", "ed.shadow.next", 0, NULL},
    {"A-[", "ed.shadow.prev", 0, NULL},
    {"C-<left>", "ed.move.word.sub_prev", 0, NULL},
    {"C-<right>", "ed.move.word.sub_next", 0, NULL},
    {"<home>", "ed.move.line.home_toggle", 0, NULL},
    {"<end>", "ed.move.unit.end", 0, NULL},
    {"l", "ed.mode.enter", 0, "L"},
    {"b", "ed.mode.enter", 0, "B"},
    {"i", "ed.mode.enter", 0, "I"},
    {"h", "ed.mode.enter", 0, "H"},
    {"f", "ed.mode.enter", 0, "F"},
    {"e", "ed.mode.enter", 0, "E"},
    {":", "ed.mode.enter", 0, "E"},
    {"!", "ed.shell.open", 0, NULL},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"q", "ed.macro.record", 0, NULL},
    {"@", "ed.macro.replay", 0, NULL},
    {"@ @", "ed.macro.replay_last", 0, NULL},
    {"Q", "ed.macro.list", 0, NULL},
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
    {"C-1", "ed.tab.goto_bar", 1, NULL},
    {"C-2", "ed.tab.goto_bar", 2, NULL},
    {"C-3", "ed.tab.goto_bar", 3, NULL},
    {"C-4", "ed.tab.goto_bar", 4, NULL},
    {"C-5", "ed.tab.goto_bar", 5, NULL},
    {"C-6", "ed.tab.goto_bar", 6, NULL},
    {"C-7", "ed.tab.goto_bar", 7, NULL},
    {"C-8", "ed.tab.goto_bar", 8, NULL},
    {"C-9", "ed.tab.goto_bar", 9, NULL},
    {"C-0", "ed.tab.goto_bar", 0, NULL},
};

static const BindRow frozen_B[] = {
    {"<left>", "ed.move.unit.home", 0, NULL},
    {"<right>", "ed.move.unit.end", 0, NULL},
    {"<up>", "ed.move.unit.prev", 0, NULL},
    {"<down>", "ed.move.unit.next", 0, NULL},
    {"S-<left>", "ed.sel.extend.left", 0, NULL},
    {"S-<right>", "ed.sel.extend.right", 0, NULL},
    {"S-<up>", "ed.sel.extend.up", 0, NULL},
    {"S-<down>", "ed.sel.extend.down", 0, NULL},
    {"C-v", "ed.clip.paste", 0, NULL},
    {"A-<left>", "ed.move.block.match_prev", 0, NULL},
    {"A-<right>", "ed.shadow.accept_word", 0, NULL},
    {"A-S-<right>", "ed.shadow.accept_word_alt", 0, NULL},
    {"A-<up>", "ed.git.hunk.prev", 0, NULL},
    {"A-<down>", "ed.git.hunk.next", 0, NULL},
    {"A-S-<up>", "ed.sel.unit.expand", 0, NULL},
    {"A-S-<down>", "ed.shadow.accept_line", 0, NULL},
    {"A-<cr>", "ed.shadow.accept_all", 0, NULL},
    {"A-]", "ed.shadow.next", 0, NULL},
    {"A-[", "ed.shadow.prev", 0, NULL},
    {"l", "ed.mode.enter", 0, "L"},
    {"w", "ed.mode.enter", 0, "W"},
    {"i", "ed.mode.enter", 0, "I"},
    {"h", "ed.mode.enter", 0, "H"},
    {"f", "ed.mode.enter", 0, "F"},
    {"e", "ed.mode.enter", 0, "E"},
    {":", "ed.mode.enter", 0, "E"},
    {"!", "ed.shell.open", 0, NULL},
    {"<esc>", "ed.mode.escape", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    {"q", "ed.macro.record", 0, NULL},
    {"@", "ed.macro.replay", 0, NULL},
    {"@ @", "ed.macro.replay_last", 0, NULL},
    {"Q", "ed.macro.list", 0, NULL},
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
    {"C-1", "ed.tab.goto_bar", 1, NULL},
    {"C-2", "ed.tab.goto_bar", 2, NULL},
    {"C-3", "ed.tab.goto_bar", 3, NULL},
    {"C-4", "ed.tab.goto_bar", 4, NULL},
    {"C-5", "ed.tab.goto_bar", 5, NULL},
    {"C-6", "ed.tab.goto_bar", 6, NULL},
    {"C-7", "ed.tab.goto_bar", 7, NULL},
    {"C-8", "ed.tab.goto_bar", 8, NULL},
    {"C-9", "ed.tab.goto_bar", 9, NULL},
    {"C-0", "ed.tab.goto_bar", 0, NULL},
};

static const BindRow frozen_I[] = {
    {"A-h", "ed.mode.enter", 0, "H"},
    {"<esc>", "ed.shadow.dismiss", 0, NULL},
    {"C-g", "ed.ui.message_expand", 0, NULL},
    /*
     * Alt+arrow is the word jump in Insert mode, and Alt+Right stays
     * contextual: it accepts a ghost word only while a suggestion is
     * showing.  L, W and B keep the plain accept -- their Alt arrows are
     * already unit motions.
     */
    {"A-<left>", "ed.move.word.prev", 0, NULL},
    {"A-b", "ed.move.word.prev", 0, NULL},
    {"A-f", "ed.move.word.next", 0, NULL},
    {"A-<right>", "ed.shadow.accept_or_word", 0, NULL},
    {"A-S-<right>", "ed.shadow.accept_word_alt", 0, NULL},
    {"A-<down>", "ed.shadow.accept_line", 0, NULL},
    {"A-<cr>", "ed.shadow.accept_all", 0, NULL},
    {"A-]", "ed.shadow.next", 0, NULL},
    {"A-[", "ed.shadow.prev", 0, NULL},
    {"C-<space>", "ed.compl.complete", 0, NULL},
    /* Signature help moved off C-k so kill-to-end could have it. */
    {"A-k", "ed.lsp.signature", 0, NULL},
    {"<cr>", "ed.edit.insert.newline", 0, NULL},
    {"<tab>", "ed.edit.insert.tab", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"<left>", "ed.move.unit.prev", 0, NULL},
    {"<right>", "ed.move.unit.next", 0, NULL},
    {"<up>", "ed.move.line.up", 0, NULL},
    {"<down>", "ed.move.line.down", 0, NULL},
    {"S-<left>", "ed.sel.extend.left", 0, NULL},
    {"S-<right>", "ed.sel.extend.right", 0, NULL},
    {"S-<up>", "ed.sel.extend.up", 0, NULL},
    {"S-<down>", "ed.sel.extend.down", 0, NULL},
    {"C-v", "ed.clip.paste", 0, NULL},
    /* Sprint 57.29: copy and cut; inert in I, which has no selection
     * (its Shift+arrows enter H, where they act). */
    {"C-c", "ed.clip.copy", 0, NULL},
    {"C-x", "ed.clip.cut", 0, NULL},
    {"<home>", "ed.move.line.home_toggle", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
    /*
     * Ctrl+arrow is the LINE's start and end here, the same pair
     * <home>/<end> give.  Deliberately NOT the word jump the usual
     * convention puts on Ctrl+arrow -- in yew that is Alt+arrow, above.
     */
    {"C-a", "ed.move.line.home_toggle", 0, NULL},
    {"C-e", "ed.move.line.end", 0, NULL},
    {"C-<left>", "ed.move.line.home_toggle", 0, NULL},
    {"C-<right>", "ed.move.line.end", 0, NULL},
    /*
     * The readline/Emacs editing set.  These fan out to every cursor and
     * are recordable; the prompt binds the same commands.  C-w and
     * Alt+Backspace are one kill under two spellings; C-_ and C-/ are
     * one chord under two protocols.
     */
    {"C-w", "ed.edit.kill.word_prev", 0, NULL},
    {"A-<bs>", "ed.edit.kill.word_prev", 0, NULL},
    {"A-d", "ed.edit.kill.word_next", 0, NULL},
    /* Sprint 57.28 §3: Insert-mode parity with the prompt. */
    {"A-<del>", "ed.edit.kill.word_next", 0, NULL},
    {"C-u", "ed.edit.kill.to_home", 0, NULL},
    {"C-k", "ed.edit.kill.to_end", 0, NULL},
    /* C-d is the forward delete and nothing else; it never quits. */
    {"C-d", "ed.edit.delete.grapheme", 0, NULL},
    {"C-y", "ed.edit.kill.yank", 0, NULL},
    {"A-y", "ed.edit.kill.yank_pop", 0, NULL},
    {"C-p", "ed.move.line.up", 0, NULL},
    {"C-n", "ed.move.line.down", 0, NULL},
    {"C-_", "ed.edit.undo", 0, NULL},
    {"C-/", "ed.edit.undo", 0, NULL},
    {"C-t", "ed.edit.transpose.chars", 0, NULL},
    {"A-t", "ed.edit.transpose.words", 0, NULL},
    {"A-u", "ed.edit.case.upper_word", 0, NULL},
    {"A-l", "ed.edit.case.lower_word", 0, NULL},
    {"A-c", "ed.edit.case.cap_word", 0, NULL},
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
    {"C-1", "ed.tab.goto_bar", 1, NULL},
    {"C-2", "ed.tab.goto_bar", 2, NULL},
    {"C-3", "ed.tab.goto_bar", 3, NULL},
    {"C-4", "ed.tab.goto_bar", 4, NULL},
    {"C-5", "ed.tab.goto_bar", 5, NULL},
    {"C-6", "ed.tab.goto_bar", 6, NULL},
    {"C-7", "ed.tab.goto_bar", 7, NULL},
    {"C-8", "ed.tab.goto_bar", 8, NULL},
    {"C-9", "ed.tab.goto_bar", 9, NULL},
    {"C-0", "ed.tab.goto_bar", 0, NULL},
};

static const BindRow frozen_E[] = {
    {"<left>", "ed.move.char.prev", 0, NULL},
    {"C-b", "ed.move.char.prev", 0, NULL},
    /*
     * Sprint 18.5 §7.  Right accepts the inline suggestion when one is
     * showing and otherwise moves one grapheme, so the key never stops
     * being a motion.  Sprint 57.28 §3: C-f is fish's spelling of it.
     */
    {"<right>", "ed.cmdline.ghost.accept", 0, NULL},
    {"C-f", "ed.cmdline.ghost.accept", 0, NULL},
    /* Sprint 57.26 §3: one word of a history ghost, fish's A-f; one
     * word right without one (57.28 §3). */
    {"A-f", "ed.cmdline.ghost.accept_word", 0, NULL},
    {"A-<right>", "ed.cmdline.ghost.accept_word", 0, NULL},
    {"A-b", "ed.move.word.prev", 0, NULL},
    {"A-<left>", "ed.move.word.prev", 0, NULL},
    {"<home>", "ed.move.line.home_toggle", 0, NULL},
    {"<end>", "ed.move.line.end", 0, NULL},
    {"C-a", "ed.move.line.home", 0, NULL},
    {"C-<left>", "ed.move.line.home", 0, NULL},
    /* Sprint 57.28 §3: fish's C-e takes a whole ghost, else line end;
     * C-<right> is its synonym, as C-<left> is C-a's. */
    {"C-e", "ed.cmdline.ghost.accept_line", 0, NULL},
    {"C-<right>", "ed.cmdline.ghost.accept_line", 0, NULL},
    {"<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    /* ^H from a legacy terminal decodes as C-<bs>; kitty sends C-h. */
    {"C-h", "ed.edit.delete.grapheme_left", 0, NULL},
    {"C-<bs>", "ed.edit.delete.grapheme_left", 0, NULL},
    {"<del>", "ed.edit.delete.grapheme", 0, NULL},
    {"C-d", "ed.edit.delete.grapheme", 0, NULL},
    /*
     * Sprint 57.28 §3: the Insert-mode readline commands, on the
     * prompt's Win, so every kill feeds the one yank stack.  C-w is
     * bash's unix-word-rubout (blank-delimited); A-<bs> the word kill.
     */
    {"C-w", "ed.edit.kill.ws_word_prev", 0, NULL},
    {"A-<bs>", "ed.edit.kill.word_prev", 0, NULL},
    {"A-d", "ed.edit.kill.word_next", 0, NULL},
    {"A-<del>", "ed.edit.kill.word_next", 0, NULL},
    {"C-u", "ed.edit.kill.to_home", 0, NULL},
    {"C-k", "ed.edit.kill.to_end", 0, NULL},
    {"C-t", "ed.edit.transpose.chars", 0, NULL},
    {"A-t", "ed.edit.transpose.words", 0, NULL},
    {"A-u", "ed.edit.case.upper_word", 0, NULL},
    {"A-l", "ed.edit.case.lower_word", 0, NULL},
    {"A-c", "ed.edit.case.cap_word", 0, NULL},
    {"C-y", "ed.edit.kill.yank", 0, NULL},
    {"A-y", "ed.edit.kill.yank_pop", 0, NULL},
    {"A-.", "ed.cmdline.last_arg", 0, NULL},
    /*
     * Sprint 57.30 §1 (replacing 57.17 §2's rule): the arrows are
     * HISTORY -- fish's substring search on the typed text -- unless Tab
     * has entered the completion table, where they move rows.  The live
     * table open while a token is typed never takes an arrow; Up off its
     * top row goes back to history, Down off its last row back to the
     * line.  C-p and C-n are the arrows exactly; completion next/prev
     * stay on Tab and S-Tab.
     */
    {"<up>", "ed.cmdline.up", 0, NULL},
    {"<down>", "ed.cmdline.down", 0, NULL},
    {"C-p", "ed.cmdline.up", 0, NULL},
    {"C-n", "ed.cmdline.down", 0, NULL},
    {"<tab>", "ed.cmdline.complete_next", 0, NULL},
    {"S-<tab>", "ed.cmdline.complete_prev", 0, NULL},
    {"<pgdn>", "ed.cmdline.menu.page_next", 0, NULL},
    {"<pgup>", "ed.cmdline.menu.page_prev", 0, NULL},
    /* Sprint 57.28 §3: readline wins the clashes.  A-r is insert-register
     * (C-r keeps it until 57.30), C-q literal-next, C-v the system
     * clipboard, C-y yank, A-/ redo. */
    {"C-r", "ed.cmdline.insert_register", 0, NULL},
    {"A-r", "ed.cmdline.insert_register", 0, NULL},
    {"C-q", "ed.cmdline.literal_next", 0, NULL},
    {"C-v", "ed.clip.paste", 0, NULL},
    {"C-z", "ed.edit.undo", 0, NULL},
    {"C-_", "ed.edit.undo", 0, NULL},
    {"C-/", "ed.edit.undo", 0, NULL},
    {"A-/", "ed.edit.redo", 0, NULL},
    {"<cr>", "ed.cmdline.accept", 0, NULL},
    {"<esc>", "ed.cmdline.cancel", 0, NULL},
    {"C-g", "ed.cmdline.cancel", 0, NULL},
    /* Sprint 57.29: the prompt's own selection, and the clipboard. */
    {"S-<left>", "ed.sel.extend.left", 0, NULL},
    {"S-<right>", "ed.sel.extend.right", 0, NULL},
    {"A-S-<left>", "ed.sel.extend.word_prev", 0, NULL},
    {"A-S-<right>", "ed.sel.extend.word_next", 0, NULL},
    {"S-<home>", "ed.sel.extend.line_home", 0, NULL},
    {"S-<end>", "ed.sel.extend.line_end", 0, NULL},
    {"C-S-<left>", "ed.sel.extend.line_home", 0, NULL},
    {"C-S-<right>", "ed.sel.extend.line_end", 0, NULL},
    {"C-c", "ed.cmdline.copy_or_cancel", 0, NULL},
    {"C-x", "ed.clip.cut", 0, NULL},
};

static const BindRow frozen_F[] = {
    {"<esc>", "ed.git.mode.leave", 0, NULL},
    {"C-w s", "ed.git.open_split_h", 0, NULL},
    {"C-w v", "ed.git.open_split_v", 0, NULL},
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
    } else if (want->type == (u8)YEW_OPT_STRLIST) {
        YEW_ASSERT_EQ_U64(actual->as.list.len, want->as.list.len);
        YEW_ASSERT_NULL(actual->as.list.v);
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
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 383U);
    for (mode = 0U; mode < (u32)YEW_MODE__N; mode++) {
        if (mode != (u32)YEW_MODE_H)
            panic_rows += yew_keymap_binding_count(&ed.mode_keys[mode]);
        assert_frozen_mode(&ed, (Mode)mode, rows[mode], counts[mode]);
    }
    YEW_ASSERT_EQ_U64(panic_rows, 60U);
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

void test_runtime_defaults_frozen_globals_and_dotted_keys_execute(void)
{
    static const char source[] =
        "import win\n"
        "import ed\n"
        "let cursors = win.current().cursors()\n"
        "if cursors[0].pos() != 0 { error(\"bad cursor\") }\n"
        "set({ \"clipboard.sync\": \"yank\", "
        "\"search.smartcase\": true })\n"
        "bind(\"L\", \"Z\", \"ed.nop\")\n"
        "on(\"ed.idle\", fn() nil)\n"
        "ed.msg(\"modal globals registered\")\n";
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, source, sizeof(source) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_bind_active_count(&ed), 1U);
    YEW_ASSERT_EQ_STR(ed.msg.text, "modal globals registered");
    yew_ed_free(&ed);
}
