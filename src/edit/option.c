#include "edit/option.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/flhook.h"
#include "search/searchui.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/gutter.h"
#include "ui/layout.h"
#include "ui/viewport.h"
#include "unicode/width.h"
#include "util/log.h"
#include "util/strmap.h"

struct OptStored {
    OptVal value;
    char *owned;
};

static const char *const number_values[] = {
    "off", "abs", "rel", "both", NULL
};
static const char *const status_column_values[] = {
    "gcol", "gcol_ccol", NULL
};
static const char *const clipboard_values[] = {
    "off", "yank", "all", "unnamed", NULL
};

#define OPT_BOOL(v_) {SAG_OPT_BOOL, {.b = (v_)}}
#define OPT_INT(v_) {SAG_OPT_INT, {.i = (v_)}}
#define OPT_STR(v_) {SAG_OPT_STR, {.str = {(v_), (u32)(sizeof(v_) - 1U)}}}
#define OPT_ENUM(v_) {SAG_OPT_ENUM, {.str = {(v_), (u32)(sizeof(v_) - 1U)}}}

static void option_changed(Ed *ed, const OptDesc *desc,
                           const OptVal *old, const OptVal *nu);

const OptDesc sag_opts[] = {
    {"tabwidth", SAG_OPT_INT, SAG_OPT_BUFFER, OPT_INT(4), NULL, 1, 16,
     NULL, option_changed, "Indent and tab display width (1..16)"},
    {"expandtab", SAG_OPT_BOOL, SAG_OPT_BUFFER, OPT_BOOL(false), NULL, 0, 0,
     NULL, option_changed, "Insert spaces when indentation emits a tab"},
    {"wrap", SAG_OPT_BOOL, SAG_OPT_WINDOW, OPT_BOOL(false), NULL, 0, 0,
     NULL, option_changed, "Wrap long lines in this window"},
    {"scrolloff", SAG_OPT_INT, SAG_OPT_WINDOW, OPT_INT(3), NULL, 0, 99,
     NULL, option_changed, "Minimum screen rows around the cursor"},
    {"number", SAG_OPT_ENUM, SAG_OPT_WINDOW, OPT_ENUM("abs"), number_values,
     0, 0, NULL, option_changed, "Line numbers: off, abs, rel, or both"},
    {"statusline.column", SAG_OPT_ENUM, SAG_OPT_GLOBAL, OPT_ENUM("gcol"),
     status_column_values, 0, 0, NULL, option_changed,
     "Show grapheme column or grapheme and cell columns"},
    {"errorbells", SAG_OPT_BOOL, SAG_OPT_GLOBAL, OPT_BOOL(false), NULL, 0, 0,
     NULL, option_changed, "Ring the terminal bell for editor errors"},
    {"ambiguous_wide", SAG_OPT_BOOL, SAG_OPT_GLOBAL, OPT_BOOL(false), NULL,
     0, 0, NULL, option_changed,
     "Render East Asian ambiguous characters as two cells"},
    {"subword", SAG_OPT_BOOL, SAG_OPT_BUFFER, OPT_BOOL(false), NULL, 0, 0,
     NULL, option_changed, "Use subword boundaries for word navigation"},
    {"chord_timeout_ms", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT(SAG_CHORD_TIMEOUT_DEFAULT_MS), NULL, 0, 5000, NULL,
     option_changed, "Milliseconds to wait for a key chord"},
    {"undo.break_on_newline", SAG_OPT_BOOL, SAG_OPT_BUFFER, OPT_BOOL(true),
     NULL, 0, 0, NULL, option_changed,
     "End an insert undo group at a newline"},
    {"undo.bytes_max", SAG_OPT_INT, SAG_OPT_BUFFER,
     OPT_INT((i64)SAG_UNDO_BYTES_MAX), NULL, 1, INT64_MAX, NULL,
     option_changed, "Maximum in-memory undo bytes per buffer"},
    {"undo.min_nodes", SAG_OPT_INT, SAG_OPT_BUFFER,
     OPT_INT((i64)SAG_UNDO_MIN_NODES), NULL, 0, INT64_MAX, NULL,
     option_changed, "Minimum undo nodes retained per buffer"},
    {"undo.persist_bytes_max", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT((i64)SAG_UNDO_PERSIST_BYTES_MAX), NULL, 1, INT64_MAX, NULL,
     option_changed, "Maximum persisted undo bytes"},
    {"registers.ring_depth", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT((i64)SAG_KILL_RING_DEPTH_DEFAULT), NULL, 0, SAG_KILL_RING_MAX,
     NULL, option_changed, "Number of entries retained in the kill ring"},
    {"registers.ring_bytes_max", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT((i64)SAG_KILL_RING_BYTES_DEFAULT), NULL, 1, INT64_MAX, NULL,
     option_changed, "Maximum bytes retained in the kill ring"},
    {"registers.clip_read_max", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT(INT64_C(64) * 1024 * 1024), NULL, 1, INT64_MAX, NULL,
     option_changed, "Maximum bytes read from the system clipboard"},
    {"clipboard.sync", SAG_OPT_ENUM, SAG_OPT_GLOBAL, OPT_ENUM("yank"),
     clipboard_values, 0, 0, NULL, option_changed,
     "System clipboard synchronization policy"},
    {"search.ignorecase", SAG_OPT_BOOL, SAG_OPT_GLOBAL, OPT_BOOL(false),
     NULL, 0, 0, NULL, option_changed, "Ignore case in searches"},
    {"search.smartcase", SAG_OPT_BOOL, SAG_OPT_GLOBAL, OPT_BOOL(true), NULL,
     0, 0, NULL, option_changed,
     "Restore case sensitivity when a pattern has uppercase literals"},
    {"hooks.error_limit", SAG_OPT_INT, SAG_OPT_GLOBAL,
     OPT_INT(SAG_HOOK_ERROR_LIMIT_DEFAULT), NULL, 1, 100, NULL,
     option_changed, "Disable a failing hook after this many errors"},
    {"theme", SAG_OPT_STR, SAG_OPT_GLOBAL, OPT_STR("quiver-dark"), NULL,
     0, 0, NULL, option_changed, "Active theme name"},
    {"macro.dir", SAG_OPT_STR, SAG_OPT_GLOBAL, OPT_STR(""), NULL,
     0, 0, NULL, option_changed, "Macro library directory (Sprint 38)"}
};

