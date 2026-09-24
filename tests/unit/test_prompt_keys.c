#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.28 §3: the `:` prompt's readline editing set, every row
 * driven as REAL keys through yew_ed_handle_key in E mode against the
 * loaded runtime -- so what these pin is what a user's fingers get,
 * binding, dispatcher sequence and all, not what a command table says.
 *
 * Modified chords carry no text, as the decoder reports them.
 */
#include "harness.h"

#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "term/grid.h"
#include "text/clipboard.h"
#include "ui/cmdhist.h"
#include "ui/cmdline.h"
#include "ui/draw.h"

typedef struct PkFix {
    Ed ed;
} PkFix;

static Key pk_key(u32 code, u16 mods)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.mods = mods;
    if (mods == 0U && code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static void pk_send(PkFix *f, u32 code, u16 mods)
{
    yew_ed_handle_key(&f->ed, pk_key(code, mods), 0);
    YEW_ASSERT_EQ_U64(f->ed.last_status, YEW_CMD_OK);
}

/* A chord, and the command it must have reached. */
static void pk_run(PkFix *f, u32 code, u16 mods, const char *command)
{
    pk_send(f, code, mods);
    YEW_ASSERT_EQ_U64(f->ed.last_cmd.v,
                      yew_cmd_lookup(command, (u32)strlen(command)).v);
}

static void pk_type(PkFix *f, const char *text)
{
    for (; *text != '\0'; text++)
        pk_send(f, (u32)(u8)*text, 0U);
}

static void pk_set_option(Ed *ed, const char *name, const char *value)
{
    OptVal v;
    const char *err = NULL;

    (void)memset(&v, 0, sizeof(v));
    v.type = (u8)YEW_OPT_ENUM;
    v.as.str.s = value;
    v.as.str.len = (u32)strlen(value);
    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, name,
                           (u32)strlen(name), &v, &err));
}

/* Open `:` with a real key, give its history `own` (oldest first), and
 * type `line` with real keys. */
static void pk_prompt(PkFix *f, const char *const *own, size_t n_own,
                      const char *line)
{
    size_t i;

    if (f->ed.cmdline.active)
        pk_run(f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    YEW_ASSERT_EQ_U64(f->ed.mode, YEW_MODE_L);
    pk_send(f, (u32)':', 0U);
    YEW_ASSERT(f->ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f->ed.mode, YEW_MODE_E);
    for (i = 0U; i < n_own; i++)
        yew_hist_add(f->ed.cmdline.history, own[i]);
    pk_type(f, line);
}

static void pk_init(PkFix *f)
{
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_test_load_runtime(&f->ed);
    /* Session-lived histories, and no shell's history file in a ghost. */
    f->ed.clean = true;
    f->ed.regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    pk_set_option(&f->ed, "shell.suggest_history", "yew");
}

static void pk_free(PkFix *f)
{
    if (f->ed.cmdline.active)
        yew_cmdline_close(&f->ed, false);
    yew_ed_free(&f->ed);
}

static void pk_text(PkFix *f, const char *want)
{
    Bytebuf b;

    bytebuf_init(&b);
    yew_cmdline_text(&f->ed, &b);
    YEW_ASSERT_EQ_U64(b.len, strlen(want));
    if (b.len != 0U)
        YEW_ASSERT_EQ_MEM(b.data, want, b.len);
    bytebuf_free(&b);
}

static u64 pk_caret(const PkFix *f)
{
    return f->ed.cmdline.cur.pos.v;
}

static void pk_ghost(PkFix *f, const char *want)
{
    size_t n = 0U;
    const char *ghost = yew_cmdline_ghost(&f->ed, &n);

    if (want == NULL) {
        YEW_ASSERT(ghost == NULL || n == 0U);
        return;
    }
    YEW_ASSERT_NOT_NULL(ghost);
    YEW_ASSERT_EQ_U64(n, strlen(want));
    YEW_ASSERT_EQ_MEM(ghost, want, n);
}

/* ------------------------------------------------------------ motions */

void test_prompt_keys_char_word_and_line_motions(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e one two");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_run(&f, (u32)'b', YEW_MOD_CTRL, "ed.move.char.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_run(&f, (u32)'f', YEW_MOD_CTRL, "ed.cmdline.ghost.accept");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    pk_run(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.move.word.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_run(&f, YEW_KEY_RIGHT, YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_run(&f, YEW_KEY_LEFT, YEW_MOD_CTRL, "ed.move.line.home");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    pk_run(&f, (u32)'e', YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    /* No ghost anywhere but the end: A-f and A-<right> are word NEXT. */
    pk_run(&f, (u32)'f', YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_run(&f, YEW_KEY_RIGHT, YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    pk_text(&f, "e one two");
    pk_free(&f);
}

/*
 * The contextual keys WITH a ghost: A-f and A-<right> take one ghost
 * word; C-e, C-<right> and C-f take all of it (fish).  Away from the end
 * there is no ghost, so A-f is the word motion again.
 */
void test_prompt_keys_ghost_accept_is_contextual(void)
{
    static const char *const own[] = {"!git status --short"};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, 1U, "!git st");
    pk_ghost(&f, "atus --short");
    pk_run(&f, (u32)'f', YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    pk_text(&f, "!git status ");
    pk_ghost(&f, "--short");
    pk_run(&f, YEW_KEY_RIGHT, YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    pk_text(&f, "!git status --short");
    pk_ghost(&f, NULL);

    pk_prompt(&f, NULL, 0U, "!git st");
    pk_ghost(&f, "atus --short");
    pk_run(&f, (u32)'e', YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    pk_text(&f, "!git status --short");

    pk_prompt(&f, NULL, 0U, "!git st");
    pk_run(&f, YEW_KEY_RIGHT, YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    pk_text(&f, "!git status --short");

    pk_prompt(&f, NULL, 0U, "!git st");
    pk_run(&f, (u32)'f', YEW_MOD_CTRL, "ed.cmdline.ghost.accept");
    pk_text(&f, "!git status --short");

    /* The caret off the end: no ghost, so A-f is one word right. */
    pk_prompt(&f, NULL, 0U, "!git st");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_ghost(&f, NULL);
    pk_run(&f, (u32)'f', YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    YEW_ASSERT(pk_caret(&f) > 0U && pk_caret(&f) < 7U);
    pk_text(&f, "!git st");
    pk_free(&f);
}

/* ------------------------------------------------------------ deletes */

/* C-d deletes forward and never cancels, even on an empty line; C-h is
 * Backspace for terminals that send ^H. */
void test_prompt_keys_ctrl_d_and_ctrl_h(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "abcd");
    pk_run(&f, (u32)'h', YEW_MOD_CTRL, "ed.edit.delete.grapheme_left");
    pk_text(&f, "abc");
    /* A legacy terminal's ^H byte decodes as C-<bs>. */
    pk_run(&f, YEW_KEY_BACKSPACE, YEW_MOD_CTRL,
           "ed.edit.delete.grapheme_left");
    pk_text(&f, "ab");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_run(&f, (u32)'d', YEW_MOD_CTRL, "ed.edit.delete.grapheme");
    pk_text(&f, "b");
    pk_run(&f, (u32)'d', YEW_MOD_CTRL, "ed.edit.delete.grapheme");
    pk_text(&f, "");
    pk_run(&f, (u32)'d', YEW_MOD_CTRL, "ed.edit.delete.grapheme");
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_E);
    YEW_ASSERT(!f.ed.quit);
    pk_free(&f);
}

/* C-w is bash's unix-word-rubout: a path goes whole.  A-<bs> is the
 * word-character kill. */
void test_prompt_keys_ctrl_w_is_whitespace_and_alt_bs_is_word(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e a/b/c");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_text(&f, "e ");
    pk_prompt(&f, NULL, 0U, "e a/b/c  ");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_text(&f, "e ");
    pk_prompt(&f, NULL, 0U, "e a/b/c");
    pk_run(&f, YEW_KEY_BACKSPACE, YEW_MOD_ALT, "ed.edit.kill.word_prev");
    pk_text(&f, "e a/b/");
    pk_free(&f);
}

void test_prompt_keys_kill_forward_and_to_the_ends(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "one two three");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_run(&f, (u32)'d', YEW_MOD_ALT, "ed.edit.kill.word_next");
    pk_text(&f, "two three");
    pk_run(&f, YEW_KEY_DELETE, YEW_MOD_ALT, "ed.edit.kill.word_next");
    pk_text(&f, "three");
    /* Consecutive: A-d A-<del> made ONE entry. */
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 1U);
    YEW_ASSERT_EQ_MEM(yew_yank_at(&f.ed.yank, 0U)->data, "one two ", 8U);

    pk_prompt(&f, NULL, 0U, "left right");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    pk_run(&f, (u32)'k', YEW_MOD_CTRL, "ed.edit.kill.to_end");
    pk_text(&f, "left ");
    pk_run(&f, (u32)'u', YEW_MOD_CTRL, "ed.edit.kill.to_home");
    pk_text(&f, "");
    /* C-k then C-u: consecutive, the backward kill in FRONT. */
    YEW_ASSERT_EQ_U64(yew_yank_at(&f.ed.yank, 0U)->len, 10U);
    YEW_ASSERT_EQ_MEM(yew_yank_at(&f.ed.yank, 0U)->data, "left right",
                      10U);
    pk_free(&f);
}

/* ----------------------------------------------------- transpose, case */

void test_prompt_keys_transpose_and_word_case(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "ab");
    pk_run(&f, (u32)'t', YEW_MOD_CTRL, "ed.edit.transpose.chars");
    pk_text(&f, "ba");
    pk_prompt(&f, NULL, 0U, "one two");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    pk_run(&f, (u32)'t', YEW_MOD_ALT, "ed.edit.transpose.words");
    pk_text(&f, "two one");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_run(&f, (u32)'u', YEW_MOD_ALT, "ed.edit.case.upper_word");
    pk_text(&f, "TWO one");
    pk_run(&f, (u32)'c', YEW_MOD_ALT, "ed.edit.case.cap_word");
    pk_text(&f, "TWO One");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_run(&f, (u32)'l', YEW_MOD_ALT, "ed.edit.case.lower_word");
    pk_text(&f, "two One");
    pk_free(&f);
}

/* ------------------------------------------------------ yank, yank-pop */

/* Three separate kills, then C-y A-y A-y A-y: newest, older, oldest, and
 * wrapping back to the newest. */
void test_prompt_keys_yank_then_yank_pop_wraps(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "one two three");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_send(&f, YEW_KEY_BACKSPACE, 0U);
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_send(&f, YEW_KEY_BACKSPACE, 0U);
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_text(&f, "");
    YEW_ASSERT_EQ_U64(f.ed.yank.len, 3U);
    pk_type(&f, "x ");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "x one");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "x two");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "x three");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 7U);
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "x one");
    pk_free(&f);
}

/*
 * The prompt's kills and yanks never touch the register file: the struct
 * (every length, head and pointer) and the unnamed register's bytes are
 * byte-identical after C-w C-u C-k C-y A-y, and no clipboard write is
 * queued even under clipboard.sync = all.
 */
void test_prompt_keys_kills_leave_registers_untouched(void)
{
    PkFix f;
    Registers before;
    RegVal seeded;

    pk_init(&f);
    f.ed.regs.clipboard_sync = YEW_CLIP_SYNC_ALL;
    yew_regval_init(&seeded);
    bytebuf_append(&seeded.bytes, "unnamed", 7U);
    yew_reg_yank(&f.ed.regs, 0U, &seeded);
    yew_regval_free(&seeded);
    yew_clip_reset();
    pk_prompt(&f, NULL, 0U, "one two three");
    (void)memcpy(&before, &f.ed.regs, sizeof(before));
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_run(&f, (u32)'b', YEW_MOD_CTRL, "ed.move.char.prev");
    pk_run(&f, (u32)'u', YEW_MOD_CTRL, "ed.edit.kill.to_home");
    pk_run(&f, (u32)'k', YEW_MOD_CTRL, "ed.edit.kill.to_end");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "three");
    YEW_ASSERT_EQ_MEM(&before, &f.ed.regs, sizeof(before));
    YEW_ASSERT_EQ_U64(f.ed.regs.unnamed.bytes.len, 7U);
    YEW_ASSERT_EQ_MEM(f.ed.regs.unnamed.bytes.data, "unnamed", 7U);
    YEW_ASSERT(!yew_clip_pending());
    pk_free(&f);
    yew_clip_reset();
}

/* A-y with no yank just before it: a message, and nothing changes. */
void test_prompt_keys_yank_pop_without_a_yank(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "one two");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_type(&f, "z");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "one z");
    YEW_ASSERT(f.ed.msg.active);
    YEW_ASSERT(strstr(f.ed.msg.text, "A-y follows a yank") != NULL);
    /* A motion between the yank and A-y breaks it too. */
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "one ztwo");
    pk_run(&f, (u32)'b', YEW_MOD_CTRL, "ed.move.char.prev");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "one ztwo");
    pk_free(&f);
}

