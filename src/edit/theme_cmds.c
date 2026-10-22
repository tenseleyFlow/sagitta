#include "edit/theme_cmds.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "fl/diag.h"
#include "syn/theme.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "util/arena.h"
#include "util/log.h"

typedef struct ThemeDiagCapture {
    char *dst;
    size_t cap;
} ThemeDiagCapture;

static void capture_diag(void *ctx, FlDiagLevel level, FlSpan span,
                         const char *msg, const char *rendered)
{
    ThemeDiagCapture *capture = ctx;

    (void)level;
    (void)span;
    (void)rendered;
    if (capture == NULL || capture->dst == NULL || capture->cap == 0U ||
        capture->dst[0] != '\0')
        return;
    (void)snprintf(capture->dst, capture->cap, "%s",
                   msg == NULL ? "theme load failed" : msg);
}

static void replace_name(char **slot, const char *name)
{
    size_t n;
    char *copy;

    if (slot == NULL || name == NULL)
        return;
    n = strlen(name);
    copy = yew_xmalloc(n + 1U);
    (void)memcpy(copy, name, n + 1U);
    yew_xfree(*slot);
    *slot = copy;
}

static YewThemeRendition active_rendition(const Ed *ed)
{
    if (ed == NULL || !ed->render_ready)
        return YEW_THEME_TRUECOLOR;
    if (ed->render.plain)
        return YEW_THEME_DUMB;
    if (ed->render.no_color)
        return YEW_THEME_MONO;
    if (ed->render.tier == YEW_RENDER_TIER_TRUECOLOR)
        return YEW_THEME_TRUECOLOR;
    if (ed->render.tier == YEW_RENDER_TIER_256)
        return YEW_THEME_256;
    return YEW_THEME_16;
}

void yew_theme_sync_surfaces(Ed *ed)
{
    const ThemeEnt *ui_fg;
    const ThemeEnt *ui_bg;

    if (ed == NULL)
        return;
    if (ed->render_ready) {
        yew_render_set_underline_colors(
            &ed->render,
            yew_theme_underline(&ed->theme, YEW_THEME_TRUECOLOR,
                                YEW_THEME_UL_ERROR),
            yew_theme_underline(&ed->theme, YEW_THEME_TRUECOLOR,
                                YEW_THEME_UL_WARN),
            yew_theme_underline(&ed->theme, YEW_THEME_TRUECOLOR,
                                YEW_THEME_UL_INFO));
    }
    if (ed->grid_ready) {
        ui_fg = yew_theme_ui_tab(ed, "fg");
        ui_bg = yew_theme_ui_tab(ed, "bg");
        ed->grid.blank.fg = ui_fg != NULL ? ui_fg->fg : (YewColor){0};
        ed->grid.blank.bg = ui_bg != NULL ? ui_bg->bg : (YewColor){0};
        yew_grid_mark_all(&ed->grid);
    }
    ed->full_damage = true;
}

bool yew_theme_load(Ed *ed, const char *name, DiagCtx *dc)
{
    YewThemeKind kind;
    const char *loaded;

    if (ed == NULL || dc == NULL ||
        !yew_theme_select(&ed->theme, name, NULL, dc))
        return false;
    loaded = yew_theme_name(&ed->theme);
    kind = yew_theme_kind(&ed->theme);
    if (kind == YEW_THEME_LIGHT)
        replace_name(&ed->theme_last_light, loaded);
    else
        replace_name(&ed->theme_last_dark, loaded);
    yew_theme_sync_surfaces(ed);
    return true;
}

const ThemeEnt *yew_theme_tab(const Ed *ed)
{
    static ThemeEnt debug[YEW_THEME_TABLE_SIZE];
    static bool initialized;

    if (!initialized) {
        debug[YEW_ATTR_COMMENT].attrs = YEW_ATTR_DIM;
        initialized = true;
    }
    if (ed == NULL || yew_theme_name(&ed->theme) == NULL)
        return debug;
    return yew_theme_table(&ed->theme, active_rendition(ed));
}