const u32 sag_opts_len = (u32)SAG_ARRAY_LEN(sag_opts);

#undef OPT_BOOL
#undef OPT_INT
#undef OPT_STR
#undef OPT_ENUM

static bool name_is(const char *name, u32 len, const char *want)
{
    size_t n = strlen(want);

    return name != NULL && n == (size_t)len && memcmp(name, want, n) == 0;
}

static u32 desc_index(const OptDesc *desc)
{
    return (u32)(desc - sag_opts);
}

const OptDesc *sag_opt_desc(const char *name, u32 len)
{
    u32 i;

    if (name == NULL)
        return NULL;
    for (i = 0U; i < sag_opts_len; i++)
        if (name_is(name, len, sag_opts[i].name))
            return &sag_opts[i];
    return NULL;
}

static void stored_clear(struct OptStored *stored)
{
    if (stored == NULL)
        return;
    free(stored->owned);
    stored->owned = NULL;
    (void)memset(&stored->value, 0, sizeof(stored->value));
}

static bool stored_assign(struct OptStored *stored, const OptVal *value)
{
    char *owned = NULL;
    OptVal copy = *value;

    if (value->type == (u8)SAG_OPT_STR ||
        value->type == (u8)SAG_OPT_ENUM) {
        if (value->as.str.s == NULL && value->as.str.len != 0U)
            return false;
        owned = sag_xmalloc((size_t)value->as.str.len + 1U);
        if (value->as.str.len != 0U)
            (void)memcpy(owned, value->as.str.s, value->as.str.len);
        owned[value->as.str.len] = '\0';
        copy.as.str.s = owned;
    }
    stored_clear(stored);
    stored->value = copy;
    stored->owned = owned;
    return true;
}

void sag_opt_scope_free(Strmap *map)
{
    StrmapIter it;
    void *value;

    if (map == NULL)
        return;
    it = strmap_iter(map);
    while (strmap_iter_next(&it, NULL, NULL, &value)) {
        struct OptStored *stored = value;

        stored_clear(stored);
        free(stored);
    }
    strmap_free(map);
}