/* The yank stack is shared: a kill in the prompt yanks in Insert mode,
 * and back. */
void test_prompt_keys_yank_stack_is_shared_with_insert_mode(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e shared");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    pk_send(&f, (u32)'i', 0U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.buffer.tb), 6U);
    pk_type(&f, " more");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.word_prev");
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    pk_prompt(&f, NULL, 0U, "");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "more");
    pk_free(&f);
}

/* A kill holding a newline yanks into the one-line prompt as a blank. */
void test_prompt_keys_yank_folds_newlines_into_the_prompt(void)
{
    PkFix f;

    pk_init(&f);
    yew_yank_kill(&f.ed.yank, (const u8 *)"a\nb", 3U, YEW_KILL_FORWARD,
                  0U, NULL);
    pk_prompt(&f, NULL, 0U, "");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "a b");
    pk_free(&f);
}

/* -------------------------------------------------------------- undo */

/* Each kill, yank and yank-pop is ONE undo step; A-/ redoes. */
void test_prompt_keys_kill_yank_pop_are_one_undo_step_each(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "one two");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_send(&f, YEW_KEY_BACKSPACE, 0U);
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_text(&f, "");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "one");
    pk_run(&f, (u32)'y', YEW_MOD_ALT, "ed.edit.kill.yank_pop");
    pk_text(&f, "two");
    pk_run(&f, (u32)'_', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "one");
    pk_run(&f, (u32)'/', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "");
    pk_run(&f, (u32)'z', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "one");
    pk_run(&f, (u32)'/', YEW_MOD_ALT, "ed.edit.redo");
    pk_text(&f, "");
    pk_run(&f, (u32)'/', YEW_MOD_ALT, "ed.edit.redo");
    pk_text(&f, "one");
    pk_free(&f);
}

/* --------------------------------------------------------- last argument */

/* A-. takes the newest entry's last word; again, the next older one's.
 * A bang entry's word is the RAW shell word, quotes and all; entries
 * with no last word are skipped; past the oldest it stays put. */
void test_prompt_keys_last_arg_walks_older_entries(void)
{
    static const char *const own[] = {
        "!ls -la /tmp", "e foo.c", "w ", "!cp a \"my file\""};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, YEW_ARRAY_LEN(own), "e ");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e \"my file\"");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e foo.c");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e /tmp");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e /tmp");
    /* Anything between starts over at the newest. */
    pk_type(&f, " ");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e /tmp \"my file\"");
    pk_free(&f);
}

/* ------------------------------------------- literal, register, paste */

/* C-q is literal-next now (readline's quoted-insert): the next key's text
 * goes in as-is, a Tab included. */
void test_prompt_keys_ctrl_q_inserts_the_next_key_literally(void)
{
    PkFix f;
    Key tab = pk_key(YEW_KEY_TAB, 0U);

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "a");
    pk_send(&f, (u32)'q', YEW_MOD_CTRL);
    tab.ntext = 1U;
    tab.text[0] = (u8)'\t';
    yew_ed_handle_key(&f.ed, tab, 0);
    YEW_ASSERT_EQ_U64(f.ed.last_status, YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(f.ed.last_cmd.v,
                      yew_cmd_lookup("ed.cmdline.literal_next", 23U).v);
    pk_text(&f, "a\t");
    pk_free(&f);
}

/* A-r inserts a register; the register name is the next key, printable
 * or not.  (C-r did too until Sprint 57.30 gave it to history search.) */
void test_prompt_keys_alt_r_inserts_a_register(void)
{
    PkFix f;
    RegVal value;

    pk_init(&f);
    yew_regval_init(&value);
    bytebuf_append(&value.bytes, "reg", 3U);
    yew_reg_yank(&f.ed.regs, (u8)'a', &value);
    yew_regval_free(&value);
    pk_prompt(&f, NULL, 0U, "x ");
    pk_send(&f, (u32)'r', YEW_MOD_ALT);
    pk_send(&f, (u32)'a', 0U);
    YEW_ASSERT_EQ_U64(f.ed.last_cmd.v,
                      yew_cmd_lookup("ed.cmdline.insert_register", 26U).v);
    pk_text(&f, "x reg");
    pk_send(&f, (u32)'r', YEW_MOD_ALT);
    pk_send(&f, (u32)'a', 0U);
    pk_text(&f, "x regreg");
    /* C-r captures nothing now: the next key is typed text. */
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT(f.ed.cmdline.hsearch);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_text(&f, "x regreg");
    YEW_ASSERT(!f.ed.cmdline.hsearch);
    pk_free(&f);
}

/* The fakeclip helper built beside the test binary. */
static void pk_fakeclip_path(char fake[PATH_MAX])
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    int n;

    if (slash != NULL) {
        size_t prefix = (size_t)(slash - program);

        YEW_ASSERT(prefix + sizeof("/fakeclip") <= PATH_MAX);
        (void)memcpy(fake, program, prefix);
        (void)memcpy(fake + prefix, "/fakeclip", sizeof("/fakeclip"));
    } else {
        n = snprintf(fake, PATH_MAX, "./fakeclip");
        YEW_ASSERT(n > 0 && n < PATH_MAX);
    }
}

static void pk_fake_clipboard(char path[PATH_MAX], const char *bytes)
{
    char fake[PATH_MAX];
    char value[PATH_MAX * 3U];
    FILE *fp;
    int fd;
    int n;

    pk_fakeclip_path(fake);
    n = snprintf(path, PATH_MAX, "/tmp/yew-pkclip-XXXXXX");
    YEW_ASSERT(n > 0 && (size_t)n < PATH_MAX);
    fd = mkstemp(path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, strlen(bytes), fp), strlen(bytes));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    n = snprintf(value, sizeof(value), "cmd:/bin/true|%s %s read", fake,
                 path);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(value));
    YEW_ASSERT_EQ_I64(setenv("YEW_CLIPBOARD", value, 1), 0);
    yew_clip_reset();
}

/* C-v is the system clipboard now, landing in the prompt, newlines
 * folded to a blank as any paste into the one-line prompt is. */
void test_prompt_keys_ctrl_v_pastes_the_system_clipboard(void)
{
    PkFix f;
    char path[PATH_MAX];
    const char *env = getenv("YEW_CLIPBOARD");
    char *saved = env == NULL ? NULL : strdup(env);

    pk_fake_clipboard(path, "git log\n--oneline");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!");
    pk_run(&f, (u32)'v', YEW_MOD_CTRL, "ed.clip.paste");
    pk_text(&f, "!git log --oneline");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 18U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_E);
    pk_free(&f);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    yew_clip_shutdown();
    if (saved != NULL)
        YEW_ASSERT_EQ_I64(setenv("YEW_CLIPBOARD", saved, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("YEW_CLIPBOARD"), 0);
    free(saved);
    yew_clip_reset();
}

/* ============================================ Sprint 57.29: selection */

static void pk_sel(PkFix *f, u64 lo, u64 hi)
{
    Span span = {0U, 0U};

    YEW_ASSERT(yew_cmdline_selection(&f->ed, &span));
    YEW_ASSERT_EQ_U64(span.lo, lo);
    YEW_ASSERT_EQ_U64(span.hi, hi);
    YEW_ASSERT_EQ_U64(f->ed.mode, YEW_MODE_E);
}

static void pk_nosel(PkFix *f)
{
    YEW_ASSERT(!yew_cmdline_selection(&f->ed, NULL));
}

/* A bracketed paste: how text beyond ASCII reaches the prompt here. */
static void pk_paste(PkFix *f, const char *bytes)
{
    yew_ed_handle_paste(&f->ed, NULL, 0U, false);
    yew_ed_handle_paste(&f->ed, (const u8 *)bytes, strlen(bytes), false);
    yew_ed_handle_paste(&f->ed, NULL, 0U, true);
}

static void pk_shift(PkFix *f, u32 code, u16 mods, const char *command)
{
    pk_run(f, code, (u16)(mods | YEW_MOD_SHIFT), command);
}

/*
 * One file behind both halves of a fake system clipboard: a copy lands
 * in it once the write is flushed, and a paste reads it back.  `path`
 * starts holding `bytes`.  The previous YEW_CLIPBOARD comes back from
 * pk_clip_done, so no test ever reaches the real clipboard.
 */
typedef struct PkClip {
    char path[PATH_MAX];
    char *saved;
} PkClip;

static void pk_clip_open(PkClip *c, const char *bytes)
{
    const char *env = getenv("YEW_CLIPBOARD");
    char fake[PATH_MAX];
    char value[PATH_MAX * 3U];
    FILE *fp;
    int fd;
    int n;

    c->saved = env == NULL ? NULL : strdup(env);
    pk_fakeclip_path(fake);
    n = snprintf(c->path, sizeof(c->path), "/tmp/yew-pksel-XXXXXX");
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(c->path));
    fd = mkstemp(c->path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    fp = fopen(c->path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, strlen(bytes), fp), strlen(bytes));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    n = snprintf(value, sizeof(value), "cmd:%s %s write|%s %s read", fake,
                 c->path, fake, c->path);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(value));
    YEW_ASSERT_EQ_I64(setenv("YEW_CLIPBOARD", value, 1), 0);
    yew_clip_reset();
}

static void pk_clip_done(PkClip *c)
{
    yew_clip_shutdown();
    YEW_ASSERT_EQ_I64(unlink(c->path), 0);
    if (c->saved != NULL)
        YEW_ASSERT_EQ_I64(setenv("YEW_CLIPBOARD", c->saved, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("YEW_CLIPBOARD"), 0);
    free(c->saved);
    yew_clip_reset();
}

static i64 pk_now_ms(void)
{
    struct timespec ts;

    YEW_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000L);
}

/* Flush a queued clipboard write the way a frame does, then wait for the
 * helper to finish, and compare what it wrote. */
static void pk_clip_holds(PkClip *c, const char *want)
{
    Bytebuf terminal;
    char got[64];
    FILE *fp;
    size_t n;
    u32 i;

    bytebuf_init(&terminal);
    yew_clip_after_render(&terminal, pk_now_ms());
    for (i = 0U; i < 5000U && yew_clip_busy(); i++) {
        int fd = yew_clip_write_fd();

        if (fd >= 0) {
            struct pollfd pfd = {fd, POLLOUT, 0};

            (void)poll(&pfd, 1U, 1);
        }
        yew_clip_pump(pk_now_ms());
    }
    YEW_ASSERT(!yew_clip_busy());
    bytebuf_free(&terminal);
    /* The helper may still be writing its file after yew lets go of
     * it: wait on the CONTENT, bounded, never on a bare sleep. */
    for (i = 0U; i < 2000U; i++) {
        struct timespec pause = {0, 1000000L};

        fp = fopen(c->path, "rb");
        YEW_ASSERT_NOT_NULL(fp);
        n = fread(got, 1U, sizeof(got), fp);
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
        if (n == strlen(want) && memcmp(got, want, n) == 0)
            break;
        (void)nanosleep(&pause, NULL);
    }
    YEW_ASSERT_EQ_U64(n, strlen(want));
    YEW_ASSERT_EQ_MEM(got, want, n);
}

static void pk_plus_holds(PkFix *f, const char *want)
{
    const RegVal *plus = yew_reg_get(&f->ed.regs, '+');

    YEW_ASSERT_NOT_NULL(plus);
    YEW_ASSERT_EQ_U64(plus->bytes.len, strlen(want));
    YEW_ASSERT_EQ_MEM(plus->bytes.data, want, plus->bytes.len);
}

/* §2: every Shift row extends, by grapheme, word and line; E stays E. */
void test_prompt_keys_shift_motions_extend_the_selection(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 11U, 12U);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_sel(&f, 8U, 12U);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_sel(&f, 2U, 12U);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_shift(&f, YEW_KEY_RIGHT, YEW_MOD_ALT, "ed.sel.extend.word_next");
    pk_sel(&f, 8U, 12U);
    pk_shift(&f, YEW_KEY_HOME, 0U, "ed.sel.extend.line_home");
    pk_sel(&f, 0U, 12U);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    pk_shift(&f, YEW_KEY_END, 0U, "ed.sel.extend.line_end");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_CTRL, "ed.sel.extend.line_home");
    pk_sel(&f, 0U, 12U);
    pk_shift(&f, YEW_KEY_RIGHT, YEW_MOD_CTRL, "ed.sel.extend.line_end");
    pk_nosel(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 12U);
    YEW_ASSERT(f.ed.cmdline.active);
    pk_text(&f, "e alpha beta");
    pk_free(&f);
}

