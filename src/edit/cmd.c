#include "edit/cmd.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "edit/edit_cmds.h"
#include "edit/file_cmds.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/log.h"

typedef struct {
    Arena arena;
    Interner names;
    CmdDesc *descs;
    size_t len;
    size_t cap;
    CmdRecordTap record_tap;
    bool initialized;
} CmdRegistry;

static CmdRegistry registry;

static CmdStatus cmd_nop(CmdCtx *cx)
{
    (void)cx;
    return SAG_CMD_OK;
}

static CmdStatus deferred_unreachable(CmdCtx *cx)
{
    (void)cx;
    SAG_BUG("deferred command reached its implementation");
}

#define DEFER(name_, arity_, flags_, sprint_, help_)                           \
    {                                                                          \
        name_, deferred_unreachable, arity_, (flags_) | SAG_CMD_DEFERRED,      \
            "Sprint " #sprint_ ": " help_                                    \
    }

static const CmdDesc builtins[] = {
    {"ed.nop", cmd_nop, SAG_ARITY_NONE, 0U, "Do nothing"},
    {"ed.quit", sag_file_cmd_quit, SAG_ARITY_NONE, 0U,
     "Quit, prompting when the buffer is dirty"},
    {"ed.quit_force", sag_file_cmd_quit_force, SAG_ARITY_NONE, 0U,
     "Quit without discarding the recovery journal"},
    {"ed.suspend", sag_file_cmd_suspend, SAG_ARITY_NONE, 0U,
     "Suspend the editor and restore the terminal"},
    {"ed.redraw", sag_file_cmd_redraw, SAG_ARITY_NONE, 0U,
     "Redraw the complete display"},
    DEFER("ed.repeat", SAG_ARITY_NONE, SAG_CMD_RECORDABLE, 35,
          "repeat the last command"),

    {"ed.move.buf.home", sag_edit_cmd_move_buf_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the buffer"},
    {"ed.move.buf.end", sag_edit_cmd_move_buf_end, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the buffer or a counted line"},
    {"ed.move.line.home", sag_edit_cmd_move_line_home, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the start of the line"},
    {"ed.move.line.end", sag_edit_cmd_move_line_end, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the end of the line"},
    {"ed.move.line.up", sag_edit_cmd_move_line_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line up"},
    {"ed.move.line.down", sag_edit_cmd_move_line_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one line down"},
    {"ed.move.line.first_nonblank", sag_edit_cmd_move_line_first_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the first nonblank grapheme"},
    {"ed.move.line.last_nonblank", sag_edit_cmd_move_line_last_nonblank,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move to the last nonblank grapheme"},
    {"ed.move.line.half_page_up", sag_edit_cmd_move_line_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport up"},
    {"ed.move.line.half_page_down", sag_edit_cmd_move_line_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move half a viewport down"},
    DEFER("ed.move.unit.next", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 16,
          "move to the next unit"),
    DEFER("ed.move.unit.prev", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 16,
          "move to the previous unit"),
    {"ed.move.char.prev", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme left"},
    {"ed.move.char.next", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one grapheme right"},
    {"ed.move.char.left", sag_edit_cmd_move_char_prev, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme left"},
    {"ed.move.char.right", sag_edit_cmd_move_char_next, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for moving one grapheme right"},

    {"ed.edit.insert.text", sag_edit_cmd_insert_text, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert UTF-8 text at the cursor"},
    {"ed.edit.insert.newline", sag_edit_cmd_insert_newline, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert the buffer's native line ending"},
    {"ed.edit.insert.tab", sag_edit_cmd_insert_tab, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Insert a literal tab"},
    {"ed.edit.insert.after", sag_edit_cmd_insert_after, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Enter insert mode after the current grapheme"},
    {"ed.edit.line.open_below", sag_edit_cmd_open_below, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line below and enter insert mode"},
    {"ed.edit.line.open_above", sag_edit_cmd_open_above, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER,
     "Open a new line above and enter insert mode"},
    {"ed.edit.delete.grapheme_left", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme left of the cursor"},
    {"ed.edit.delete.grapheme", sag_edit_cmd_delete_grapheme,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the grapheme at the cursor"},
    {"ed.edit.line.delete", sag_edit_cmd_delete_line, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Delete the current logical line"},
    {"ed.edit.delete.prev", sag_edit_cmd_delete_grapheme_left,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the previous grapheme"},
    {"ed.edit.delete.next", sag_edit_cmd_delete_grapheme, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
         SAG_CMD_CHANGES_BUFFER,
     "Alias for deleting the next grapheme"},
    {"ed.edit.undo", sag_edit_cmd_undo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Undo the last edit transaction"},
    {"ed.edit.redo", sag_edit_cmd_redo, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Redo the last undone transaction"},
    {"ed.edit.undo_barrier", sag_edit_cmd_undo_barrier, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Close the active insert transaction"},
    DEFER("ed.edit.yank", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17,
          "yank text into a register"),
    DEFER("ed.edit.paste", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER, 17,
          "paste text from a register"),

    {"ed.mode.enter", sag_edit_cmd_mode_enter, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE,
     "Sprint 14: enter L/I; W/B Sprint 16, H Sprint 17, E Sprint 18, F Sprint 52"},
    {"ed.mode.escape", sag_edit_cmd_mode_escape, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE, "Return to line mode"},
    DEFER("ed.sel.expand", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17,
          "expand the selection"),
    DEFER("ed.sel.contract", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17,
          "contract the selection"),
    DEFER("ed.cursor.add", SAG_ARITY_INT,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17, "add a cursor"),
    DEFER("ed.cursor.delete", SAG_ARITY_NONE,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17, "delete a cursor"),

    {"ed.view.center", sag_edit_cmd_view_center, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Center the cursor line"},
    {"ed.view.top", sag_edit_cmd_view_top, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the top"},
    {"ed.view.bottom", sag_edit_cmd_view_bottom, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Place the cursor line at the bottom"},
    {"ed.view.scroll.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view up one display row"},
    {"ed.view.scroll.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll the active view down one display row"},
    {"ed.view.up", sag_edit_cmd_view_scroll_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view up"},
    {"ed.view.down", sag_edit_cmd_view_scroll_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Alias for scrolling the active view down"},
    {"ed.view.page_up", sag_edit_cmd_view_page_up, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport up"},
    {"ed.view.page_down", sag_edit_cmd_view_page_down, SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Move one viewport down"},
    {"ed.view.half_page_up", sag_edit_cmd_view_half_page_up,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport up"},
    {"ed.view.half_page_down", sag_edit_cmd_view_half_page_down,
     SAG_ARITY_NONE,
     SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Scroll half a viewport down"},
    {"ed.view.goto_line", sag_edit_cmd_view_goto_line, SAG_ARITY_NONE,
     SAG_CMD_TAKES_COUNT | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Go to a counted line and center it"},
    {"ed.view.toggle_wrap", sag_edit_cmd_view_toggle_wrap, SAG_ARITY_NONE,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, "Toggle line wrapping"},
    {"ed.view.number_style", sag_edit_cmd_view_number_style, SAG_ARITY_STR,
     SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN,
     "Set line numbers to none, abs, rel, or hybrid"},
    {"ed.ui.message_expand", sag_edit_cmd_message_expand, SAG_ARITY_NONE,
     SAG_CMD_PROMPTS, "Expand the current message"},
    {"ed.ui.cancel", sag_edit_cmd_ui_cancel, SAG_ARITY_NONE, 0U,
     "Cancel the active prompt or message overlay"},

    DEFER("ed.file.open", SAG_ARITY_STR, SAG_CMD_PROMPTS, 23,
          "open a file"),
    {"ed.file.save", sag_file_cmd_save_current, SAG_ARITY_NONE,
     SAG_CMD_NEEDS_WIN, "Atomically save the active file"},
    DEFER("ed.file.new", SAG_ARITY_OPT_STR, 0U, 18, "create a file"),
    DEFER("ed.file.reload", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 18,
          "reload the active file"),
    DEFER("ed.file.close", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 23,
          "close the active file"),
    DEFER("ed.buf.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the next buffer"),
    DEFER("ed.buf.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 23,
          "activate the previous buffer"),
    DEFER("ed.tab.goto", SAG_ARITY_INT, SAG_CMD_TAKES_COUNT, 23,
          "activate a numbered tab"),
    DEFER("ed.tab.new", SAG_ARITY_NONE, 0U, 23, "create a tab"),
    DEFER("ed.tab.close", SAG_ARITY_NONE, 0U, 23, "close the active tab"),
    DEFER("ed.group.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the next tab group"),
    DEFER("ed.group.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 24,
          "activate the previous tab group"),
    DEFER("ed.pane.split_h", SAG_ARITY_NONE, 0U, 22,
          "split the pane horizontally"),
    DEFER("ed.pane.split_v", SAG_ARITY_NONE, 0U, 22,
          "split the pane vertically"),
    DEFER("ed.pane.grow", SAG_ARITY_OPT_INT, SAG_CMD_TAKES_COUNT, 22,
          "grow the active pane"),
    DEFER("ed.pane.shrink", SAG_ARITY_OPT_INT, SAG_CMD_TAKES_COUNT, 22,
          "shrink the active pane"),
    DEFER("ed.win.next", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the next window"),
    DEFER("ed.win.prev", SAG_ARITY_NONE, SAG_CMD_REPEATABLE, 22,
          "focus the previous window"),

    DEFER("ed.reg.yank", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 17,
          "yank into a register"),
    DEFER("ed.reg.paste", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER, 17,
          "paste from a register"),
    DEFER("ed.search.open", SAG_ARITY_OPT_STR, SAG_CMD_PROMPTS, 21,
          "open incremental search"),
    DEFER("ed.search.next", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 21,
          "move to the next search match"),
    DEFER("ed.search.prev", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 21,
          "move to the previous search match"),
    DEFER("ed.macro.record", SAG_ARITY_OPT_STR, SAG_CMD_PROMPTS, 35,
          "record a command macro"),
    DEFER("ed.macro.replay", SAG_ARITY_OPT_STR,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE, 35,
          "replay a command macro"),
    DEFER("ed.job.cancel", SAG_ARITY_OPT_INT, 0U, 19,
          "cancel a background job"),
    DEFER("ed.git.stage", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 52,
          "stage the selected path"),
    DEFER("ed.lsp.goto", SAG_ARITY_STR, SAG_CMD_NEEDS_WIN, 47,
          "go to an LSP location"),
    DEFER("ed.ai.open", SAG_ARITY_NONE, SAG_CMD_PROMPTS, 49,
          "open the AI prompt"),
    DEFER("ed.plug.reload", SAG_ARITY_OPT_STR, 0U, 54,
          "reload a plugin")
};

#undef DEFER

static bool word_in(const char *word, size_t len, const char *const *words,
                    size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strlen(words[i]) == len && memcmp(words[i], word, len) == 0)
            return true;
    }
    return false;
}

static bool command_name_valid(const char *name)
{
    static const char *const app_verbs[] = {
        "quit", "quit_force", "suspend", "redraw", "repeat", "nop"};
    static const char *const domains[] = {
        "move", "edit", "mode", "sel", "cursor", "view", "ui",
        "file", "buf", "tab", "group", "pane", "win", "reg",
        "search", "macro", "job", "git", "lsp", "ai", "plug"};
    static const char *const verbs[] = {
        "home", "end", "next", "prev", "up", "down", "left", "right",
        "goto", "insert", "delete", "replace", "yank", "paste", "toggle",
        "open", "close", "save", "new", "enter", "leave", "grow",
        "shrink", "expand", "contract", "list", "reload", "cancel",
        "text", "undo", "redo", "escape", "add", "above", "below", "center",
        "message_expand", "split_h", "split_v", "record", "replay", "stage",
        "first_nonblank", "last_nonblank", "half_page_up", "half_page_down",
        "page_up", "page_down", "after", "newline", "tab",
        "grapheme_left", "grapheme", "undo_barrier", "open_above",
        "open_below", "top", "bottom", "goto_line", "toggle_wrap",
        "number_style"};
    const char *segments[4];
    size_t lengths[4];
    const char *p;
    size_t n = 0;

    if (name == NULL)
        return false;
    p = name;
    for (;;) {
        const char *start = p;
        size_t len;

        if (n == SAG_ARRAY_LEN(segments))
            return false;
        while (*p != '\0' && *p != '.')
            p++;
        len = (size_t)(p - start);
        if (len == 0U || len > 16U || start[0] < 'a' || start[0] > 'z')
            return false;
        {
            size_t i;
            for (i = 1; i < len; i++) {
                if (!((start[i] >= 'a' && start[i] <= 'z') ||
                      (start[i] >= '0' && start[i] <= '9') ||
                      start[i] == '_'))
                    return false;
            }
        }
        segments[n] = start;
        lengths[n++] = len;
        if (*p == '\0')
            break;
        p++;
    }
    if (lengths[0] != 2U || memcmp(segments[0], "ed", 2U) != 0)
        return false;
    if (n == 2U)
        return word_in(segments[1], lengths[1], app_verbs,
                       SAG_ARRAY_LEN(app_verbs));
    if ((n != 3U && n != 4U) ||
        !word_in(segments[1], lengths[1], domains, SAG_ARRAY_LEN(domains)))
        return false;
    return word_in(segments[n - 1U], lengths[n - 1U], verbs,
                   SAG_ARRAY_LEN(verbs));
}

static bool help_names_sprint(const char *help)
{
    const char *p;

    if (help == NULL)
        return false;
    p = strstr(help, "Sprint ");
    if (p == NULL)
        return false;
    p += strlen("Sprint ");
    return isdigit((unsigned char)*p) != 0;
}

static void desc_validate(const CmdDesc *d)
{
    const u32 known_flags = SAG_CMD_REPEATABLE | SAG_CMD_TAKES_COUNT |
                            SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
                            SAG_CMD_CHANGES_BUFFER | SAG_CMD_PROMPTS |
                            SAG_CMD_DEFERRED;

    if (d == NULL)
        SAG_BUG("sag_cmd_register: NULL descriptor");
    if (!command_name_valid(d->name))
        SAG_BUG("invalid command name: %s", d->name ? d->name : "(null)");
    if (d->fn == NULL)
        SAG_BUG("command %s has no implementation", d->name);
    if (d->arity > SAG_ARITY_OPT_STR)
        SAG_BUG("command %s has invalid arity %u", d->name,
                (unsigned)d->arity);
    if ((d->flags & ~known_flags) != 0U)
        SAG_BUG("command %s has unknown flags", d->name);
    if ((d->flags & SAG_CMD_REPEATABLE) != 0U &&
        (d->flags & SAG_CMD_TAKES_COUNT) != 0U)
        SAG_BUG("command %s is both REPEATABLE and TAKES_COUNT", d->name);
    if (d->help == NULL || d->help[0] == '\0')
        SAG_BUG("command %s has empty help", d->name);
    if ((d->flags & SAG_CMD_DEFERRED) != 0U &&
        !help_names_sprint(d->help))
        SAG_BUG("deferred command %s help does not name its sprint", d->name);
}

static CmdId register_desc(const CmdDesc *d)
{
    CmdDesc copy;
    u32 id;

    desc_validate(d);
    if (strmap_has(&registry.names.map, d->name, strlen(d->name)))
        SAG_BUG("duplicate command registration: %s", d->name);
    if (registry.len == UINT32_MAX)
        SAG_BUG("command registry overflow");
    if (registry.len == registry.cap) {
        size_t cap = registry.cap ? registry.cap * 2U : 64U;

        if (cap < registry.cap)
            SAG_BUG("command registry allocation overflow");
        registry.descs = sag_xreallocarray(registry.descs, cap,
                                           sizeof(*registry.descs));
        registry.cap = cap;
    }
    id = sag_intern_cstr(&registry.names, d->name);
    if ((size_t)id != registry.len + 1U)
        SAG_BUG("command registry and interner order diverged");
    copy = *d;
    copy.name = sag_intern_str(&registry.names, id);
    copy.help = arena_strdup(&registry.arena, d->help);
    registry.descs[registry.len++] = copy;
    return (CmdId){id};
}

void sag_cmd_init(void)
{
    size_t i;

    if (registry.initialized)
        return;
    arena_init(&registry.arena);
    interner_init(&registry.names, &registry.arena);
    registry.initialized = true;
    for (i = 0; i < SAG_ARRAY_LEN(builtins); i++)
        (void)register_desc(&builtins[i]);
}

void sag_cmd_shutdown(void)
{
    if (!registry.initialized)
        return;
    interner_free(&registry.names);
    arena_free_all(&registry.arena);
    free(registry.descs);
    registry = (CmdRegistry){0};
}

CmdId sag_cmd_register(const CmdDesc *d)
{
    sag_cmd_init();
    return register_desc(d);
}

CmdId sag_cmd_lookup(const char *name, u32 len)
{
    void *found;

    sag_cmd_init();
    if (name == NULL)
        return SAG_CMD_NONE;
    found = strmap_get(&registry.names.map, name, len);
    return (CmdId){(u32)(uintptr_t)found};
}

const CmdDesc *sag_cmd_desc(CmdId id)
{
    sag_cmd_init();
    if (id.v == 0U || (size_t)id.v > registry.len)
        return NULL;
    return &registry.descs[id.v - 1U];
}

static bool args_valid(const CmdDesc *d, const CmdCtx *cx)
{
    if (cx->source < SAG_SRC_KEY || cx->source > SAG_SRC_TEST)
        return false;
    if (cx->count == 0U)
        return false;
    if (cx->sarg == NULL && cx->sarg_len != 0U)
        return false;
    switch ((CmdArity)d->arity) {
    case SAG_ARITY_NONE:
        return cx->iarg == 0 && cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_INT:
    case SAG_ARITY_OPT_INT:
        return cx->sarg == NULL && cx->sarg_len == 0U;
    case SAG_ARITY_STR:
        return cx->iarg == 0 && cx->sarg != NULL;
    case SAG_ARITY_OPT_STR:
        return cx->iarg == 0;
    }
    return false;
}

static CmdStatus command_fail(const CmdDesc *d, const char *reason,
                              CmdStatus status)
{
    sag_log(SAG_LOG_ERROR, "command failed: %s: %s", d->name, reason);
    return status;
}

static const char *deferred_sprint(const CmdDesc *d, char *number,
                                   size_t number_size)
{
    const char *sprint = strstr(d->help, "Sprint ");
    size_t n = 0;

    sprint += strlen("Sprint ");
    while (isdigit((unsigned char)sprint[n]) && n + 1U < number_size) {
        number[n] = sprint[n];
        n++;
    }
    number[n] = '\0';
    return number;
}

static CmdStatus command_deferred(const CmdDesc *d)
{
    char number[16];
    const char *sprint = deferred_sprint(d, number, sizeof(number));

    sag_log(SAG_LOG_ERROR,
            "command not implemented yet: %s lands in Sprint %s", d->name,
            sprint);
    return SAG_CMD_ERR_DEFERRED;
}

CmdStatus sag_cmd_invoke(CmdId id, CmdCtx *cx)
{
    const CmdDesc *d = sag_cmd_desc(id);
    CmdStatus status = SAG_CMD_OK;
    u32 n;
    u32 i;

    if (d == NULL || cx == NULL)
        return SAG_CMD_ERR_ARG;
    if ((d->flags & SAG_CMD_NEEDS_WIN) != 0U && cx->win == NULL)
        return command_fail(d, "no window", SAG_CMD_ERR_STATE);
    if ((d->flags & SAG_CMD_DEFERRED) != 0U)
        return command_deferred(d);
    if (!args_valid(d, cx))
        return command_fail(d, "invalid arguments", SAG_CMD_ERR_ARG);
    if (registry.record_tap != NULL &&
        (d->flags & SAG_CMD_RECORDABLE) != 0U)
        registry.record_tap(id, cx);
    n = (d->flags & SAG_CMD_REPEATABLE) != 0U ? cx->count : 1U;
    for (i = 0; i < n && status == SAG_CMD_OK; i++)
        status = d->fn(cx);
    return status;
}

u32 sag_cmd_count(void)
{
    sag_cmd_init();
    return (u32)registry.len;
}

const CmdDesc *sag_cmd_at(u32 i)
{
    sag_cmd_init();
    if ((size_t)i >= registry.len)
        return NULL;
    return &registry.descs[i];
}

void sag_cmd_set_record_tap(CmdRecordTap tap)
{
    sag_cmd_init();
    registry.record_tap = tap;
}