void sag_opt_scope_clone(Strmap *dst, const Strmap *src)
{
    StrmapIter it;
    const char *key;
    size_t key_len;
    void *value;

    if (dst == NULL || src == NULL)
        return;
    sag_opt_scope_free(dst);
    it = strmap_iter(src);
    while (strmap_iter_next(&it, &key, &key_len, &value)) {
        const struct OptStored *from = value;
        struct OptStored *to = sag_xcalloc(1U, sizeof(*to));

        if (!stored_assign(to, &from->value))
            SAG_BUG("option clone: invalid stored string");
        (void)strmap_put(dst, key, key_len, to);
    }
}

static struct OptStored *scope_stored(const Strmap *map,
                                      const char *name, u32 len)
{
    return map == NULL ? NULL : strmap_get(map, name, (size_t)len);
}

static bool value_string_is(const OptVal *value, const char *want)
{
    size_t n = strlen(want);

    return value != NULL &&
           (value->type == (u8)SAG_OPT_STR ||
            value->type == (u8)SAG_OPT_ENUM) &&
           value->as.str.s != NULL && value->as.str.len == (u32)n &&
           memcmp(value->as.str.s, want, n) == 0;
}

static bool enum_contains(const OptDesc *desc, const OptVal *value)
{
    u32 i;

    if (desc->enums == NULL)
        return false;
    for (i = 0U; desc->enums[i] != NULL; i++)
        if (value_string_is(value, desc->enums[i]))
            return true;
    return false;
}

static bool value_validate(const OptDesc *desc, const OptVal *value,
                           OptVal *normalized, const char **err)
{
    *normalized = *value;
    if (desc->type == (u8)SAG_OPT_ENUM && value->type == (u8)SAG_OPT_STR)
        normalized->type = SAG_OPT_ENUM;
    if (normalized->type != desc->type) {
        *err = desc->type == (u8)SAG_OPT_BOOL ? "option requires a bool" :
               desc->type == (u8)SAG_OPT_INT ? "option requires an int" :
               desc->type == (u8)SAG_OPT_STR ? "option requires a string" :
                                               "option requires an enum value";
        return false;
    }
    if (desc->type == (u8)SAG_OPT_INT &&
        (normalized->as.i < desc->imin || normalized->as.i > desc->imax)) {
        *err = "option value is outside its allowed range";
        return false;
    }
    if (desc->type == (u8)SAG_OPT_ENUM && !enum_contains(desc, normalized)) {
        *err = "option value is not one of its allowed enum values";
        return false;
    }
    if ((desc->type == (u8)SAG_OPT_STR ||
         desc->type == (u8)SAG_OPT_ENUM) &&
        normalized->as.str.s == NULL && normalized->as.str.len != 0U) {
        *err = "option string is missing its bytes";
        return false;
    }
    return desc->validate == NULL || desc->validate(normalized, err);
}

static Buffer *current_buffer(Ed *ed)
{
    return ed == NULL ? NULL : sag_ed_doc(ed);
}

static void invalidate_buffer_views(Ed *ed, Buffer *buffer)
{
    u32 i;

    if (ed == NULL || buffer == NULL)
        return;
    for (i = 0U; i < ed->tabs.v.len; i++) {
        Pane *leaves[SAG_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 k;

        sag_pane_collect_leaves(ed->tabs.v.data[i].root, leaves,
                                SAG_ARRAY_LEN(leaves), &n);
        for (k = 0U; k < n; k++) {
            Win *win = leaves[k]->win;

            if (win != NULL && win->buf == buffer)
                sag_vp_invalidate(win);
        }
    }
}

static void each_undo_set_persist(Ed *ed, u64 bytes)
{
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        UndoTree *undo = ed->ws.bufs[i] == NULL ? NULL : ed->ws.bufs[i]->undo;

        if (undo != NULL)
            sag_undo_set_limits(undo, undo->bytes_max, undo->min_nodes,
                                bytes);
    }
}