/* §1: <left>/<right> (and C-b/C-f, the same commands) collapse to the
 * selection's start/end without moving further; with none they move. */
void test_prompt_keys_left_right_collapse_to_the_selection_edges(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, YEW_KEY_LEFT, 0U, "ed.move.char.prev");
    pk_nosel(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_sel(&f, 8U, 10U);
    pk_run(&f, YEW_KEY_LEFT, 0U, "ed.move.char.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, YEW_KEY_LEFT, 0U, "ed.move.char.prev");
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 7U, 8U);
    pk_run(&f, YEW_KEY_RIGHT, 0U, "ed.cmdline.ghost.accept");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 6U, 8U);
    pk_run(&f, (u32)'b', YEW_MOD_CTRL, "ed.move.char.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_sel(&f, 6U, 7U);
    pk_run(&f, (u32)'f', YEW_MOD_CTRL, "ed.cmdline.ghost.accept");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 7U);
    pk_nosel(&f);
    /* Nothing selected: an ordinary motion. */
    pk_run(&f, YEW_KEY_LEFT, 0U, "ed.move.char.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    pk_text(&f, "e alpha beta");
    pk_free(&f);
}

/* §1: every other motion collapses, then moves from the CARET. */
void test_prompt_keys_other_motions_collapse_then_move_from_the_caret(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, YEW_KEY_END, 0U, "ed.move.line.end");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 12U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_HOME, 0U, "ed.move.line.home_toggle");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, (u32)'f', YEW_MOD_ALT, "ed.cmdline.ghost.accept_word");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, (u32)'e', YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 12U);
    pk_nosel(&f);
    pk_text(&f, "e alpha beta");
    pk_free(&f);
}

/* §1: a printable key, a yank, A-., a register, C-q and a bracketed
 * paste each REPLACE the selection. */
void test_prompt_keys_typing_yank_and_paste_replace_the_selection(void)
{
    static const char *const own[] = {"e last.txt"};
    PkFix f;
    RegVal value;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_type(&f, "X");
    pk_text(&f, "e alpha X");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_nosel(&f);

    pk_prompt(&f, NULL, 0U, "e one two");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_type(&f, "three");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'y', YEW_MOD_CTRL, "ed.edit.kill.yank");
    pk_text(&f, "e one two");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 9U);
    pk_nosel(&f);

    pk_prompt(&f, own, 1U, "e a b");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'.', YEW_MOD_ALT, "ed.cmdline.last_arg");
    pk_text(&f, "e a last.txt");
    pk_nosel(&f);

    yew_regval_init(&value);
    bytebuf_append(&value.bytes, "reg", 3U);
    yew_reg_yank(&f.ed.regs, (u8)'a', &value);
    yew_regval_free(&value);
    pk_prompt(&f, NULL, 0U, "x yy");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_send(&f, (u32)'r', YEW_MOD_ALT);
    pk_send(&f, (u32)'a', 0U);
    pk_text(&f, "x reg");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_send(&f, (u32)'q', YEW_MOD_CTRL);
    pk_send(&f, (u32)'Q', 0U);
    pk_text(&f, "x reQ");
    pk_nosel(&f);

    pk_shift(&f, YEW_KEY_HOME, 0U, "ed.sel.extend.line_home");
    pk_paste(&f, "pasted");
    pk_text(&f, "pasted");
    pk_nosel(&f);
    pk_free(&f);
}

/* §1: <bs>, <del>, C-d and C-h delete the selection and nothing more. */
void test_prompt_keys_delete_keys_delete_only_the_selection(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e abcdef");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_BACKSPACE, 0U, "ed.edit.delete.grapheme_left");
    pk_text(&f, "e abcd");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_DELETE, 0U, "ed.edit.delete.grapheme");
    pk_text(&f, "e ab");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 4U);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'d', YEW_MOD_CTRL, "ed.edit.delete.grapheme");
    pk_text(&f, "e a");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'h', YEW_MOD_CTRL, "ed.edit.delete.grapheme_left");
    pk_text(&f, "e ");
    pk_nosel(&f);
    /* Nothing selected: one grapheme, as ever. */
    pk_run(&f, YEW_KEY_BACKSPACE, 0U, "ed.edit.delete.grapheme_left");
    pk_text(&f, "e");
    YEW_ASSERT(f.ed.cmdline.active);
    pk_free(&f);
}

/* §1: kills collapse and act from the caret -- they never kill the
 * selection (C-x is the cut); transpose and case likewise. */
void test_prompt_keys_kills_transpose_and_case_collapse_first(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e one two three");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'u', YEW_MOD_CTRL, "ed.edit.kill.to_home");
    pk_text(&f, "three");
    YEW_ASSERT_EQ_MEM(yew_yank_at(&f.ed.yank, 0U)->data, "e one two ", 10U);
    pk_nosel(&f);

    pk_prompt(&f, NULL, 0U, "e one two");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_text(&f, "e one wo");
    pk_nosel(&f);
    /* The blank before "wo" selected, the caret on its left: A-<bs>
     * takes the word before the CARET and leaves the blank. */
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_BACKSPACE, YEW_MOD_ALT, "ed.edit.kill.word_prev");
    pk_text(&f, "e  wo");
    pk_nosel(&f);
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_run(&f, (u32)'d', YEW_MOD_ALT, "ed.edit.kill.word_next");
    pk_text(&f, "ewo");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'k', YEW_MOD_CTRL, "ed.edit.kill.to_end");
    pk_text(&f, "");
    pk_nosel(&f);

    pk_prompt(&f, NULL, 0U, "ab");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'t', YEW_MOD_CTRL, "ed.edit.transpose.chars");
    pk_text(&f, "ba");
    pk_nosel(&f);
    pk_prompt(&f, NULL, 0U, "one two");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'u', YEW_MOD_ALT, "ed.edit.case.upper_word");
    pk_text(&f, "one TWO");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'t', YEW_MOD_ALT, "ed.edit.transpose.words");
    pk_text(&f, "TWO one");
    pk_nosel(&f);
    pk_free(&f);
}

/* §1: a ghost accept, a completion, history, Enter and cancel collapse;
 * Enter submits the WHOLE line. */
void test_prompt_keys_ghost_tab_history_enter_and_cancel_collapse(void)
{
    static const char *const own[] = {"!git status"};
    static const char *const older[] = {"e older"};
    PkFix f;
    OptVal v;

    pk_init(&f);
    pk_prompt(&f, own, 1U, "!git st");
    pk_shift(&f, YEW_KEY_HOME, 0U, "ed.sel.extend.line_home");
    pk_ghost(&f, NULL);
    /* <right> collapses to the END; the ghost is back but not taken. */
    pk_run(&f, YEW_KEY_RIGHT, 0U, "ed.cmdline.ghost.accept");
    pk_nosel(&f);
    pk_text(&f, "!git st");
    pk_ghost(&f, "atus");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'e', YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    pk_nosel(&f);
    pk_text(&f, "!git st");
    pk_run(&f, (u32)'e', YEW_MOD_CTRL, "ed.cmdline.ghost.accept_line");
    pk_text(&f, "!git status");

    pk_prompt(&f, NULL, 0U, "se");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_TAB, 0U, "ed.cmdline.complete_next");
    pk_nosel(&f);
    YEW_ASSERT(yew_textbuf_len(f.ed.cmdline.buf) >= 2U);
    /* Tab opened a menu C-g would close first; the next scene wants
     * a fresh prompt. */
    yew_cmdline_close(&f.ed, false);

    /* History walks by the typed stem, so the line is its prefix. */
    pk_prompt(&f, older, 1U, "e ");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_text(&f, "e older");
    pk_nosel(&f);

    pk_prompt(&f, NULL, 0U, "set shell.suggest_history all");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, YEW_KEY_ENTER, 0U, "ed.cmdline.accept");
    YEW_ASSERT(!f.ed.cmdline.active);
    YEW_ASSERT(yew_opt_get(&f.ed, NULL, NULL, "shell.suggest_history", 21U,
                           &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, 3U);
    YEW_ASSERT_EQ_MEM(v.as.str.s, "all", 3U);

    pk_prompt(&f, NULL, 0U, "e x");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    YEW_ASSERT(!f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    pk_free(&f);
}

/* §1 undo rows: undo and redo collapse; a typed replacement and a cut
 * are ONE undo step each. */
void test_prompt_keys_replacement_and_cut_are_one_undo_step(void)
{
    PkFix f;
    PkClip clip;

    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_type(&f, "X");
    pk_text(&f, "e alpha X");
    pk_run(&f, (u32)'_', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "e alpha beta");
    pk_nosel(&f);
    pk_run(&f, (u32)'/', YEW_MOD_ALT, "ed.edit.redo");
    pk_text(&f, "e alpha X");
    pk_nosel(&f);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    pk_text(&f, "e alpha");
    pk_run(&f, (u32)'z', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "e alpha X");
    pk_nosel(&f);
    pk_free(&f);
    pk_clip_done(&clip);
}

/* §2: C-c copies a selection and stays; without one it is C-g. */
void test_prompt_keys_ctrl_c_copies_or_cancels(void)
{
    PkFix f;
    PkClip clip;

    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.cmdline.copy_or_cancel");
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_E);
    pk_nosel(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_text(&f, "e alpha beta");
    pk_plus_holds(&f, "beta");
    pk_clip_holds(&clip, "beta");
    /* Nothing selected now: exactly C-g. */
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.cmdline.copy_or_cancel");
    YEW_ASSERT(!f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);
    pk_free(&f);
    pk_clip_done(&clip);
}

/* §2: C-x cuts a selection to the clipboard; with none, nothing at all. */
void test_prompt_keys_ctrl_x_cuts_or_does_nothing(void)
{
    PkFix f;
    PkClip clip;

    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e alpha beta");
    yew_msg_clear(&f.ed);
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    pk_text(&f, "e alpha beta");
    YEW_ASSERT(!f.ed.msg.active);
    YEW_ASSERT(!yew_clip_pending());
    YEW_ASSERT(f.ed.cmdline.active);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    pk_text(&f, "e alpha ");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_nosel(&f);
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_E);
    pk_plus_holds(&f, "beta");
    pk_clip_holds(&clip, "beta");
    pk_free(&f);
    pk_clip_done(&clip);
}

/* §2: C-v replaces a selection with the clipboard, folded to one line,
 * as one undo step. */
void test_prompt_keys_ctrl_v_replaces_the_selection(void)
{
    PkFix f;
    PkClip clip;

    pk_clip_open(&clip, "git\nlog");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!echo hi");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'v', YEW_MOD_CTRL, "ed.clip.paste");
    pk_text(&f, "!echo git log");
    pk_nosel(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 13U);
    pk_run(&f, (u32)'z', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "!echo hi");
    pk_free(&f);
    pk_clip_done(&clip);

    /* An EMPTY clipboard refuses the paste, and the selection it would
     * have replaced is still there: the delete rolls back with it. */
    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!echo hi");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    yew_ed_handle_key(&f.ed, pk_key((u32)'v', YEW_MOD_CTRL), 0);
    YEW_ASSERT(f.ed.last_status != YEW_CMD_OK);
    pk_text(&f, "!echo hi");
    YEW_ASSERT(f.ed.cmdline.active);
    pk_free(&f);
    pk_clip_done(&clip);
}

/* Invariant 2: a Shift+motion takes a CJK wide cluster or a combining
 * sequence whole, and typing replaces it whole. */
void test_prompt_keys_selection_edges_respect_graphemes(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e ");
    pk_paste(&f, "\xe6\x97\xa5\xe6\x9c\xac");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 5U, 8U);
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 2U, 8U);
    pk_shift(&f, YEW_KEY_RIGHT, 0U, "ed.sel.extend.right");
    pk_sel(&f, 5U, 8U);
    pk_type(&f, "Z");
    pk_text(&f, "e \xe6\x97\xa5Z");

    pk_prompt(&f, NULL, 0U, "e x");
    pk_paste(&f, "e\xcc\x81");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 3U, 6U);
    pk_run(&f, YEW_KEY_BACKSPACE, 0U, "ed.edit.delete.grapheme_left");
    pk_text(&f, "e x");
    pk_free(&f);
}

/* The pitfall: a selection is never text -- not in yew_cmdline_text(),
 * the history draft, the hint the parse point produces, or the ghost. */
