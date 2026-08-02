#include "edit/cmd.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static CmdStatus cmd_mode_enter(CmdCtx *cx)
{
    const char *sprint = "14";

    if (cx->sarg_len == 1U) {
        switch (cx->sarg[0]) {
        case 'W':
        case 'B':
            sprint = "16";
            break;
        case 'H':
            sprint = "17";
            break;
        case 'E':
            sprint = "18";
            break;
        case 'F':
            sprint = "52";
            break;
        default:
            break;
        }
    }
    sag_log(SAG_LOG_ERROR,
            "command not implemented yet: ed.mode.enter lands in Sprint %s",
            sprint);
    return SAG_CMD_ERR_DEFERRED;
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
    DEFER("ed.quit", SAG_ARITY_NONE, 0U, 14, "quit the editor"),
    DEFER("ed.quit_force", SAG_ARITY_NONE, 0U, 14,
          "quit and discard unsaved changes"),
    DEFER("ed.suspend", SAG_ARITY_NONE, 0U, 14, "suspend the editor"),
    DEFER("ed.redraw", SAG_ARITY_NONE, 0U, 15, "redraw the display"),
    DEFER("ed.repeat", SAG_ARITY_NONE, SAG_CMD_RECORDABLE, 35,
          "repeat the last command"),

    DEFER("ed.move.buf.home", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move to the start of the buffer"),
    DEFER("ed.move.buf.end", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move to the end of the buffer"),
    DEFER("ed.move.line.home", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move to the start of the line"),
    DEFER("ed.move.line.end", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move to the end of the line"),
    DEFER("ed.move.line.up", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move one display line up"),
    DEFER("ed.move.line.down", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move one display line down"),
    DEFER("ed.move.unit.next", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 16,
          "move to the next unit"),
    DEFER("ed.move.unit.prev", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 16,
          "move to the previous unit"),
    DEFER("ed.move.char.left", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move one character left"),
    DEFER("ed.move.char.right", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "move one character right"),

    DEFER("ed.edit.insert.text", SAG_ARITY_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER, 14,
          "insert text at the cursor"),
    DEFER("ed.edit.delete.prev", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
              SAG_CMD_CHANGES_BUFFER,
          14, "delete the previous character"),
    DEFER("ed.edit.delete.next", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN |
              SAG_CMD_CHANGES_BUFFER,
          14, "delete the next character"),
    DEFER("ed.edit.undo", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "undo the last edit"),
    DEFER("ed.edit.redo", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "redo the last undone edit"),
    DEFER("ed.edit.yank", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 14,
          "yank text into a register"),
    DEFER("ed.edit.paste", SAG_ARITY_OPT_STR,
          SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN | SAG_CMD_CHANGES_BUFFER, 14,
          "paste text from a register"),

    {"ed.mode.enter", cmd_mode_enter, SAG_ARITY_STR, SAG_CMD_RECORDABLE,
     "Sprint 14: enter L/I; W/B Sprint 16, H Sprint 17, E Sprint 18, F Sprint 52"},
    DEFER("ed.mode.escape", SAG_ARITY_NONE, SAG_CMD_RECORDABLE, 14,
          "return to line mode"),
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

    DEFER("ed.view.center", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 15,
          "center the active view"),
    DEFER("ed.view.up", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 15,
          "scroll the active view up"),
    DEFER("ed.view.down", SAG_ARITY_NONE,
          SAG_CMD_REPEATABLE | SAG_CMD_RECORDABLE | SAG_CMD_NEEDS_WIN, 15,
          "scroll the active view down"),
    DEFER("ed.ui.message_expand", SAG_ARITY_NONE, SAG_CMD_PROMPTS, 15,
          "expand the current message"),
    DEFER("ed.ui.cancel", SAG_ARITY_NONE, 0U, 15,
          "cancel the active prompt"),

    DEFER("ed.file.open", SAG_ARITY_STR, SAG_CMD_PROMPTS, 14,
          "open a file"),
    DEFER("ed.file.save", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 14,
          "save the active file"),
    DEFER("ed.file.new", SAG_ARITY_OPT_STR, 0U, 14, "create a file"),
    DEFER("ed.file.reload", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 14,
          "reload the active file"),
    DEFER("ed.file.close", SAG_ARITY_NONE, SAG_CMD_NEEDS_WIN, 14,
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
        "message_expand", "split_h", "split_v", "record", "replay", "stage"};
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