const ThemeEnt *yew_theme_ui_tab(const Ed *ed, const char *role)
{
    if (ed == NULL || yew_theme_name(&ed->theme) == NULL)
        return NULL;
    return yew_theme_ui(&ed->theme, role, active_rendition(ed));
}

ThemeEnt yew_theme_modal_style(const Ed *ed, const char *role)
{
    ThemeEnt style = {0};
    const ThemeEnt *fg = yew_theme_ui_tab(ed, "fg");
    const ThemeEnt *bg = yew_theme_ui_tab(ed, "bg");
    const ThemeEnt *surface = yew_theme_ui_tab(ed, "modal.surface");
    const ThemeEnt *ink = yew_theme_ui_tab(ed, role);

    if (fg != NULL) style.fg = fg->fg;
    if (bg != NULL) style.bg = bg->bg;
    if (surface != NULL) style = *surface;
    if (strcmp(role, "modal.selection") == 0) {
        if (ink != NULL) style = *ink;
        else style.attrs ^= YEW_ATTR_REVERSE;
        /* Two distinct RGB fills can collapse to the same ANSI color. */
        if (surface != NULL && memcmp(&style.bg, &surface->bg,
                                       sizeof(style.bg)) == 0 &&
            active_rendition(ed) != YEW_THEME_DUMB)
            style.attrs |= YEW_ATTR_REVERSE;
    } else if (ink != NULL) {
        if (ink->fg.tag != YEW_COLOR_DEFAULT) style.fg = ink->fg;
        style.attrs = ink->attrs;
    } else if (strcmp(role, "modal.title") == 0 ||
               strcmp(role, "modal.key") == 0) {
        style.attrs |= YEW_ATTR_BOLD;
    }
    return style;
}

typedef struct ThemePicker {
    Theme original;
    ThemeNames names;
    PickItem items[YEW_THEME_NAMES_MAX];
    i32 successful;
    bool valid;
} ThemePicker;

static const PickItem *theme_items(void *ctx, u32 *n)
{
    ThemePicker *p = ctx;

    *n = p->names.n;
    return p->items;
}

static void theme_preview(Ed *ed, void *ctx, i32 payload)
{
    ThemePicker *p = ctx;
    Arena arena;
    DiagCtx dc;
    char error[192] = {0};
    ThemeDiagCapture capture = {error, sizeof(error)};

    p->valid = false;
    if (payload < 0 || (u32)payload >= p->names.n)
        return;
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, capture_diag, &capture);
    /* No option callbacks or dark/light-history writes during preview. */
    if (p->original.name != NULL &&
        strcmp(p->names.names[payload], p->original.name) == 0) {
        yew_theme_copy(&ed->theme, &p->original);
        p->valid = true;
    } else {
        p->valid = yew_theme_select(&ed->theme, p->names.names[payload],
                                     NULL, &dc);
    }
    if (p->valid) {
        p->successful = payload;
        yew_theme_sync_surfaces(ed);
    } else {
        yew_msg(ed, YEW_MSG_ERROR, "%s", error);
    }
    arena_free_all(&arena);
}

static void theme_picker_closed(Ed *ed, void *ctx, bool accepted)
{
    ThemePicker *p = ctx;
    char error[192] = {0};
    bool committed = false;

    if (ed != NULL && accepted && p->valid) {
        /* Publish the already-previewed tables, not a second disk read.
         * A theme changed/deleted since preview must not apply different
         * colors from the ones the user just approved. */
        bool inflight = ed->theme_option_inflight;

        ed->theme_option_inflight = true;
        committed = yew_theme_set(ed, p->names.names[p->successful],
                                   error, sizeof(error));
        ed->theme_option_inflight = inflight;
        if (committed) {
            replace_name(yew_theme_kind(&ed->theme) == YEW_THEME_LIGHT
                             ? &ed->theme_last_light : &ed->theme_last_dark,
                         yew_theme_name(&ed->theme));
            yew_msg(ed, YEW_MSG_INFO, "theme: %s (this session)",
                    yew_theme_name(&ed->theme));
        }
    }
    if (ed != NULL && !committed) {
        yew_theme_free(&ed->theme);
        ed->theme = p->original;
        yew_theme_init(&p->original);
        yew_theme_sync_surfaces(ed);
        if (accepted)
            yew_msg(ed, YEW_MSG_ERROR, "%s", error[0] != '\0' ? error :
                    "theme not applied: selected preview failed");
    }
    yew_theme_free(&p->original);
    yew_xfree(p);
}