void test_prompt_keys_selection_is_never_text(void)
{
    static const char *const own[] = {"!git status"};
    PkFix f;
    char hint[sizeof(f.ed.cmdline.hint)];

    pk_init(&f);
    pk_prompt(&f, own, 1U, "!git st");
    pk_ghost(&f, "atus");
    (void)memcpy(hint, f.ed.cmdline.hint, sizeof(hint));
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_shift(&f, YEW_KEY_END, 0U, "ed.sel.extend.line_end");
    pk_sel(&f, 0U, 7U);
    pk_text(&f, "!git st");
    YEW_ASSERT_EQ_MEM(f.ed.cmdline.hint, hint, sizeof(hint));
    pk_ghost(&f, "atus");
    /* Sprint 57.30: the history draft and term are taken when a walk
     * begins, from the line's TEXT -- the selection is not in them. */
    YEW_ASSERT(!f.ed.cmdline.walk.on);
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    YEW_ASSERT(f.ed.cmdline.walk.on);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.walk.draft, "!git st");
    YEW_ASSERT_EQ_STR(f.ed.cmdline.walk.term, "git st");
    pk_text(&f, "!git status");
    pk_free(&f);
}

static void pk_yank_snapshot(const YewYankStack *y, YewYankStack *copy,
                             Bytebuf *bytes)
{
    u32 k;

    (void)memcpy(copy, y, sizeof(*copy));
    bytebuf_init(bytes);
    for (k = 0U; k < y->len; k++) {
        const Bytebuf *e = yew_yank_at(y, k);

        bytebuf_append(bytes, e->data, e->len);
        bytebuf_push_u8(bytes, 0U);
    }
}

/* §4 both ways: copy and cut leave the yank stack byte-identical; kills
 * over a selection leave the clipboard and `+` untouched. */
void test_prompt_keys_clipboard_and_yank_stack_stay_apart(void)
{
    PkFix f;
    PkClip clip;
    YewYankStack before;
    YewYankStack after;
    Bytebuf before_bytes;
    Bytebuf after_bytes;

    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e one two three");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_type(&f, "four");
    pk_yank_snapshot(&f.ed.yank, &before, &before_bytes);
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.cmdline.copy_or_cancel");
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    pk_text(&f, "e one four");
    pk_yank_snapshot(&f.ed.yank, &after, &after_bytes);
    YEW_ASSERT_EQ_MEM(&before, &after, sizeof(before));
    YEW_ASSERT_EQ_U64(before_bytes.len, after_bytes.len);
    YEW_ASSERT_EQ_MEM(before_bytes.data, after_bytes.data,
                      before_bytes.len);
    bytebuf_free(&before_bytes);
    bytebuf_free(&after_bytes);
    pk_plus_holds(&f, "two ");
    pk_clip_holds(&clip, "two ");

    /* And back: kills with a selection present write neither. */
    f.ed.regs.clipboard_sync = YEW_CLIP_SYNC_ALL;
    pk_shift(&f, YEW_KEY_LEFT, YEW_MOD_ALT, "ed.sel.extend.word_prev");
    pk_run(&f, (u32)'w', YEW_MOD_CTRL, "ed.edit.kill.ws_word_prev");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_run(&f, (u32)'k', YEW_MOD_CTRL, "ed.edit.kill.to_end");
    YEW_ASSERT(!yew_clip_pending());
    pk_plus_holds(&f, "two ");
    pk_free(&f);
    pk_clip_done(&clip);
}

/*
 * Insert mode: with nothing selected C-c and C-x change nothing -- and an
 * anchor away from the caret in I is not a selection (57.13), so it is
 * not copied either.  Shift+arrows from Insert select (in H), and there
 * C-c reaches the clipboard.
 */
void test_prompt_keys_insert_mode_ctrl_c_and_ctrl_x(void)
{
    PkFix f;
    PkClip clip;
    Cursor *c;

    pk_clip_open(&clip, "");
    pk_init(&f);
    pk_send(&f, (u32)'i', 0U);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    pk_type(&f, "hello");
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.clip.copy");
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    c = yew_ed_cursor(&f.ed);
    c->anchor = BYTEOFF(0U);
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.clip.copy");
    pk_run(&f, (u32)'x', YEW_MOD_CTRL, "ed.clip.cut");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(f.ed.buffer.tb), 5U);
    YEW_ASSERT(!yew_clip_pending());
    YEW_ASSERT_EQ_U64(f.ed.regs.system.bytes.len, 0U);
    c = yew_ed_cursor(&f.ed);
    c->anchor = c->pos;
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_H);
    pk_run(&f, (u32)'c', YEW_MOD_CTRL, "ed.clip.copy");
    pk_plus_holds(&f, "lo");
    pk_clip_holds(&clip, "lo");
    pk_free(&f);
    pk_clip_done(&clip);
}

/* ------------------------------------------------------------ drawing */

static bool pk_color_eq(YewColor a, YewColor b)
{
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* Does cell `x` of the prompt row carry every field of the selection
 * style? */
static bool pk_styled(PkFix *f, u16 x)
{
    Cell sel;
    u8 fields = yew_draw_sel_style(&f->ed, &sel);
    const Cell *cell = &f->ed.grid.back[x];

    YEW_ASSERT(fields != 0U);
    if ((fields & YEW_OVERLAY_BG) != 0U && !pk_color_eq(cell->bg, sel.bg))
        return false;
    if ((fields & YEW_OVERLAY_FG) != 0U && !pk_color_eq(cell->fg, sel.fg))
        return false;
    if ((fields & YEW_OVERLAY_ATTRS) != 0U &&
        (cell->attrs & sel.attrs) != sel.attrs)
        return false;
    return true;
}

static void pk_draw(PkFix *f, u16 cols)
{
    yew_cmdline_draw(&f->ed, (Rect){0U, 0U, cols, 1U});
}

/*
 * §3: the prompt paints its selection in the document's style, across
 * the horizontal scroll at either edge, whole wide clusters, never the
 * `:` or the ghost -- and identical state draws identical cells.
 */
void test_prompt_keys_selection_draws_in_the_selection_style(void)
{
    static const char *const own[] = {"!git status"};
    PkFix f;
    Cell first[12];
    u16 x;

    pk_init(&f);
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 1U, 40U));
    /* 18 bytes in 11 text cells: the start of the selection is off the
     * left edge, the caret cell after the text is not selected. */
    pk_prompt(&f, NULL, 0U, "e 0123456789abcdef");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_shift(&f, YEW_KEY_END, 0U, "ed.sel.extend.line_end");
    pk_draw(&f, 12U);
    YEW_ASSERT(!pk_styled(&f, 0U));
    for (x = 1U; x <= 10U; x++)
        YEW_ASSERT(pk_styled(&f, x));
    YEW_ASSERT(!pk_styled(&f, 11U));
    YEW_ASSERT_EQ_U64(f.ed.grid.back[1].utf8[0], (u8)'6');
    (void)memcpy(first, f.ed.grid.back, sizeof(first));
    pk_draw(&f, 12U);
    YEW_ASSERT_EQ_MEM(first, f.ed.grid.back, sizeof(first));
    /* Selected from the end back to the start: the caret is home and
     * the selection runs off the RIGHT edge. */
    pk_run(&f, YEW_KEY_END, 0U, "ed.move.line.end");
    pk_shift(&f, YEW_KEY_HOME, 0U, "ed.sel.extend.line_home");
    pk_sel(&f, 0U, 18U);
    pk_draw(&f, 12U);
    YEW_ASSERT_EQ_U64(f.ed.grid.back[1].utf8[0], (u8)'e');
    for (x = 1U; x <= 11U; x++)
        YEW_ASSERT(pk_styled(&f, x));
    YEW_ASSERT(!pk_styled(&f, 0U));

    /* A wide cluster: both of its cells, and nothing of its neighbour. */
    pk_prompt(&f, NULL, 0U, "e ");
    pk_paste(&f, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_draw(&f, 12U);
    YEW_ASSERT(!pk_styled(&f, 5U));
    YEW_ASSERT(!pk_styled(&f, 6U));
    YEW_ASSERT(pk_styled(&f, 7U));
    YEW_ASSERT(pk_styled(&f, 8U));
    YEW_ASSERT(!pk_styled(&f, 9U));

    /* The ghost trails the selected text and is never selected. */
    pk_prompt(&f, own, 1U, "!git st");
    pk_run(&f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    pk_shift(&f, YEW_KEY_END, 0U, "ed.sel.extend.line_end");
    pk_ghost(&f, "atus");
    pk_draw(&f, 40U);
    for (x = 1U; x <= 7U; x++)
        YEW_ASSERT(pk_styled(&f, x));
    YEW_ASSERT_EQ_U64(f.ed.grid.back[8].utf8[0], (u8)'a');
    for (x = 8U; x <= 11U; x++)
        YEW_ASSERT(!pk_styled(&f, x));
    yew_grid_free(&f.ed.grid);
    pk_free(&f);
}

/* ================================================================== */
/* Sprint 57.30: prompt history and the completion table               */
/* ================================================================== */

/* The two chords a scene drives Up and Down with: the arrows, or C-p
 * and C-n, which §3 binds to the same commands. */
typedef struct PkArrows {
    u32 up;
    u16 up_mods;
    u32 down;
    u16 down_mods;
} PkArrows;

static const PkArrows pk_arrow_keys = {YEW_KEY_UP, 0U, YEW_KEY_DOWN, 0U};
static const PkArrows pk_ctrl_keys = {(u32)'p', YEW_MOD_CTRL, (u32)'n',
                                      YEW_MOD_CTRL};

static void pk_up(PkFix *f, const PkArrows *k)
{
    pk_run(f, k->up, k->up_mods, "ed.cmdline.up");
}

static void pk_down(PkFix *f, const PkArrows *k)
{
    pk_run(f, k->down, k->down_mods, "ed.cmdline.down");
}

static bool pk_in_table(PkFix *f)
{
    return yew_menu_focused(&f->ed.cmdline.menu);
}

/* Tab until the table has focus -- the first Tab may only extend the
 * word by its common prefix.  Returns the line as it was just before
 * the Tab that entered: the text the table was entered from. */
static char *pk_enter_table(PkFix *f)
{
    Bytebuf b;
    char *before = NULL;
    int i;

    for (i = 0; i < 3 && !pk_in_table(f); i++) {
        yew_xfree(before);
        bytebuf_init(&b);
        yew_cmdline_text(&f->ed, &b);
        bytebuf_push_u8(&b, 0U);
        before = (char *)b.data;
        pk_run(f, YEW_KEY_TAB, 0U, "ed.cmdline.complete_next");
    }
    YEW_ASSERT(pk_in_table(f));
    return before;
}

static const char *pk_row(PkFix *f, u32 i)
{
    YEW_ASSERT(i < f->ed.cmdline.menu.items.len);
    return f->ed.cmdline.menu.items.data[i].text;
}

/* Every row of §1's table, driven by `k`. */
static void pk_table_scene(const PkArrows *k)
{
    static const char *const own[] = {"file.zzz", "set wrap"};
    PkFix f;
    char *typed;
    char again[160];
    u32 last;
    bool scrolled = false;

    pk_init(&f);
    pk_prompt(&f, own, 2U, "fil");
    YEW_ASSERT(f.ed.cmdline.menu.items.len > 5U);

    /* NOT in the table, the live table open: Up is history (the
     * dogfooding bug), Down past the newest is the draft again. */
    YEW_ASSERT(!pk_in_table(&f));
    pk_up(&f, k);
    pk_text(&f, "file.zzz");
    YEW_ASSERT(!pk_in_table(&f));
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 0U);
    pk_down(&f, k);
    pk_text(&f, "fil");
    YEW_ASSERT(f.ed.cmdline.menu.items.len > 5U);
    YEW_ASSERT(!pk_in_table(&f));

    /* Tab enters; in the table, Down moves a row and writes it. */
    typed = pk_enter_table(&f);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_text(&f, pk_row(&f, 0U));
    pk_down(&f, k);
    YEW_ASSERT(pk_in_table(&f));
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    pk_text(&f, pk_row(&f, 1U));
    /* Up, not on the top row: a row up. */
    pk_up(&f, k);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_text(&f, pk_row(&f, 0U));

    /* Down to the true last row; off the bottom VISIBLE row, with rows
     * hidden below, the window scrolls one row at a time. */
    last = (u32)f.ed.cmdline.menu.items.len - 1U;
    while (f.ed.cmdline.menu.sel < (i32)last) {
        u32 top = f.ed.cmdline.menu.top;
        i32 sel = f.ed.cmdline.menu.sel;

        pk_down(&f, k);
        YEW_ASSERT(pk_in_table(&f));
        YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, sel + 1);
        YEW_ASSERT(f.ed.cmdline.menu.top == top ||
                   f.ed.cmdline.menu.top == top + 1U);
        if (f.ed.cmdline.menu.top != top)
            scrolled = true;
    }
    YEW_ASSERT(scrolled);
    /* Down on the true last row: out of the table, the candidate kept
     * in the line, the table still OPEN but unfocused. */
    pk_down(&f, k);
    YEW_ASSERT(!pk_in_table(&f));
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, (i32)last);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, (u64)last + 1U);
    pk_text(&f, pk_row(&f, last));
    /* Tab enters it again and the rows move again. */
    pk_run(&f, YEW_KEY_TAB, 0U, "ed.cmdline.complete_next");
    YEW_ASSERT(pk_in_table(&f));
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_down(&f, k);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    while (f.ed.cmdline.menu.sel < (i32)last)
        pk_down(&f, k);
    pk_down(&f, k);
    YEW_ASSERT(!pk_in_table(&f));
    /* From the line, Up is history again -- searched with the line. */
    {
        int n = snprintf(again, sizeof(again), "%s again", pk_row(&f, last));

        YEW_ASSERT(n > 0 && (size_t)n < sizeof(again));
    }
    yew_hist_add(f.ed.cmdline.history, again);
    pk_up(&f, k);
    pk_text(&f, again);
    YEW_ASSERT(!pk_in_table(&f));
    pk_down(&f, k);
    YEW_ASSERT(!pk_in_table(&f));

    /* Up on the TOP row: out of the table, the table closed, history
     * searched with what was TYPED -- `file.zzz` holds that, and no
     * candidate's name. */
    pk_prompt(&f, own, 2U, "fil");
    yew_xfree(typed);
    typed = pk_enter_table(&f);
    YEW_ASSERT(strcmp(typed, pk_row(&f, 0U)) != 0);
    pk_down(&f, k);
    pk_up(&f, k);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_up(&f, k);
    YEW_ASSERT(!pk_in_table(&f));
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 0U);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.walk.term, typed);
    pk_text(&f, "file.zzz");
    pk_down(&f, k);
    pk_text(&f, typed);
    YEW_ASSERT(!pk_in_table(&f));
    /* <pgdn>/<pgup> page inside the table. */
    (void)pk_enter_table(&f);
    pk_run(&f, YEW_KEY_PAGE_DOWN, 0U, "ed.cmdline.menu.page_next");
    YEW_ASSERT(pk_in_table(&f));
    YEW_ASSERT(f.ed.cmdline.menu.sel >= 5);
    pk_text(&f, pk_row(&f, (u32)f.ed.cmdline.menu.sel));
    pk_run(&f, YEW_KEY_PAGE_UP, 0U, "ed.cmdline.menu.page_prev");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    yew_xfree(typed);
    pk_free(&f);
}