static void option_changed(Ed *ed, const OptDesc *desc,
                           const OptVal *old, const OptVal *nu)
{
    Buffer *buffer = current_buffer(ed);
    Win *win = ed == NULL ? NULL : ed->win;

    (void)old;
    if (ed == NULL || desc == NULL || nu == NULL)
        return;
    if (strcmp(desc->name, "tabwidth") == 0 && buffer != NULL) {
        buffer->tabwidth = (u32)nu->as.i;
        invalidate_buffer_views(ed, buffer);
    } else if (strcmp(desc->name, "wrap") == 0 && win != NULL) {
        win->vp.wrap = nu->as.b;
        win->wrap_goal_valid = false;
        sag_vp_invalidate(win);
    } else if (strcmp(desc->name, "scrolloff") == 0 && win != NULL) {
        win->vp.scrolloff = (u8)nu->as.i;
        sag_vp_follow(win);
    } else if (strcmp(desc->name, "number") == 0 && win != NULL) {
        win->number_style = value_string_is(nu, "off") ? SAG_NUM_NONE :
                            value_string_is(nu, "rel") ? SAG_NUM_REL :
                            value_string_is(nu, "both") ? SAG_NUM_HYBRID :
                                                          SAG_NUM_ABS;
    } else if (strcmp(desc->name, "errorbells") == 0) {
        ed->errorbells = nu->as.b;
    } else if (strcmp(desc->name, "ambiguous_wide") == 0) {
        SagWidthOpts opts = {nu->as.b};

        ed->ambiguous_wide = nu->as.b;
        sag_width_set_opts(&opts);
    } else if (strcmp(desc->name, "chord_timeout_ms") == 0) {
        ed->chord_timeout_ms = (u32)nu->as.i;
        if (ed->chord.n != 0U)
            ed->chord.deadline = ed->now_ms + nu->as.i;
    } else if (strcmp(desc->name, "undo.break_on_newline") == 0) {
        ed->undo_break_on_newline = nu->as.b;
    } else if (strcmp(desc->name, "undo.bytes_max") == 0 &&
               buffer != NULL && buffer->undo != NULL) {
        sag_undo_set_limits(buffer->undo, (u64)nu->as.i,
                            buffer->undo->min_nodes,
                            buffer->undo->persist_bytes_max);
    } else if (strcmp(desc->name, "undo.min_nodes") == 0 &&
               buffer != NULL && buffer->undo != NULL) {
        sag_undo_set_limits(buffer->undo, buffer->undo->bytes_max,
                            (u32)nu->as.i,
                            buffer->undo->persist_bytes_max);
    } else if (strcmp(desc->name, "undo.persist_bytes_max") == 0) {
        each_undo_set_persist(ed, (u64)nu->as.i);
    } else if (strcmp(desc->name, "registers.ring_depth") == 0) {
        sag_reg_ring_set_depth(&ed->regs, (u32)nu->as.i);
    } else if (strcmp(desc->name, "registers.ring_bytes_max") == 0) {
        ed->regs.ring_bytes_max = (u64)nu->as.i;
    } else if (strcmp(desc->name, "registers.clip_read_max") == 0) {
        ed->regs.clip_read_max = (u64)nu->as.i;
    } else if (strcmp(desc->name, "clipboard.sync") == 0) {
        ed->regs.clipboard_sync = value_string_is(nu, "off") ?
                                  (u8)SAG_CLIP_SYNC_OFF :
                                  value_string_is(nu, "all") ?
                                  (u8)SAG_CLIP_SYNC_ALL :
                                  value_string_is(nu, "unnamed") ?
                                  (u8)SAG_CLIP_SYNC_UNNAMED :
                                  (u8)SAG_CLIP_SYNC_YANK;
    } else if (strcmp(desc->name, "search.ignorecase") == 0) {
        ed->search_opts.ignorecase = nu->as.b;
    } else if (strcmp(desc->name, "search.smartcase") == 0) {
        ed->search_opts.smartcase = nu->as.b;
    } else if (strcmp(desc->name, "hooks.error_limit") == 0) {
        fl_hook_error_limit(&ed->hooks, (u32)nu->as.i);
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
}

void sag_opt_init(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    ed->opt_globals = sag_xcalloc(sag_opts_len, sizeof(*ed->opt_globals));
    ed->opt_inflight = sag_xcalloc(sag_opts_len, sizeof(*ed->opt_inflight));
    for (i = 0U; i < sag_opts_len; i++)
        if (!stored_assign(&ed->opt_globals[i], &sag_opts[i].dflt))
            SAG_BUG("option default has an invalid string");
    for (i = 0U; i < sag_opts_len; i++)
        if (sag_opts[i].on_change != NULL)
            sag_opts[i].on_change(ed, &sag_opts[i], &sag_opts[i].dflt,
                                  &ed->opt_globals[i].value);
}

void sag_opt_free(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < sag_opts_len; i++)
        stored_clear(&ed->opt_globals[i]);
    free(ed->opt_globals);
    free(ed->opt_inflight);
    ed->opt_globals = NULL;
    ed->opt_inflight = NULL;
}

