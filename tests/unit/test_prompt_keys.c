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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "text/clipboard.h"
#include "ui/cmdhist.h"
#include "ui/cmdline.h"

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

/* A-r and C-r both insert a register; the register name is the next key,
 * printable or not. */
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
    pk_send(&f, (u32)'r', YEW_MOD_CTRL);
    pk_send(&f, (u32)'a', 0U);
    pk_text(&f, "x regreg");
    pk_free(&f);
}

static void pk_fake_clipboard(char path[PATH_MAX], const char *bytes)
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    char fake[PATH_MAX];
    char value[PATH_MAX * 3U];
    FILE *fp;
    int fd;
    int n;

    if (slash != NULL) {
        size_t prefix = (size_t)(slash - program);

        YEW_ASSERT(prefix + sizeof("/fakeclip") <= sizeof(fake));
        (void)memcpy(fake, program, prefix);
        (void)memcpy(fake + prefix, "/fakeclip", sizeof("/fakeclip"));
    } else {
        n = snprintf(fake, sizeof(fake), "./fakeclip");
        YEW_ASSERT(n > 0 && (size_t)n < sizeof(fake));
    }
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