/* §1, every row, on the arrows. */
void test_prompt_keys_table_rows_on_the_arrows(void)
{
    pk_table_scene(&pk_arrow_keys);
}

/* §3: C-p and C-n are the arrows exactly, edge rules included. */
void test_prompt_keys_table_rows_on_ctrl_p_and_ctrl_n(void)
{
    pk_table_scene(&pk_ctrl_keys);
}

/* §2 on a prompt's own history: substring, newest first, a frozen term,
 * Down past the newest back at the draft, an edit ending the walk. */
static void pk_history_scene(const PkArrows *k)
{
    static const char *const own[] = {"e notes.txt", "set wrap",
                                      "echo wrapped", "set nowrap"};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, 4U, "wrap");
    pk_up(&f, k);
    pk_text(&f, "set nowrap");
    pk_up(&f, k);
    pk_text(&f, "echo wrapped");
    /* Frozen: the term is still what was typed, not the entry shown. */
    YEW_ASSERT_EQ_STR(f.ed.cmdline.walk.term, "wrap");
    pk_up(&f, k);
    pk_text(&f, "set wrap");
    /* Past the oldest match it stays put. */
    pk_up(&f, k);
    pk_text(&f, "set wrap");
    pk_down(&f, k);
    pk_text(&f, "echo wrapped");
    pk_down(&f, k);
    pk_text(&f, "set nowrap");
    pk_down(&f, k);
    pk_text(&f, "wrap");
    pk_down(&f, k);
    pk_text(&f, "wrap");

    /* An edit ends the walk; the next Up searches the new text. */
    pk_up(&f, k);
    pk_up(&f, k);
    pk_text(&f, "echo wrapped");
    pk_type(&f, "x");
    YEW_ASSERT(!f.ed.cmdline.walk.on);
    pk_up(&f, k);
    pk_text(&f, "echo wrappedx");
    YEW_ASSERT_EQ_STR(f.ed.cmdline.walk.term, "echo wrappedx");

    /* Case-sensitive. */
    pk_prompt(&f, NULL, 0U, "WRAP");
    pk_up(&f, k);
    pk_text(&f, "WRAP");

    /* An empty line walks all of it. */
    pk_prompt(&f, NULL, 0U, "");
    pk_up(&f, k);
    pk_text(&f, "set nowrap");
    pk_up(&f, k);
    pk_text(&f, "echo wrapped");
    pk_up(&f, k);
    pk_text(&f, "set wrap");
    pk_up(&f, k);
    pk_text(&f, "e notes.txt");
    pk_free(&f);
}

void test_prompt_keys_history_is_a_substring_walk(void)
{
    pk_history_scene(&pk_arrow_keys);
    pk_history_scene(&pk_ctrl_keys);
}

/* The `/` prompt walks the search history the same way. */
void test_prompt_keys_search_prompt_walks_its_own_history(void)
{
    PkFix f;

    pk_init(&f);
    pk_send(&f, (u32)'/', 0U);
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_SEARCH_F);
    yew_hist_add(f.ed.cmdline.history, "alpha beta");
    yew_hist_add(f.ed.cmdline.history, "gamma");
    pk_type(&f, "bet");
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_text(&f, "alpha beta");
    YEW_ASSERT(!f.ed.cmdline.walk_bang);
    pk_free(&f);
}

static bool pk_match_styled(PkFix *f, u16 x)
{
    Cell m;
    u8 fields = yew_draw_search_style(&f->ed, false, &m);
    const Cell *cell = &f->ed.grid.back[x];

    YEW_ASSERT(fields != 0U);
    if ((fields & YEW_OVERLAY_BG) != 0U && !pk_color_eq(cell->bg, m.bg))
        return false;
    if ((fields & YEW_OVERLAY_ATTRS) != 0U &&
        (cell->attrs & m.attrs) != m.attrs)
        return false;
    return true;
}

/* §2: the matched part is highlighted in the `/` match style -- STATE,
 * never text. */
void test_prompt_keys_history_highlight_is_state(void)
{
    static const char *const own[] = {"echo wrapped", "set nowrap"};
    PkFix f;
    Span hit = {0U, 0U};
    u16 x;

    pk_init(&f);
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 1U, 40U));
    pk_prompt(&f, own, 2U, "wrap");
    YEW_ASSERT(!yew_cmdline_hist_match(&f.ed, NULL));
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_text(&f, "echo wrapped");
    YEW_ASSERT(yew_cmdline_hist_match(&f.ed, &hit));
    YEW_ASSERT_EQ_U64(hit.lo, 5U);
    YEW_ASSERT_EQ_U64(hit.hi, 9U);
    pk_draw(&f, 40U);
    /* `:` is cell 0, so bytes 5..9 are cells 6..9. */
    for (x = 0U; x <= 5U; x++)
        YEW_ASSERT(!pk_match_styled(&f, x));
    for (x = 6U; x <= 9U; x++)
        YEW_ASSERT(pk_match_styled(&f, x));
    for (x = 10U; x <= 13U; x++)
        YEW_ASSERT(!pk_match_styled(&f, x));
    /* No ghost trails a walked entry. */
    pk_ghost(&f, NULL);
    /* Back at the draft, and after an edit, nothing is highlighted. */
    pk_run(&f, YEW_KEY_DOWN, 0U, "ed.cmdline.down");
    pk_run(&f, YEW_KEY_DOWN, 0U, "ed.cmdline.down");
    pk_text(&f, "wrap");
    YEW_ASSERT(!yew_cmdline_hist_match(&f.ed, NULL));
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    YEW_ASSERT(yew_cmdline_hist_match(&f.ed, NULL));
    pk_type(&f, "!");
    YEW_ASSERT(!yew_cmdline_hist_match(&f.ed, NULL));
    pk_text(&f, "set nowrap!");
    /* An empty term walks everything and highlights nothing. */
    pk_prompt(&f, NULL, 0U, "");
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_text(&f, "set nowrap");
    YEW_ASSERT(!yew_cmdline_hist_match(&f.ed, NULL));
    yew_grid_free(&f.ed.grid);
    pk_free(&f);
}

static void pk_rows(PkFix *f, const char *const *want, size_t n)
{
    size_t i;

    YEW_ASSERT_EQ_U64(f->ed.cmdline.menu.items.len, n);
    for (i = 0U; i < n && i < f->ed.cmdline.menu.items.len; i++)
        YEW_ASSERT_EQ_STR(f->ed.cmdline.menu.items.data[i].text, want[i]);
}

/* Does grid row `row` hold `text` anywhere (ASCII cells)? */
static bool pk_row_has(PkFix *f, u16 row, const char *text)
{
    char line[256];
    u16 x;
    u16 cols = f->ed.grid.cols;

    if (cols >= sizeof(line))
        cols = (u16)(sizeof(line) - 1U);
    for (x = 0U; x < cols; x++) {
        u8 c = f->ed.grid.back[(size_t)row * f->ed.grid.cols + x].utf8[0];

        line[x] = c >= 0x20U && c < 0x7fU ? (char)c : '.';
    }
    line[cols] = '\0';
    return strstr(line, text) != NULL;
}

/* §4: C-r lists matching history newest first, refilters as the line is
 * typed, moves older on C-r, and Enter fills the line WITHOUT running
 * it; the second Enter runs it. */
void test_prompt_keys_ctrl_r_searches_history(void)
{
    static const char *const own[] = {
        "set shell.suggest_history all", "e notes.txt",
        "set shell.suggest_history yew", "echo hi"};
    static const char *const all[] = {
        "echo hi", "set shell.suggest_history yew", "e notes.txt",
        "set shell.suggest_history all"};
    static const char *const sugg[] = {"set shell.suggest_history yew",
                                       "set shell.suggest_history all"};
    PkFix f;
    OptVal v;
    u16 row;
    bool footer = false;

    pk_init(&f);
    pk_prompt(&f, own, 4U, "");
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT(f.ed.cmdline.hsearch);
    pk_rows(&f, all, 4U);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.menu.where, "history search");

    /* Typing edits the line and refilters, the match highlighted. */
    pk_type(&f, "suggest");
    pk_text(&f, "suggest");
    YEW_ASSERT(f.ed.cmdline.hsearch);
    pk_rows(&f, sugg, 2U);
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.data[0].m.n_pos, 7U);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.data[0].m.pos[0], 10U);
    /* The pager's footer names the mode. */
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 8U, 60U));
    yew_cmdline_draw(&f.ed, (Rect){0U, 7U, 60U, 1U});
    for (row = 0U; row < 7U; row++)
        footer = footer || pk_row_has(&f, row, "history search 1/2");
    YEW_ASSERT(footer);
    yew_grid_free(&f.ed.grid);

    /* C-r again: the next OLDER match; past the oldest it stays. */
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    /* The arrows, C-p/C-n and the page keys move between rows; the line
     * does not change while they do. */
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_run(&f, YEW_KEY_DOWN, 0U, "ed.cmdline.down");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    pk_run(&f, (u32)'p', YEW_MOD_CTRL, "ed.cmdline.up");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_run(&f, (u32)'n', YEW_MOD_CTRL, "ed.cmdline.down");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    pk_run(&f, YEW_KEY_PAGE_UP, 0U, "ed.cmdline.menu.page_prev");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 0);
    pk_run(&f, YEW_KEY_PAGE_DOWN, 0U, "ed.cmdline.menu.page_next");
    YEW_ASSERT_EQ_I64(f.ed.cmdline.menu.sel, 1);
    pk_text(&f, "suggest");

    /* Enter fills the line and closes the list; it does not run. */
    pk_run(&f, YEW_KEY_ENTER, 0U, "ed.cmdline.accept");
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT(!f.ed.cmdline.hsearch);
    pk_text(&f, "set shell.suggest_history all");
    YEW_ASSERT(yew_opt_get(&f.ed, NULL, NULL, "shell.suggest_history", 21U,
                           &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, 3U);
    YEW_ASSERT_EQ_MEM(v.as.str.s, "yew", 3U);
    /* A second Enter does. */
    pk_run(&f, YEW_KEY_ENTER, 0U, "ed.cmdline.accept");
    YEW_ASSERT(!f.ed.cmdline.active);
    YEW_ASSERT(yew_opt_get(&f.ed, NULL, NULL, "shell.suggest_history", 21U,
                           &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, 3U);
    YEW_ASSERT_EQ_MEM(v.as.str.s, "all", 3U);
    pk_free(&f);
}

/* §4: Esc and C-g close the list and restore the line as C-r found it;
 * no match says so; Tab leaves the search for completion. */
void test_prompt_keys_ctrl_r_escape_restores_the_line(void)
{
    static const char *const own[] = {"echo one", "echo two"};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, 2U, "ec");
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 2U);
    pk_type(&f, "ho t");
    pk_text(&f, "echo t");
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 1U);
    pk_run(&f, YEW_KEY_ESCAPE, 0U, "ed.cmdline.cancel");
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT(!f.ed.cmdline.hsearch);
    pk_text(&f, "ec");

    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    pk_type(&f, "zzz");
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 0U);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.hint, "history search: no match");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT(!f.ed.cmdline.hsearch);
    pk_text(&f, "ec");

    /* Tab leaves C-r's rows for completion's, keeping the line. */
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    pk_run(&f, YEW_KEY_TAB, 0U, "ed.cmdline.complete_next");
    YEW_ASSERT(!f.ed.cmdline.hsearch);
    YEW_ASSERT(strcmp(f.ed.cmdline.menu.where, "history search") != 0);
    pk_free(&f);
}