CmdStatus yew_theme_cmd_pick(CmdCtx *cx)
{
    ThemePicker *p;
    PickerSpec spec = {0};
    Ed *ed;
    u32 i;
    i32 selected = 0;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    if (!ed->grid_ready || ed->grid.cols < YEW_PICKER_MIN_COLS ||
        ed->grid.rows < YEW_PICKER_MIN_ROWS) {
        yew_msg(ed, YEW_MSG_ERROR, "terminal too small for theme picker");
        return YEW_CMD_ERR_STATE;
    }
    yew_picker_close(ed, false);
    p = yew_xmalloc(sizeof(*p));
    (void)memset(p, 0, sizeof(*p));
    yew_theme_names(&p->names);
    if (p->names.n == 0U) {
        yew_xfree(p);
        yew_msg(ed, YEW_MSG_ERROR, "no themes found in user/runtime themes");
        return YEW_CMD_ERR_STATE;
    }
    p->original = ed->theme;
    yew_theme_init(&ed->theme);
    yew_theme_copy(&ed->theme, &p->original);
    for (i = 0U; i < p->names.n; i++) {
        bool current = p->original.name != NULL &&
                       strcmp(p->names.names[i], p->original.name) == 0;

        p->items[i] = (PickItem){p->names.names[i],
                                 current ? "current" : NULL, (i32)i, 0U};
        if (current) selected = (i32)i;
    }
    spec.title = "Theme preview";
    spec.items = theme_items;
    spec.selected = theme_preview;
    spec.closed = theme_picker_closed;
    spec.space_accept = true;
    spec.no_split = true;
    spec.max_width = 56U;
    spec.max_height = 15U;
    spec.footer = "Enter/Space/click apply | Esc cancel";
    spec.ctx = p;
    if (p->names.truncated)
        yew_msg(ed, YEW_MSG_WARN, "theme list limited to %u names",
                (unsigned)YEW_THEME_NAMES_MAX);
    yew_picker_open(ed, &spec);
    yew_picker_select_payload(ed, selected);
    return YEW_CMD_OK;
}

static bool theme_auto_enabled(Ed *ed)
{
    OptVal value;

    return ed != NULL &&
           yew_opt_get(ed, NULL, NULL, "theme_auto", 10U, &value) &&
           value.type == YEW_OPT_BOOL && value.as.b;
}

static void theme_auto_expire_probe(Tty *tty)
{
    i64 now = yew_now_ms();
    i64 left = yew_tty_probe_deadline(tty, now);

    if (left > 0 && now <= INT64_MAX - left)
        now += left;
    yew_tty_probe_tick(tty, now);
}