static void reset_map(Strmap *map)
{
    sag_opt_scope_free(map);
    strmap_init(map);
}

static void reset_tree_windows(Pane *root)
{
    Pane *leaves[SAG_PANE_MAX_LEAVES];
    u32 n = 0U;
    u32 i;

    if (root == NULL)
        return;
    sag_pane_collect_leaves(root, leaves, SAG_ARRAY_LEN(leaves), &n);
    for (i = 0U; i < n; i++)
        if (leaves[i]->win != NULL)
            reset_map(&leaves[i]->win->opt_overrides);
}

void sag_opt_reset(Ed *ed)
{
    u32 i;

    if (ed == NULL || ed->opt_globals == NULL)
        return;
    for (i = 0U; i < ed->ws.nbufs; i++)
        if (ed->ws.bufs[i] != NULL)
            reset_map(&ed->ws.bufs[i]->opt_overrides);
    for (i = 0U; i < ed->tabs.v.len; i++)
        reset_tree_windows(ed->tabs.v.data[i].root);
    for (i = 0U; i < sag_opts_len; i++) {
        struct OptStored old = {0};

        if (!stored_assign(&old, &ed->opt_globals[i].value))
            SAG_BUG("option reset could not retain the old value");
        if (!stored_assign(&ed->opt_globals[i], &sag_opts[i].dflt))
            SAG_BUG("option reset has an invalid string");
        option_changed(ed, &sag_opts[i], &old.value,
                       &ed->opt_globals[i].value);
        stored_clear(&old);
    }
}

bool sag_opt_get(Ed *ed, Buffer *buffer, Win *win,
                 const char *name, u32 len, OptVal *out)
{
    const OptDesc *desc;
    struct OptStored *stored;

    if (ed == NULL || out == NULL || ed->opt_globals == NULL)
        return false;
    desc = sag_opt_desc(name, len);
    if (desc == NULL)
        return false;
    stored = win == NULL ? NULL : scope_stored(&win->opt_overrides,
                                               name, len);
    if (stored == NULL)
        stored = buffer == NULL ? NULL :
                 scope_stored(&buffer->opt_overrides, name, len);
    if (stored == NULL)
        stored = &ed->opt_globals[desc_index(desc)];
    *out = stored->value;
    return true;
}

static struct OptStored *set_target(Ed *ed, const OptDesc *desc,
                                    const char *name, u32 len,
                                    const char **err)
{
    Strmap *map;
    struct OptStored *stored;

    if (desc->scope == (u8)SAG_OPT_GLOBAL)
        return &ed->opt_globals[desc_index(desc)];
    if (desc->scope == (u8)SAG_OPT_BUFFER) {
        Buffer *buffer = current_buffer(ed);

        if (buffer == NULL) {
            *err = "no current buffer";
            return NULL;
        }
        map = &buffer->opt_overrides;
    } else {
        if (ed->win == NULL) {
            *err = "no current window";
            return NULL;
        }
        map = &ed->win->opt_overrides;
    }
    stored = scope_stored(map, name, len);
    if (stored == NULL) {
        stored = sag_xcalloc(1U, sizeof(*stored));
        (void)strmap_put(map, name, len, stored);
    }
    return stored;
}

bool sag_opt_validate(Ed *ed, u8 scope_hint, const char *name, u32 len,
                      const OptVal *value, const char **err)
{
    const OptDesc *desc;
    OptVal normalized;

    if (err != NULL)
        *err = NULL;
    if (ed == NULL || name == NULL || value == NULL || err == NULL ||
        ed->opt_globals == NULL)
        return false;
    desc = sag_opt_desc(name, len);
    if (desc == NULL) {
        *err = "unknown option";
        return false;
    }
    if (scope_hint != (u8)SAG_OPT_SCOPE_DECLARED &&
        scope_hint != desc->scope) {
        *err = desc->scope == (u8)SAG_OPT_GLOBAL ?
               "global option refuses a buffer or window scope" :
               "option refuses the requested scope";
        return false;
    }
    if (desc->scope == (u8)SAG_OPT_BUFFER && current_buffer(ed) == NULL) {
        *err = "no current buffer";
        return false;
    }
    if (desc->scope == (u8)SAG_OPT_WINDOW && ed->win == NULL) {
        *err = "no current window";
        return false;
    }
    return value_validate(desc, value, &normalized, err);
}