/* §4 on a bang line: the snapshot, the row after the typed `!`. */
void test_prompt_keys_ctrl_r_on_a_bang_line(void)
{
    static const char *const own[] = {"!make all", "!git stage -p"};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, 2U, "r !a");
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 2U);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.menu.items.data[0].text, "git stage -p");
    pk_run(&f, (u32)'r', YEW_MOD_CTRL, "ed.cmdline.hist_search");
    pk_run(&f, YEW_KEY_ENTER, 0U, "ed.cmdline.accept");
    YEW_ASSERT(f.ed.cmdline.active);
    pk_text(&f, "r !make all");
    pk_free(&f);
}

/* §5: A-<up> replaces JUST the token under the caret with the next
 * older distinct history token holding it; A-<down> walks back, the
 * original past the newest; anything else ends the walk. */
void test_prompt_keys_token_search_replaces_only_the_token(void)
{
    static const char *const own[] = {"e src/main.c", "e src/util.c",
                                      "e docs/main.md", "e src/main.c"};
    PkFix f;

    pk_init(&f);
    /* History, newest first: e src/main.c, e docs/main.md, e src/util.c. */
    pk_prompt(&f, own, 4U, "e main x");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 2U);
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "e src/main.c x");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 12U);
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "e docs/main.md x");
    /* `src/main.c` again is not a new answer; nothing older holds it. */
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "e docs/main.md x");
    pk_run(&f, YEW_KEY_DOWN, YEW_MOD_ALT, "ed.cmdline.token_next");
    pk_text(&f, "e src/main.c x");
    pk_run(&f, YEW_KEY_DOWN, YEW_MOD_ALT, "ed.cmdline.token_next");
    pk_text(&f, "e main x");
    pk_run(&f, YEW_KEY_DOWN, YEW_MOD_ALT, "ed.cmdline.token_next");
    pk_text(&f, "e main x");

    /* Anything between two presses ends the walk: the next begins on
     * the token then under the caret. */
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "e src/main.c x");
    pk_run(&f, YEW_KEY_LEFT, 0U, "ed.move.char.prev");
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    YEW_ASSERT_EQ_STR(f.ed.cmdline.tok_term, "src/main.c");
    /* Distinct: `src/main.c` itself is the term, so the next token that
     * holds it is from an older entry -- there is none. */
    pk_text(&f, "e src/main.c x");

    /* Words of every entry, the caret's word only: `util` in the middle
     * of a line. */
    pk_prompt(&f, own, 4U, "x util y");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    pk_run(&f, (u32)'b', YEW_MOD_ALT, "ed.move.word.prev");
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "x src/util.c y");
    pk_free(&f);
}

/* §5 on a bang line: the shell word under the caret, and an entry's
 * RAW shell words, quotes as typed. */
void test_prompt_keys_token_search_on_a_bang_line(void)
{
    static const char *const own[] = {"!cp a \"my file\" b",
                                      "!grep -n 'my pat' src"};
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, own, 2U, "!cat my");
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    YEW_ASSERT(f.ed.cmdline.tok_bang);
    pk_text(&f, "!cat 'my pat'");
    pk_run(&f, YEW_KEY_UP, YEW_MOD_ALT, "ed.cmdline.token_prev");
    pk_text(&f, "!cat \"my file\"");
    pk_run(&f, YEW_KEY_DOWN, YEW_MOD_ALT, "ed.cmdline.token_next");
    pk_run(&f, YEW_KEY_DOWN, YEW_MOD_ALT, "ed.cmdline.token_next");
    pk_text(&f, "!cat my");
    pk_free(&f);
}

/* ----------------------------------------------------- fish's extras */

static void pk_sudo(PkFix *f)
{
    pk_run(f, (u32)'s', YEW_MOD_ALT, "ed.cmdline.toggle_sudo");
}

/* Moves the caret to byte `at` with real keys: C-a, then C-f. */
static void pk_caret_to(PkFix *f, u64 at)
{
    u64 i;

    pk_run(f, (u32)'a', YEW_MOD_CTRL, "ed.move.line.home");
    for (i = 0U; i < at; i++)
        pk_send(f, (u32)'f', YEW_MOD_CTRL);
    YEW_ASSERT_EQ_U64(pk_caret(f), at);
}

/* Sprint 57.31 §1: every row of the table, on every bang form. */
void test_prompt_keys_alt_s_toggles_sudo(void)
{
    static const char *const own[] = {"!make test", "e foo.c"};
    static const char *const own_sudo[] = {"!sudo apt update"};
    PkFix f;

    pk_init(&f);
    /* Otherwise, non-empty: `sudo ` goes in front. */
    pk_prompt(&f, NULL, 0U, "!make");
    pk_sudo(&f);
    pk_text(&f, "!sudo make");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 10U);
    /* Starts with `sudo `: removed. */
    pk_sudo(&f);
    pk_text(&f, "!make");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 5U);
    /* `doas ` too, after leading blanks, which stay. */
    pk_prompt(&f, NULL, 0U, "!  doas rm x");
    pk_sudo(&f);
    pk_text(&f, "!  rm x");
    pk_sudo(&f);
    pk_text(&f, "!  sudo rm x");
    /* A lone `sudo` is the prefix too. */
    pk_prompt(&f, NULL, 0U, "!sudo");
    pk_sudo(&f);
    pk_text(&f, "!");
    /* `sudoedit` is a command, not the prefix. */
    pk_prompt(&f, NULL, 0U, "!sudoedit x");
    pk_sudo(&f);
    pk_text(&f, "!sudo sudoedit x");
    /* The other bang spellings. */
    pk_prompt(&f, NULL, 0U, "r !ls");
    pk_sudo(&f);
    pk_text(&f, "r !sudo ls");
    pk_prompt(&f, NULL, 0U, "%!sort");
    pk_sudo(&f);
    pk_text(&f, "%!sudo sort");
    pk_prompt(&f, NULL, 0U, "!!top");
    pk_sudo(&f);
    pk_text(&f, "!!sudo top");

    /* Empty: `sudo ` and the newest bang entry's body (fish), caret at
     * the end.  Blanks alone are empty too. */
    pk_prompt(&f, own, YEW_ARRAY_LEN(own), "!");
    pk_sudo(&f);
    pk_text(&f, "!sudo make test");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 15U);
    pk_prompt(&f, own, YEW_ARRAY_LEN(own), "! ");
    pk_sudo(&f);
    pk_text(&f, "! sudo make test");
    /* An entry that already starts with the prefix goes in as it is. */
    pk_prompt(&f, own_sudo, 1U, "!");
    pk_sudo(&f);
    pk_text(&f, "!sudo apt update");
    /* No history at all: just the prefix. */
    pk_free(&f);
    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!");
    pk_sudo(&f);
    pk_text(&f, "!sudo ");
    pk_free(&f);
}

/* §1: the caret stays on the text it was on. */
void test_prompt_keys_alt_s_keeps_the_caret_on_its_text(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!make test");
    /* On the `t` of `test`. */
    pk_caret_to(&f, 6U);
    pk_sudo(&f);
    pk_text(&f, "!sudo make test");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 11U);
    pk_sudo(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    /* At the body's start: the caret was on `m`, and stays on it. */
    pk_caret_to(&f, 1U);
    pk_sudo(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 6U);
    /* Inside the removed word: where the word began. */
    pk_caret_to(&f, 3U);
    pk_sudo(&f);
    pk_text(&f, "!make test");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 1U);
    /* Before the body (on the `!`): untouched. */
    pk_caret_to(&f, 0U);
    pk_sudo(&f);
    pk_text(&f, "!sudo make test");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 0U);
    pk_free(&f);
}

/* §1: not a shell command -- the line is untouched and the message says
 * why; the search prompt is never one.  And each toggle is ONE undo
 * step. */
void test_prompt_keys_alt_s_refuses_and_undoes_in_one_step(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "e foo.c");
    pk_sudo(&f);
    pk_text(&f, "e foo.c");
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "A-s: not a shell command");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_send(&f, (u32)'/', 0U);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_SEARCH_F);
    pk_type(&f, "!x");
    pk_sudo(&f);
    pk_text(&f, "!x");
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "A-s: not a shell command");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");

    pk_prompt(&f, NULL, 0U, "!make");
    pk_sudo(&f);
    pk_text(&f, "!sudo make");
    pk_run(&f, (u32)'_', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "!make");
    pk_run(&f, (u32)'/', YEW_MOD_ALT, "ed.edit.redo");
    pk_text(&f, "!sudo make");
    pk_sudo(&f);
    pk_text(&f, "!make");
    pk_run(&f, (u32)'_', YEW_MOD_CTRL, "ed.edit.undo");
    pk_text(&f, "!sudo make");
    pk_free(&f);
}

/* §3: the argv A-h would run, for the line and caret given. */
static u32 pk_man(PkFix *f, const char *line, size_t caret, Arena *a,
                  char *argv[7], const char **why)
{
    return yew_cmdline_man_argv(&f->ed, line, strlen(line), caret, a, argv,
                                why);
}

/*
 * Sprint 57.31 §3: the child command line.  The command word is argv[0]
 * of the simple command under the caret, wrappers stripped; a spec'd
 * command with subcommands tries `<cmd>-<sub>` first.  The script is
 * FIXED: whatever the line says reaches it only as $1 and $2.
 */
void test_prompt_keys_alt_h_builds_the_man_argv(void)
{
    PkFix f;
    Arena a;
    char *argv[7];
    const char *why = NULL;

    pk_init(&f);
    arena_init(&a);
    /* `git che‸`: the caret's word is where git takes a subcommand. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!git che", 8U, &a, argv, &why), 6U);
    YEW_ASSERT_EQ_STR(argv[0], "/bin/sh");
    YEW_ASSERT_EQ_STR(argv[1], "-c");
    YEW_ASSERT_EQ_STR(argv[2],
                      "man -- \"$1\" 2>/dev/null || man -- \"$2\"");
    YEW_ASSERT_EQ_STR(argv[3], "sh");
    YEW_ASSERT_EQ_STR(argv[4], "git-che");
    YEW_ASSERT_EQ_STR(argv[5], "git");
    YEW_ASSERT(argv[6] == NULL);
    YEW_ASSERT(why == NULL);
    /* Past the subcommand, the walk names it; a path is its basename. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!/usr/bin/git checkout -b fo", 28U, &a,
                             argv, &why), 6U);
    YEW_ASSERT_EQ_STR(argv[4], "git-checkout");
    YEW_ASSERT_EQ_STR(argv[5], "git");
    /* On the command word itself: its own page, the whole word. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!git log", 3U, &a, argv, &why), 5U);
    YEW_ASSERT_EQ_STR(argv[2], "man -- \"$1\"");
    YEW_ASSERT_EQ_STR(argv[4], "git");
    YEW_ASSERT(argv[5] == NULL);
    /* `sudo make‸`: the wrapper is stripped. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!sudo make", 10U, &a, argv, &why), 5U);
    YEW_ASSERT_EQ_STR(argv[4], "make");
    /* The simple command under the caret, not the line's first. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!ls -la | sort -r", 16U, &a, argv, &why),
                      5U);
    YEW_ASSERT_EQ_STR(argv[4], "sort");
    YEW_ASSERT_EQ_U64(pk_man(&f, "!ls -la | sort -r", 2U, &a, argv, &why),
                      5U);
    YEW_ASSERT_EQ_STR(argv[4], "ls");
    /* A quoted name is decoded, and stays one word. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!'my tool' x", 12U, &a, argv, &why), 5U);
    YEW_ASSERT_EQ_STR(argv[4], "my tool");
    YEW_ASSERT_EQ_STR(argv[2], "man -- \"$1\"");
    /* A subcommand that is not a plain word is never offered. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!git ../x", 9U, &a, argv, &why), 5U);
    YEW_ASSERT_EQ_STR(argv[4], "git");
    /* Nothing to look up. */
    YEW_ASSERT_EQ_U64(pk_man(&f, "!", 1U, &a, argv, &why), 0U);
    YEW_ASSERT_EQ_STR(why, "A-h: no command under the caret");
    YEW_ASSERT_EQ_U64(pk_man(&f, "!-rf x", 2U, &a, argv, &why), 0U);
    YEW_ASSERT_EQ_STR(why, "A-h: no command under the caret");
    YEW_ASSERT_EQ_U64(pk_man(&f, "e foo", 5U, &a, argv, &why), 0U);
    YEW_ASSERT_EQ_STR(why, "A-h: man pages are for :! commands");
    arena_free_all(&a);
    pk_free(&f);
}

