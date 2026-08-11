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
    free(*slot);
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

    if (ed == NULL || yew_theme_name(&ed->theme) == NULL)
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
        if (ui_fg != NULL)
            ed->grid.blank.fg = ui_fg->fg;
        if (ui_bg != NULL)
            ed->grid.blank.bg = ui_bg->bg;
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
    if (!yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, "theme", 5U, &value,
                     &why)) {
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