bool sag_opt_set(Ed *ed, u8 scope_hint, const char *name, u32 len,
                 const OptVal *value, const char **err)
{
    const OptDesc *desc;
    struct OptStored *target;
    OptVal normalized;
    OptVal resolved_old;
    struct OptStored old = {0};
    u32 index;

    if (err != NULL)
        *err = NULL;
    if (!sag_opt_validate(ed, scope_hint, name, len, value, err))
        return false;
    desc = sag_opt_desc(name, len);
    normalized = *value;
    if (desc->type == (u8)SAG_OPT_ENUM &&
        normalized.type == (u8)SAG_OPT_STR)
        normalized.type = SAG_OPT_ENUM;
    if (!value_validate(desc, value, &normalized, err))
        return false;
    index = desc_index(desc);
    if (ed->opt_inflight[index]) {
        *err = "option is already being changed";
        return false;
    }
    if (!sag_opt_get(ed, current_buffer(ed), ed->win, name, len,
                     &resolved_old) || !stored_assign(&old, &resolved_old)) {
        *err = "could not retain the old option value";
        return false;
    }
    target = set_target(ed, desc, name, len, err);
    if (target == NULL)
        goto fail_old;
    if (!stored_assign(target, &normalized)) {
        *err = "option string is missing its bytes";
        goto fail_old;
    }
    ed->opt_inflight[index] = true;
    if (desc->on_change != NULL)
        desc->on_change(ed, desc, &old.value, &target->value);
    ed->opt_inflight[index] = false;
    stored_clear(&old);
    return true;

fail_old:
    stored_clear(&old);
    return false;
}

u32 sag_opt_list(const char **out, u32 max)
{
    u32 n = sag_opts_len;
    u32 i;

    if (out == NULL)
        return n;
    if (max < n)
        n = max;
    for (i = 0U; i < n; i++)
        out[i] = sag_opts[i].name;
    return n;
}

static bool builtin_get(Ed *ed, const char *name, u32 len, OptVal *out)
{
    return sag_opt_get(ed, current_buffer(ed), ed == NULL ? NULL : ed->win,
                       name, len, out);
}

static bool builtin_set(Ed *ed, const char *name, u32 len,
                        const OptVal *value, const char **err)
{
    return sag_opt_set(ed, SAG_OPT_SCOPE_DECLARED, name, len, value, err);
}

static u32 builtin_list(Ed *ed, const char **out, u32 max)
{
    (void)ed;
    return sag_opt_list(out, max);
}

static const OptProvider builtin_provider = {
    builtin_get, builtin_set, builtin_list
};

void sag_opt_provider_set(Ed *ed, const OptProvider *provider)
{
    if (ed != NULL)
        ed->opt_provider = provider == NULL ? &builtin_provider : provider;
}

const OptProvider *sag_opt_provider(const Ed *ed)
{
    return ed == NULL || ed->opt_provider == NULL ? &builtin_provider :
                                                    ed->opt_provider;
}

CmdStatus sag_opt_cmd_get(CmdCtx *cx)
{
    const OptProvider *provider;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->opt_out == NULL)
        return SAG_CMD_ERR_ARG;
    provider = sag_opt_provider(cx->ed);
    if (!provider->get(cx->ed, cx->sarg, cx->sarg_len, cx->opt_out)) {
        cx->opt_error = SAG_OPT_ERROR_NAME;
        return SAG_CMD_ERR_ARG;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_opt_cmd_set(CmdCtx *cx)
{
    const OptProvider *provider;
    const char *err = NULL;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->opt_in == NULL)
        return SAG_CMD_ERR_ARG;
    provider = sag_opt_provider(cx->ed);
    if (!provider->set(cx->ed, cx->sarg, cx->sarg_len, cx->opt_in, &err)) {
        cx->opt_error = err != NULL && strcmp(err, "unknown option") == 0 ?
                        SAG_OPT_ERROR_NAME : SAG_OPT_ERROR_TYPE;
        cx->opt_error_msg = err;
        return SAG_CMD_ERR_ARG;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_fl_cmd_eval(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    return sag_fl_eval(cx->ed, cx->sarg, cx->sarg_len);
}