/*
 * A fake `man` first on PATH: it appends its argc and each argument,
 * bracketed, to $YEW_TEST_MAN_OUT, and succeeds only for a name in
 * $YEW_TEST_MAN_HAVE.  The workspace root -- the child's directory -- is
 * the fixture directory, so an expansion that ran would leave a file
 * there.
 */
typedef struct PkMan {
    char dir[PATH_MAX];
    char out[PATH_MAX];
    char *path;
} PkMan;

static void pk_man_open(PkMan *m, PkFix *f, const char *have)
{
    static const char script[] =
        "#!/bin/sh\n"
        "{ printf '%s\\n' \"$#\"; for a in \"$@\"; do "
        "printf '[%s]\\n' \"$a\"; done; } >> \"$YEW_TEST_MAN_OUT\"\n"
        "case \" $YEW_TEST_MAN_HAVE \" in *\" $2 \"*) exit 0 ;; esac\n"
        "exit 1\n";
    const char *path = getenv("PATH");
    char man[PATH_MAX];
    char value[PATH_MAX * 2U];
    FILE *fp;
    int n;

    m->path = path == NULL ? NULL : strdup(path);
    n = snprintf(m->dir, sizeof(m->dir), "/tmp/yew-pkman-XXXXXX");
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(m->dir));
    YEW_ASSERT_NOT_NULL(mkdtemp(m->dir));
    n = snprintf(man, sizeof(man), "%s/man", m->dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(man));
    fp = fopen(man, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(script, 1U, sizeof(script) - 1U, fp),
                      sizeof(script) - 1U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(chmod(man, 0755), 0);
    n = snprintf(m->out, sizeof(m->out), "%s/out", m->dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(m->out));
    n = snprintf(value, sizeof(value), "%s:%s", m->dir,
                 path == NULL ? "/usr/bin:/bin" : path);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(value));
    YEW_ASSERT_EQ_I64(setenv("PATH", value, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_MAN_OUT", m->out, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_MAN_HAVE", have, 1), 0);
    YEW_ASSERT(yew_ed_set_workspace_root(&f->ed, m->dir));
}

/* What the fake man saw since the last call, then forgotten. */
static void pk_man_saw(PkMan *m, const char *want)
{
    char got[1024];
    FILE *fp = fopen(m->out, "rb");
    size_t n = 0U;

    if (fp != NULL) {
        n = fread(got, 1U, sizeof(got) - 1U, fp);
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
        YEW_ASSERT_EQ_I64(unlink(m->out), 0);
    }
    got[n] = '\0';
    YEW_ASSERT_EQ_STR(got, want);
}

static void pk_man_done(PkMan *m)
{
    char man[PATH_MAX];
    int n = snprintf(man, sizeof(man), "%s/man", m->dir);

    YEW_ASSERT(n > 0 && (size_t)n < sizeof(man));
    YEW_ASSERT_EQ_I64(unlink(man), 0);
    YEW_ASSERT_EQ_I64(rmdir(m->dir), 0);
    if (m->path != NULL)
        YEW_ASSERT_EQ_I64(setenv("PATH", m->path, 1), 0);
    free(m->path);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_TEST_MAN_OUT"), 0);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_TEST_MAN_HAVE"), 0);
}

static void pk_man_key(PkFix *f)
{
    pk_run(f, (u32)'h', YEW_MOD_ALT, "ed.cmdline.man_page");
}

/*
 * §3 through real keys, the handover stubbed as 57.18's tests stub it
 * (no controlling terminal, so the child runs on the inherited stdio).
 * A hostile name arrives as ONE argument and nothing in it runs; the
 * prompt is the same prompt afterwards -- kind, text, caret, generation
 * -- with a selection collapsed.
 */
void test_prompt_keys_alt_h_runs_man_with_names_as_arguments(void)
{
    PkFix f;
    PkMan m;
    u64 gen;
    char pwned[PATH_MAX];
    int n;

    pk_init(&f);
    pk_man_open(&m, &f, "git a b;$(touch pwned)`touch pwned`");
    n = snprintf(pwned, sizeof(pwned), "%s/pwned", m.dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(pwned));

    pk_prompt(&f, NULL, 0U, "!'a b;$(touch pwned)`touch pwned`' -x");
    gen = f.ed.cmdline.generation;
    pk_caret_to(&f, 3U);
    pk_man_key(&f);
    pk_man_saw(&m, "2\n[--]\n[a b;$(touch pwned)`touch pwned`]\n");
    YEW_ASSERT(access(pwned, F_OK) != 0);
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.generation, gen);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_CMD);
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_E);
    pk_text(&f, "!'a b;$(touch pwned)`touch pwned`' -x");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 3U);
    YEW_ASSERT(!f.ed.msg.active);

    /* `git che`: the subcommand's page first, then git's. */
    pk_prompt(&f, NULL, 0U, "!git che");
    pk_shift(&f, YEW_KEY_LEFT, 0U, "ed.sel.extend.left");
    pk_sel(&f, 7U, 8U);
    pk_man_key(&f);
    pk_man_saw(&m, "2\n[--]\n[git-che]\n2\n[--]\n[git]\n");
    YEW_ASSERT(!f.ed.msg.active);
    pk_nosel(&f);
    YEW_ASSERT_EQ_U64(pk_caret(&f), 7U);
    pk_text(&f, "!git che");
    pk_man_done(&m);
    pk_free(&f);
}

/* §3: no page -- a message on the prompt, which stays as it was; and
 * the refusals off a bang line. */
void test_prompt_keys_alt_h_reports_a_missing_page(void)
{
    PkFix f;
    PkMan m;

    pk_init(&f);
    pk_man_open(&m, &f, "");
    pk_prompt(&f, NULL, 0U, "!sudo make all");
    pk_caret_to(&f, 8U);
    pk_man_key(&f);
    pk_man_saw(&m, "2\n[--]\n[make]\n");
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "no man page for make");
    pk_text(&f, "!sudo make all");
    YEW_ASSERT_EQ_U64(pk_caret(&f), 8U);
    pk_prompt(&f, NULL, 0U, "!git frob");
    pk_man_key(&f);
    pk_man_saw(&m, "2\n[--]\n[git-frob]\n2\n[--]\n[git]\n");
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "no man page for git-frob or git");

    /* Not a shell command: said, and nothing runs. */
    pk_prompt(&f, NULL, 0U, "e foo.c");
    pk_man_key(&f);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "A-h: man pages are for :! commands");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_send(&f, (u32)'/', 0U);
    pk_type(&f, "!ls");
    pk_man_key(&f);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "A-h: man pages are for :! commands");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_prompt(&f, NULL, 0U, "!");
    pk_man_key(&f);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "A-h: no command under the caret");
    pk_man_saw(&m, "");
    pk_man_done(&m);
    pk_free(&f);
}

/* ----------------------------------------------------------------- A-e */

static void pk_edit(PkFix *f)
{
    pk_run(f, (u32)'e', YEW_MOD_ALT, "ed.cmdline.edit_in_buffer");
}

/* A key whose command may refuse: no status assertion. */
static void pk_try(PkFix *f, u32 code, u16 mods)
{
    yew_ed_handle_key(&f->ed, pk_key(code, mods), 0);
}

/* A `:` line typed from L and run with Enter; `ok` is what it returns. */
static void pk_colon(PkFix *f, const char *line, bool ok)
{
    pk_send(f, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT_EQ_U64(f->ed.mode, YEW_MODE_L);
    pk_send(f, (u32)':', 0U);
    pk_type(f, line);
    pk_try(f, YEW_KEY_ENTER, 0U);
    YEW_ASSERT_EQ_U64(f->ed.last_status == YEW_CMD_OK, ok);
}

/* The *command-line* buffer is focused and holds `want`. */
static void pk_scratch(PkFix *f, const char *want)
{
    Buffer *b = yew_ws_scratch_find(&f->ed, "*command-line*");
    Bytebuf got;
    TextIter it;
    u64 left;

    YEW_ASSERT_NOT_NULL(b);
    if (b == NULL)
        return;
    YEW_ASSERT(f->ed.win != NULL && f->ed.win->buf == b);
    YEW_ASSERT(!f->ed.cmdline.active);
    bytebuf_init(&got);
    left = yew_textbuf_len(b->tb);
    if (left != 0U && yew_textiter_begin(&it, b->tb, BYTEOFF(0U))) {
        while (left != 0U) {
            const u8 *p;
            u64 n;

            if (!yew_textiter_chunk(&it, b->tb, &p, &n) || n == 0U)
                break;
            if (n > left)
                n = left;
            bytebuf_append(&got, p, (size_t)n);
            left -= n;
            if (left != 0U && !yew_textiter_advance(&it, b->tb))
                break;
        }
    }
    YEW_ASSERT_EQ_U64(got.len, strlen(want));
    if (got.len == strlen(want) && got.len != 0U)
        YEW_ASSERT_EQ_MEM(got.data, want, got.len);
    bytebuf_free(&got);
}

static u64 pk_doc_caret(PkFix *f)
{
    const Cursor *c = yew_ed_cursor(&f->ed);

    return c == NULL ? UINT64_MAX : c->pos.v;
}

/* The prompt is back: `kind`, `text`, caret `caret`, in E, and the
 * buffer is gone. */
static void pk_back(PkFix *f, YewPromptKind kind, const char *text,
                    u64 caret)
{
    YEW_ASSERT(f->ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f->ed.cmdline.kind, kind);
    YEW_ASSERT_EQ_U64(f->ed.mode, YEW_MODE_E);
    pk_text(f, text);
    YEW_ASSERT_EQ_U64(pk_caret(f), caret);
    YEW_ASSERT(yew_ws_scratch_find(&f->ed, "*command-line*") == NULL);
    YEW_ASSERT_EQ_U64(f->ed.cmdedit.buf_id, 0U);
}

/*
 * Sprint 57.31 §2: A-e opens the BODY of a bang line in a new tab, caret
 * where it was, in Insert; `:q` brings the edited text back to the same
 * kind of prompt, caret at the end, in the tab it came from.
 */
void test_prompt_keys_alt_e_edits_the_body_and_q_returns_it(void)
{
    PkFix f;
    u32 tabs;
    u32 origin;

    pk_init(&f);
    tabs = yew_tab_count(&f.ed);
    origin = f.ed.tabs.v.data[f.ed.tabs.active].tab_id;
    pk_prompt(&f, NULL, 0U, "!make test");
    pk_caret_to(&f, 3U);
    pk_edit(&f);
    pk_scratch(&f, "make test");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    YEW_ASSERT_EQ_U64(pk_doc_caret(&f), 2U);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tabs + 1U);
    pk_type(&f, "x");
    pk_scratch(&f, "maxke test");
    pk_colon(&f, "q", true);
    pk_back(&f, YEW_PROMPT_CMD, "!maxke test", 11U);
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tabs);
    YEW_ASSERT_EQ_U64(f.ed.tabs.v.data[f.ed.tabs.active].tab_id, origin);
    /* And it is the ordinary prompt again: Esc goes back to L. */
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_L);

    /* A-v is the same key (fish); `:wq` and `:close` return it too. */
    pk_prompt(&f, NULL, 0U, "e foo.c");
    pk_run(&f, (u32)'v', YEW_MOD_ALT, "ed.cmdline.edit_in_buffer");
    pk_scratch(&f, "e foo.c");
    YEW_ASSERT_EQ_U64(pk_doc_caret(&f), 7U);
    pk_type(&f, "x");
    pk_colon(&f, "wq", true);
    pk_back(&f, YEW_PROMPT_CMD, "e foo.cx", 8U);
    pk_edit(&f);
    pk_colon(&f, "close", true);
    pk_back(&f, YEW_PROMPT_CMD, "e foo.cx", 8U);
    pk_edit(&f);
    pk_colon(&f, "tabclose", true);
    pk_back(&f, YEW_PROMPT_CMD, "e foo.cx", 8U);
    pk_free(&f);
}