static void theme_auto_wait_probe(Tty *tty)
{
    while (!yew_tty_probe_done(tty)) {
        struct pollfd pfd;
        i64 now = yew_now_ms();
        i64 left;
        int timeout;
        int ready;

        yew_tty_probe_tick(tty, now);
        if (yew_tty_probe_done(tty))
            break;
        left = yew_tty_probe_deadline(tty, now);
        timeout = left > INT_MAX ? INT_MAX : (int)left;
        pfd.fd = tty->rfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1U, timeout);
        if (ready > 0 && (pfd.revents & POLLIN) != 0) {
            u8 bytes[4096];
            ssize_t n;

            do {
                n = read(tty->rfd, bytes, sizeof(bytes));
            } while (n < 0 && errno == EINTR);
            if (n > 0) {
                (void)yew_tty_probe_feed(tty, bytes, (size_t)n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            theme_auto_expire_probe(tty);
            break;
        }
        if (ready < 0 && errno == EINTR)
            continue;
        yew_tty_probe_tick(tty, yew_now_ms());
        if (ready < 0)
            theme_auto_expire_probe(tty);
    }
}

bool yew_theme_auto_startup(Ed *ed)
{
    TtyBackground background;
    YewThemeKind wanted;
    const char *name;
    char error[192];

    if (!theme_auto_enabled(ed) ||
        !yew_tty_probe_background_start(&ed->tty, yew_now_ms()))
        return false;
    theme_auto_wait_probe(&ed->tty);
    background = yew_tty_probe_background(&ed->tty);
    if (background == YEW_TTY_BACKGROUND_UNKNOWN)
        return false;
    wanted = background == YEW_TTY_BACKGROUND_LIGHT ? YEW_THEME_LIGHT :
                                                       YEW_THEME_DARK;
    if (yew_theme_name(&ed->theme) != NULL &&
        yew_theme_kind(&ed->theme) == wanted)
        return true;
    name = wanted == YEW_THEME_LIGHT ? ed->theme_last_light :
                                      ed->theme_last_dark;
    if (name == NULL || !yew_theme_set(ed, name, error, sizeof(error))) {
        yew_log(YEW_LOG_WARN, "automatic theme selection failed: %s",
                name == NULL ? "no theme for terminal background" : error);
        return false;
    }
    return true;
}

bool yew_theme_apply(Ed *ed, const char *name, char *error, size_t cap)
{
    Arena arena;
    DiagCtx dc;
    ThemeDiagCapture capture = {error, cap};
    bool ok;

    if (error != NULL && cap != 0U)
        error[0] = '\0';
    arena_init(&arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, capture_diag, &capture);
    ok = yew_theme_load(ed, name, &dc);
    if (!ok && error != NULL && cap != 0U && error[0] == '\0')
        (void)snprintf(error, cap, "theme '%s' could not be loaded",
                       name == NULL ? "" : name);
    arena_free_all(&arena);
    return ok;
}

bool yew_theme_set(Ed *ed, const char *name, char *error, size_t cap)
{
    const OptProvider *provider;
    OptVal value;
    const char *why = NULL;
    size_t len;

    if (ed == NULL || name == NULL)
        return yew_theme_apply(ed, name, error, cap);
    if (ed->opt_globals == NULL)
        return yew_theme_apply(ed, name, error, cap);
    len = strlen(name);
    if (len > UINT32_MAX) {
        if (error != NULL && cap != 0U)
            (void)snprintf(error, cap, "theme name is too long");
        return false;
    }
    value = (OptVal){YEW_OPT_STR, {.str = {name, (u32)len}}};
    provider = yew_opt_provider(ed);
    if (!provider->set(ed, "theme", 5U, &value, &why)) {
        if (error != NULL && cap != 0U)
            (void)snprintf(error, cap, "%s",
                           why == NULL ? "theme could not be set" : why);
        return false;
    }
    return true;
}

CmdStatus yew_theme_cmd_set(CmdCtx *cx)
{
    char name[128];
    char error[192];

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len == 0U || cx->sarg_len >= sizeof(name))
        return YEW_CMD_ERR_ARG;
    (void)memcpy(name, cx->sarg, cx->sarg_len);
    name[cx->sarg_len] = '\0';
    if (!yew_theme_set(cx->ed, name, error, sizeof(error))) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", error);
        return YEW_CMD_ERR_ARG;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "theme: %s",
            yew_theme_name(&cx->ed->theme));
    return YEW_CMD_OK;
}

CmdStatus yew_theme_cmd_toggle(CmdCtx *cx)
{
    const char *name;
    char error[192];

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    name = yew_theme_kind(&cx->ed->theme) == YEW_THEME_LIGHT
               ? cx->ed->theme_last_dark : cx->ed->theme_last_light;
    if (name == NULL || !yew_theme_set(cx->ed, name, error,
                                       sizeof(error))) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s",
                name == NULL ? "no opposite theme has been selected" : error);
        return YEW_CMD_ERR_ARG;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "theme: %s",
            yew_theme_name(&cx->ed->theme));
    return YEW_CMD_OK;
}
