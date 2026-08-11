#ifndef YEW_SYN_THEME_H
#define YEW_SYN_THEME_H

#include <stdbool.h>
#include <stddef.h>

#include "fl/diag.h"
#include "syn/attr.h"
#include "term/grid.h"

typedef struct Ed Ed;

enum {
    YEW_THEME_TABLE_SIZE = 64,
    YEW_THEME_UL_COUNT = 3
};

typedef enum YewThemeKind {
    YEW_THEME_DARK = 0,
    YEW_THEME_LIGHT
} YewThemeKind;

/* A rendition is selected once per frame, never once per cell. */
typedef enum YewThemeRendition {
    YEW_THEME_TRUECOLOR = 0,
    YEW_THEME_256,
    YEW_THEME_16,
    YEW_THEME_MONO,
    YEW_THEME_DUMB,
    YEW_THEME_RENDITION_COUNT
} YewThemeRendition;

typedef enum YewThemeUnderline {
    YEW_THEME_UL_ERROR = 0,
    YEW_THEME_UL_WARN,
    YEW_THEME_UL_INFO
} YewThemeUnderline;

typedef struct ThemeEnt {
    YewColor fg;
    YewColor bg;
    u16 attrs;
} ThemeEnt;

typedef struct ThemeUiRole {
    ThemeEnt tab[YEW_THEME_RENDITION_COUNT];
    char *name;
} ThemeUiRole;

/*
 * All degradation is compiled here.  The draw path selects one flat table
 * and performs exactly one attr-id lookup per syntax span.
 */
typedef struct Theme {
    ThemeEnt tab[YEW_THEME_RENDITION_COUNT][YEW_THEME_TABLE_SIZE];
    YewColor ul[3][YEW_THEME_UL_COUNT]; /* truecolor, 256, 16 */
    ThemeUiRole *ui;
    char *name;
    u64 explicit_attrs;
    u32 nui;
    u8 kind;
} Theme;

void yew_theme_init(Theme *theme);
void yew_theme_free(Theme *theme);

/* Compile one pure-literal Fletch document.  On error, `theme` is unchanged. */
bool yew_theme_compile(Theme *theme, const u8 *src, size_t n,
                       const char *path, DiagCtx *dc);

/*
 * Discover NAME in $XDG_CONFIG_HOME/yew/themes first, then RUNTIME_DIR/themes.
 * A NULL runtime_dir uses $YEW_RUNTIME_DIR, the installed runtime directory,
 * and finally the repository-local runtime directory.  No disk cache exists.
 */
bool yew_theme_select(Theme *theme, const char *name,
                      const char *runtime_dir, DiagCtx *dc);

const ThemeEnt *yew_theme_table(const Theme *theme,
                                YewThemeRendition rendition);
const char *yew_theme_name(const Theme *theme);
YewThemeKind yew_theme_kind(const Theme *theme);
YewColor yew_theme_underline(const Theme *theme,
                             YewThemeRendition rendition,
                             YewThemeUnderline which);
const ThemeEnt *yew_theme_ui(const Theme *theme, const char *role,
                             YewThemeRendition rendition);
bool yew_theme_attr_explicit(const Theme *theme, SynAttr attr);

/* Editor integration: transactional load and one-index draw-path table. */
bool yew_theme_load(Ed *ed, const char *name, DiagCtx *dc);
const ThemeEnt *yew_theme_tab(const Ed *ed);
const ThemeEnt *yew_theme_ui_tab(const Ed *ed, const char *role);

/* NO_COLOR is presence-based, including an empty value. TERM=dumb is plain. */
YewThemeRendition yew_theme_rendition(u8 render_tier,
                                      const char *(*getv)(const char *));

#endif