/* §2.3: `:q!` (and `:close!`) discard -- the ORIGINAL line and caret. */
void test_prompt_keys_alt_e_discard_returns_the_original(void)
{
    PkFix f;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "r !date -u");
    pk_caret_to(&f, 5U);
    pk_edit(&f);
    pk_scratch(&f, "date -u");
    YEW_ASSERT_EQ_U64(pk_doc_caret(&f), 2U);
    pk_type(&f, "zzz");
    pk_send(&f, YEW_KEY_ENTER, 0U);
    pk_type(&f, "echo 'open");
    pk_colon(&f, "q!", true);
    pk_back(&f, YEW_PROMPT_CMD, "r !date -u", 5U);
    pk_edit(&f);
    pk_type(&f, "qqq");
    pk_colon(&f, "close!", true);
    pk_back(&f, YEW_PROMPT_CMD, "r !date -u", 5U);
    pk_free(&f);
}

/* Types `lines` into the buffer, a real Enter between them. */
static void pk_lines(PkFix *f, const char *const *lines, size_t n)
{
    size_t i;

    for (i = 0U; i < n; i++) {
        if (i != 0U)
            pk_send(f, YEW_KEY_ENTER, 0U);
        pk_type(f, lines[i]);
    }
}

/* One multi-line body through A-e and `:q`; the line it comes back as. */
static void pk_join(PkFix *f, const char *const *lines, size_t n,
                    const char *want)
{
    pk_prompt(f, NULL, 0U, "!");
    pk_edit(f);
    pk_lines(f, lines, n);
    pk_colon(f, "q", true);
    pk_back(f, YEW_PROMPT_CMD, want, strlen(want));
}

/*
 * §2.4 through real keys: a newline after a complete command is `; `;
 * after an operator, a keyword that opens a list, or an empty line it
 * is a blank; a `\` continuation vanishes; trailing newlines go.
 */
void test_prompt_keys_alt_e_joins_lines_as_the_shell_would(void)
{
    static const char *const two[] = {"make", "make install"};
    static const char *const cont[] = {"cc a.c \\", "  -o a"};
    static const char *const glued[] = {"echo a\\", "b"};
    static const char *const ops[] = {"ls |", "sort &&", "echo ok"};
    static const char *const kw[] = {"if true; then", "  echo y",
                                     "fi", ""};
    static const char *const empty[] = {"ls", "", "", "pwd", "", ""};
    PkFix f;

    pk_init(&f);
    pk_join(&f, two, YEW_ARRAY_LEN(two), "!make; make install");
    pk_join(&f, cont, YEW_ARRAY_LEN(cont), "!cc a.c -o a");
    pk_join(&f, glued, YEW_ARRAY_LEN(glued), "!echo ab");
    pk_join(&f, ops, YEW_ARRAY_LEN(ops), "!ls | sort && echo ok");
    pk_join(&f, kw, YEW_ARRAY_LEN(kw), "!if true; then echo y; fi");
    pk_join(&f, empty, YEW_ARRAY_LEN(empty), "!ls; pwd");
    pk_free(&f);
}

/*
 * §2.4: a newline the prompt cannot hold is REFUSED, naming the line,
 * and the buffer stays open with every byte in it -- by `:q`,
 * `:tabclose` and `:close` alike.  Fixed, it goes back.
 */
void test_prompt_keys_alt_e_refuses_a_newline_it_cannot_join(void)
{
    static const char *const quoted[] = {"ls", "pwd", "echo 'x", "y'"};
    static const char *const comment[] = {"ls # list", "pwd"};
    PkFix f;
    u32 tabs;

    pk_init(&f);
    tabs = yew_tab_count(&f.ed);
    pk_prompt(&f, NULL, 0U, "!");
    pk_edit(&f);
    pk_lines(&f, quoted, YEW_ARRAY_LEN(quoted));
    pk_colon(&f, "q", false);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text,
                               "line 3: a one-line prompt cannot hold a "
                               "newline inside quotes"));
    /* The `:q` line stays up with the error; the buffer is untouched. */
    YEW_ASSERT(f.ed.cmdline.active);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_scratch(&f, "ls\npwd\necho 'x\ny'");
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tabs + 1U);
    pk_colon(&f, "tabclose", false);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_scratch(&f, "ls\npwd\necho 'x\ny'");
    pk_colon(&f, "close", false);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_scratch(&f, "ls\npwd\necho 'x\ny'");
    pk_colon(&f, "q!", true);
    pk_back(&f, YEW_PROMPT_CMD, "!", 1U);

    /* A comment would swallow the next line: refused too. */
    pk_edit(&f);
    pk_lines(&f, comment, YEW_ARRAY_LEN(comment));
    pk_colon(&f, "q", false);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "line 1: "));
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_colon(&f, "q!", true);

    /* Not a shell line: any newline but a trailing one is refused. */
    pk_prompt(&f, NULL, 0U, "e foo");
    pk_edit(&f);
    pk_send(&f, YEW_KEY_ENTER, 0U);
    pk_type(&f, "bar");
    pk_colon(&f, "q", false);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text,
                               "line 1: a one-line prompt cannot hold a "
                               "newline"));
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    /* Fixed (the newline deleted), it goes back. */
    pk_send(&f, (u32)'i', 0U);
    pk_send(&f, YEW_KEY_HOME, 0U);
    pk_send(&f, YEW_KEY_BACKSPACE, 0U);
    pk_scratch(&f, "e foobar");
    pk_colon(&f, "q", true);
    pk_back(&f, YEW_PROMPT_CMD, "e foobar", 8U);
    pk_free(&f);
}

/*
 * §2 pitfall: the return hangs off the buffer's RELEASE.  A tab closed
 * with no command at all (as its close box does) still returns the line
 * at the next event boundary; one whose text cannot be one line comes
 * back as a buffer, never as nothing.
 */
void test_prompt_keys_alt_e_returns_on_any_release(void)
{
    static const char *const bad[] = {"echo 'a", "b'"};
    PkFix f;
    int idx;

    pk_init(&f);
    pk_prompt(&f, NULL, 0U, "!ls");
    pk_edit(&f);
    pk_type(&f, " -la");
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    idx = f.ed.tabs.active;
    YEW_ASSERT(yew_tab_close(&f.ed, idx));
    yew_cmdedit_settle(&f.ed);
    pk_back(&f, YEW_PROMPT_CMD, "!ls -la", 7U);

    pk_edit(&f);
    pk_send(&f, YEW_KEY_END, 0U);
    pk_send(&f, YEW_KEY_ENTER, 0U);
    pk_lines(&f, bad, YEW_ARRAY_LEN(bad));
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT(yew_tab_close(&f.ed, f.ed.tabs.active));
    yew_cmdedit_settle(&f.ed);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text,
                               "line 2: a one-line prompt cannot hold a "
                               "newline inside quotes"));
    pk_scratch(&f, "ls -la\necho 'a\nb'");

    /* The buffer dropped outright (a script's ed.buf.close) is the same
     * release. */
    yew_ws_scratch_drop(&f.ed, f.ed.win->buf);
    yew_cmdedit_settle(&f.ed);
    YEW_ASSERT_NOT_NULL(yew_ws_scratch_find(&f.ed, "*command-line*"));
    pk_colon(&f, "q!", true);
    pk_back(&f, YEW_PROMPT_CMD, "!ls -la", 7U);

    /* Released while another prompt is open: the line waits for that
     * prompt to close, and A-e meanwhile does not start over on it. */
    pk_edit(&f);
    pk_type(&f, " /");
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    pk_send(&f, (u32)':', 0U);
    pk_type(&f, "e x");
    yew_ws_scratch_drop(&f.ed, yew_ws_scratch_find(&f.ed, "*command-line*"));
    yew_cmdedit_settle(&f.ed);
    YEW_ASSERT(f.ed.cmdedit.released);
    pk_text(&f, "e x");
    pk_edit(&f);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "A-e: *command-line* is still returning its line");
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_back(&f, YEW_PROMPT_CMD, "!ls -la /", 9U);
    pk_free(&f);
}

/* §2.5: one at a time.  A second A-e switches to the open buffer, and
 * the line it was pressed on is kept where Up finds it. */
void test_prompt_keys_alt_e_twice_switches_to_the_open_buffer(void)
{
    PkFix f;
    u32 tabs;

    pk_init(&f);
    tabs = yew_tab_count(&f.ed);
    pk_prompt(&f, NULL, 0U, "!make");
    pk_edit(&f);
    pk_send(&f, YEW_KEY_ESCAPE, 0U);
    /* Back to the first tab, a new prompt there, A-e again. */
    yew_tab_switch(&f.ed, 0);
    pk_send(&f, (u32)':', 0U);
    pk_type(&f, "e other");
    pk_edit(&f);
    pk_scratch(&f, "make");
    YEW_ASSERT_EQ_U64(yew_tab_count(&f.ed), tabs + 1U);
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "*command-line* is already open");
    YEW_ASSERT_EQ_U64(f.ed.mode, YEW_MODE_I);
    pk_colon(&f, "q", true);
    pk_back(&f, YEW_PROMPT_CMD, "!make", 5U);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");
    pk_prompt(&f, NULL, 0U, "e ot");
    pk_run(&f, YEW_KEY_UP, 0U, "ed.cmdline.up");
    pk_text(&f, "e other");
    pk_free(&f);
}

/* §2.1: the prompt's KIND comes back -- a search stays a search -- and a
 * prompt whose owner waits on its answer does not leave. */
void test_prompt_keys_alt_e_keeps_the_prompt_kind(void)
{
    PkFix f;

    pk_init(&f);
    pk_send(&f, (u32)'?', 0U);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_SEARCH_B);
    pk_type(&f, "needle");
    pk_edit(&f);
    pk_scratch(&f, "needle");
    pk_type(&f, "s");
    pk_colon(&f, "q", true);
    pk_back(&f, YEW_PROMPT_SEARCH_B, "needles", 7U);
    pk_run(&f, (u32)'g', YEW_MOD_CTRL, "ed.cmdline.cancel");

    yew_cmdline_open_input(&f.ed, "answer", NULL, NULL);
    pk_edit(&f);
    YEW_ASSERT(f.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.kind, YEW_PROMPT_INPUT);
    YEW_ASSERT_EQ_STR(f.ed.msg.text,
                      "A-e: this prompt cannot become a buffer");
    pk_text(&f, "answer");
    pk_free(&f);
}

/* The one-line rule itself, byte for byte. */
static void pk_oneline(const char *in, bool shell, const char *want,
                       u32 want_line)
{
    Bytebuf out;
    u32 line = 99U;
    const char *why = NULL;
    bool ok;

    bytebuf_init(&out);
    ok = yew_cmdedit_oneline(in, strlen(in), shell, &out, &line, &why);
    YEW_ASSERT_EQ_U64(ok, want != NULL);
    if (want != NULL) {
        YEW_ASSERT_EQ_U64(out.len, strlen(want));
        if (out.len == strlen(want) && out.len != 0U)
            YEW_ASSERT_EQ_MEM(out.data, want, out.len);
    } else {
        YEW_ASSERT_EQ_U64(line, want_line);
    }
    bytebuf_free(&out);
}

void test_cmdedit_oneline_rules(void)
{
    pk_oneline("make\nmake install\n", true, "make; make install", 0U);
    pk_oneline("make\r\nmake install\r\n", true, "make; make install", 0U);
    pk_oneline("a &\nb", true, "a & b", 0U);
    pk_oneline("a;\nb", true, "a; b", 0U);
    pk_oneline("a ||\n  b", true, "a || b", 0U);
    pk_oneline("while true\ndo\n  x\ndone", true,
               "while true; do x; done", 0U);
    pk_oneline("{\n a\n}", true, "{ a; }", 0U);
    pk_oneline("x=$(\n date\n)", true, "x=$( date; )", 0U);
    pk_oneline("echo a\\ \nb", true, "echo a\\ ; b", 0U);
    pk_oneline("echo a \\\nb", true, "echo a b", 0U);
    pk_oneline("echo \"a\nb\"", true, NULL, 1U);
    pk_oneline("ls\ncat <<EOF\nx\nEOF", true, NULL, 2U);
    pk_oneline("ls # c\n", true, "ls # c", 0U);
    pk_oneline("ls\n\n", false, "ls", 0U);
    pk_oneline("a\nb", false, NULL, 1U);
    pk_oneline("", true, "", 0U);
    {
        /* A NUL is refused, with the line it is on. */
        Bytebuf out;
        u32 line = 0U;
        const char *why = NULL;

        bytebuf_init(&out);
        YEW_ASSERT(!yew_cmdedit_oneline("a\nb\0c", 5U, false, &out, &line,
                                        &why));
        YEW_ASSERT_EQ_U64(line, 2U);
        YEW_ASSERT_EQ_STR(why, "a prompt cannot hold a NUL byte");
        bytebuf_free(&out);
    }
}
