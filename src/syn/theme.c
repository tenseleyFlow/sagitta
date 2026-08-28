#define _POSIX_C_SOURCE 200809L

#include "syn/theme.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fl/ast.h"
#include "fl/parse.h"
#include "syn/defs.h"
#include "term/render.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/xdg.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

enum { THEME_MAX_BYTES = 1024U * 1024U, THEME_MAX_DIAGS = 64U };

typedef struct ColorSpec {
    YewColor color;
    FlSpan sp;
    bool set;
} ColorSpec;

typedef struct StyleSpec {
    ColorSpec fg;
    ColorSpec bg;
    i16 c256;
    i16 c16;
    u16 attrs;
    u16 mono;
    u8 ul;
    bool mono_set;
    bool present;
} StyleSpec;

typedef struct PaletteEnt {
    const char *name;
    size_t name_len;
    FlNode *value;
    YewColor color;
    u8 state;
} PaletteEnt;

typedef struct ThemeCompile {
    Arena arena;
    Interner in;
    DiagCtx *dc;
    u32 file_id;
    u32 errors;
    PaletteEnt *palette;
    u32 npalette;
} ThemeCompile;

static const u8 attr_parent[YEW_ATTR__COUNT] = {
    [YEW_ATTR_TEXT] = YEW_ATTR_TEXT,
    [YEW_ATTR_KEYWORD] = YEW_ATTR_TEXT,
    [YEW_ATTR_KEYWORD_CONTROL] = YEW_ATTR_KEYWORD,
    [YEW_ATTR_KEYWORD_OP] = YEW_ATTR_KEYWORD,
    [YEW_ATTR_KEYWORD_STORAGE] = YEW_ATTR_KEYWORD,
    [YEW_ATTR_TYPE] = YEW_ATTR_TEXT,
    [YEW_ATTR_TYPE_BUILTIN] = YEW_ATTR_TYPE,
    [YEW_ATTR_CONSTANT] = YEW_ATTR_TEXT,
    [YEW_ATTR_CONSTANT_BUILTIN] = YEW_ATTR_CONSTANT,
    [YEW_ATTR_NUMBER] = YEW_ATTR_CONSTANT,
    [YEW_ATTR_BOOLEAN] = YEW_ATTR_CONSTANT,
    [YEW_ATTR_CHARACTER] = YEW_ATTR_STRING,
    [YEW_ATTR_STRING] = YEW_ATTR_TEXT,
    [YEW_ATTR_STRING_ESCAPE] = YEW_ATTR_STRING,
    [YEW_ATTR_STRING_INTERP] = YEW_ATTR_STRING,
    [YEW_ATTR_STRING_SPECIAL] = YEW_ATTR_STRING,
    [YEW_ATTR_COMMENT] = YEW_ATTR_TEXT,
    [YEW_ATTR_COMMENT_DOC] = YEW_ATTR_COMMENT,
    [YEW_ATTR_COMMENT_TODO] = YEW_ATTR_COMMENT,
    [YEW_ATTR_FUNCTION] = YEW_ATTR_TEXT,
    [YEW_ATTR_FUNCTION_BUILTIN] = YEW_ATTR_FUNCTION,
    [YEW_ATTR_FUNCTION_MACRO] = YEW_ATTR_FUNCTION,
    [YEW_ATTR_METHOD] = YEW_ATTR_FUNCTION,
    [YEW_ATTR_VARIABLE] = YEW_ATTR_TEXT,
    [YEW_ATTR_VARIABLE_BUILTIN] = YEW_ATTR_VARIABLE,
    [YEW_ATTR_VARIABLE_PARAM] = YEW_ATTR_VARIABLE,
    [YEW_ATTR_VARIABLE_MEMBER] = YEW_ATTR_VARIABLE,
    [YEW_ATTR_NAMESPACE] = YEW_ATTR_TEXT,
    [YEW_ATTR_LABEL] = YEW_ATTR_TEXT,
    [YEW_ATTR_ATTRIBUTE] = YEW_ATTR_TEXT,
    [YEW_ATTR_PREPROC] = YEW_ATTR_TEXT,
    [YEW_ATTR_OPERATOR] = YEW_ATTR_TEXT,
    [YEW_ATTR_PUNCT] = YEW_ATTR_TEXT,
    [YEW_ATTR_PUNCT_BRACKET] = YEW_ATTR_PUNCT,
    [YEW_ATTR_PUNCT_DELIM] = YEW_ATTR_PUNCT,
    [YEW_ATTR_TAG] = YEW_ATTR_TEXT,
    [YEW_ATTR_TAG_ATTR] = YEW_ATTR_TAG,
    [YEW_ATTR_HEADING] = YEW_ATTR_TEXT,
    [YEW_ATTR_LINK] = YEW_ATTR_TEXT,
    [YEW_ATTR_EMPHASIS] = YEW_ATTR_TEXT,
    [YEW_ATTR_STRONG] = YEW_ATTR_TEXT,
    [YEW_ATTR_CODE] = YEW_ATTR_TEXT,
    [YEW_ATTR_LIST] = YEW_ATTR_TEXT,
    [YEW_ATTR_QUOTE] = YEW_ATTR_TEXT,
    [YEW_ATTR_DIFF_ADD] = YEW_ATTR_TEXT,
    [YEW_ATTR_DIFF_DEL] = YEW_ATTR_TEXT,
    [YEW_ATTR_ERROR] = YEW_ATTR_TEXT,
    [YEW_ATTR_WARNING] = YEW_ATTR_TEXT,
    [YEW_ATTR_WHITESPACE_SPECIAL] = YEW_ATTR_TEXT,
    [YEW_ATTR_MOTION_UNIT] = YEW_ATTR_KEYWORD,
    [YEW_ATTR_MOTION_ARROW] = YEW_ATTR_OPERATOR,
    [YEW_ATTR_MOTION_COUNT] = YEW_ATTR_NUMBER,
    [YEW_ATTR_MOTION_CMD] = YEW_ATTR_FUNCTION,
    [YEW_ATTR_UI_INVISIBLE] = YEW_ATTR_TEXT
};

u8 yew_syn_attr_parent(u8 attr)
{
    return attr < YEW_ATTR__COUNT ? attr_parent[attr] : YEW_ATTR_TEXT;
}

static const YewColor color_default = {YEW_COLOR_DEFAULT, 0U, 0U, 0U};

static bool node_is(const FlNode *node, FlAstKind kind)
{
    return node != NULL && node->kind == (u8)kind;
}

static bool lit_is(const FlNode *node, FlLitKind kind)
{
    return node_is(node, FL_A_LIT) && node->as.lit.lit == (u8)kind;
}

static const char *node_string(const ThemeCompile *c, const FlNode *node,
                               size_t *n)
{
    if (!lit_is(node, FL_L_STR))
        return NULL;
    if (n != NULL)
        *n = yew_intern_len(&c->in, node->as.lit.v.str_id);
    return yew_intern_str(&c->in, node->as.lit.v.str_id);
}

static bool text_is(const char *text, size_t n, const char *want)
{
    return strlen(want) == n && (n == 0U || memcmp(text, want, n) == 0);
}

static void theme_diag(ThemeCompile *c, FlSpan sp, const char *fmt, ...)
{
    va_list ap;

    if (c->errors >= THEME_MAX_DIAGS)
        return;
    c->errors++;
    va_start(ap, fmt);
    fl_diag_vemit(c->dc, FL_DIAG_ERROR, sp, fmt, ap);
    va_end(ap);
}

static const char *key_string(ThemeCompile *c, FlNode *node, size_t *n)
{
    const char *key = node_string(c, node, n);

    if (key == NULL)
        theme_diag(c, node->sp, "theme map key must be a string or name");
    return key;
}

static FlNode *map_find(ThemeCompile *c, FlNode *map, const char *want)
{
    u32 i;

    if (!node_is(map, FL_A_MAP))
        return NULL;
    for (i = 0U; i < map->as.map.n; i++) {
        size_t n;
        const char *key = node_string(c, map->as.map.keys[i], &n);

        if (key != NULL && text_is(key, n, want))
            return map->as.map.vals[i];
    }
    return NULL;
}

static bool key_known(const char *key, size_t n,
                      const char *const *known, u32 nknown)
{
    u32 i;

    for (i = 0U; i < nknown; i++) {
        if (text_is(key, n, known[i]))
            return true;
    }
    return false;
}

static void validate_keys(ThemeCompile *c, FlNode *map,
                          const char *const *known, u32 nknown)
{
    u32 i;

    if (!node_is(map, FL_A_MAP))
        return;
    for (i = 0U; i < map->as.map.n; i++) {
        size_t n;
        FlNode *key_node = map->as.map.keys[i];
        const char *key = key_string(c, key_node, &n);

        if (key != NULL && !key_known(key, n, known, nknown))
            theme_diag(c, key_node->sp, "unknown theme key '%.*s'", (int)n,
                       key);
    }
}

static bool require_map(ThemeCompile *c, FlNode *node, const char *what)
{
    if (node_is(node, FL_A_MAP))
        return true;
    theme_diag(c, node == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U} : node->sp,
               "%s must be a map", what);
    return false;
}

static bool require_string(ThemeCompile *c, FlNode *node, const char *what,
                           const char **text, size_t *n)
{
    const char *got = node_string(c, node, n);

    if (got != NULL) {
        *text = got;
        return true;
    }
    theme_diag(c, node == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U} : node->sp,
               "%s must be a string", what);
    return false;
}

static bool require_int(ThemeCompile *c, FlNode *node, const char *what,
                        i64 *value)
{
    if (lit_is(node, FL_L_INT)) {
        *value = node->as.lit.v.i;
        return true;
    }
    theme_diag(c, node == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U} : node->sp,
               "%s must be an integer", what);
    return false;
}

static bool require_bool(ThemeCompile *c, FlNode *node, const char *what,
                         bool *value)
{
    if (lit_is(node, FL_L_BOOL)) {
        *value = node->as.lit.v.b;
        return true;
    }
    theme_diag(c, node == NULL ? (FlSpan){c->file_id, 1U, 1U, 1U} : node->sp,
               "%s must be a boolean", what);
    return false;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex(ThemeCompile *c, FlNode *node, const char *text,
                      size_t n, YewColor *out)
{
    int h[6];
    u32 i;

    if (n != 7U || text[0] != '#') {
        theme_diag(c, node->sp, "invalid theme colour '%.*s'", (int)n, text);
        return false;
    }
    for (i = 0U; i < 6U; i++) {
        h[i] = hex_digit(text[i + 1U]);
        if (h[i] < 0) {
            theme_diag(c, node->sp, "invalid theme colour '%.*s'", (int)n,
                       text);
            return false;
        }
    }
    *out = (YewColor){YEW_COLOR_RGB, (u8)(h[0] * 16 + h[1]),
                      (u8)(h[2] * 16 + h[3]), (u8)(h[4] * 16 + h[5])};
    return true;
}

static PaletteEnt *palette_find(ThemeCompile *c, const char *name, size_t n)
{
    u32 i;

    for (i = 0U; i < c->npalette; i++) {
        if (c->palette[i].name_len == n &&
            memcmp(c->palette[i].name, name, n) == 0)
            return &c->palette[i];
    }
    return NULL;
}

static bool color_value(ThemeCompile *c, FlNode *node, YewColor *out);

static bool palette_resolve(ThemeCompile *c, PaletteEnt *entry,
                            YewColor *out)
{
    if (entry->state == 2U) {
        *out = entry->color;
        return true;
    }
    if (entry->state == 1U) {
        theme_diag(c, entry->value->sp, "palette reference cycle at '%.*s'",
                   (int)entry->name_len, entry->name);
        return false;
    }
    entry->state = 1U;
    if (!color_value(c, entry->value, &entry->color))
        return false;
    entry->state = 2U;
    *out = entry->color;
    return true;
}

static bool color_value(ThemeCompile *c, FlNode *node, YewColor *out)
{
    const char *text;
    size_t n;

    text = node_string(c, node, &n);
    if (text != NULL) {
        if (text_is(text, n, "none")) {
            *out = color_default;
            return true;
        }
        if (n != 0U && text[0] == '@') {
            PaletteEnt *entry = palette_find(c, text + 1U, n - 1U);

            if (entry == NULL) {
                theme_diag(c, node->sp, "unknown palette colour '%.*s'",
                           (int)(n - 1U), text + 1U);
                return false;
            }
            return palette_resolve(c, entry, out);
        }
        return parse_hex(c, node, text, n, out);
    }
    if (node_is(node, FL_A_MAP)) {
        static const char *const keys[] = {"idx"};
        FlNode *idx;
        i64 value;

        validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
        idx = map_find(c, node, "idx");
        if (idx == NULL) {
            theme_diag(c, node->sp, "indexed colour requires 'idx'");
            return false;
        }
        if (!require_int(c, idx, "colour idx", &value))
            return false;
        if (value < 0 || value > 255) {
            theme_diag(c, idx->sp, "colour idx must be between 0 and 255");
            return false;
        }
        *out = (YewColor){YEW_COLOR_INDEXED, (u8)value, 0U, 0U};
        return true;
    }
    theme_diag(c, node->sp,
               "colour must be #rrggbb, @palette, { idx: N }, or none");
    return false;
}

static void parse_palette(ThemeCompile *c, FlNode *node)
{
    u32 i;

    if (!require_map(c, node, "palette"))
        return;
    c->palette = arena_alloc(&c->arena,
                             (size_t)node->as.map.n * sizeof(*c->palette),
                             _Alignof(PaletteEnt));
    (void)memset(c->palette, 0,
                 (size_t)node->as.map.n * sizeof(*c->palette));
    for (i = 0U; i < node->as.map.n; i++) {
        size_t n;
        const char *key = key_string(c, node->as.map.keys[i], &n);
        u32 j;

        if (key == NULL)
            continue;
        for (j = 0U; j < c->npalette; j++) {
            if (c->palette[j].name_len == n &&
                memcmp(c->palette[j].name, key, n) == 0) {
                theme_diag(c, node->as.map.keys[i]->sp,
                           "duplicate palette colour '%.*s'", (int)n, key);
                break;
            }
        }
        if (j != c->npalette)
            continue;
        c->palette[c->npalette++] =
            (PaletteEnt){key, n, node->as.map.vals[i], color_default, 0U};
    }
    for (i = 0U; i < c->npalette; i++) {
        YewColor ignored;

        (void)palette_resolve(c, &c->palette[i], &ignored);
    }
}

static u16 style_bit(const char *key, size_t n)
{
    if (text_is(key, n, "bold")) return YEW_ATTR_BOLD;
    if (text_is(key, n, "italic")) return YEW_ATTR_ITALIC;
    if (text_is(key, n, "underline")) return YEW_ATTR_UNDERLINE;
    if (text_is(key, n, "undercurl")) return YEW_ATTR_UNDERCURL;
    if (text_is(key, n, "strike")) return YEW_ATTR_STRIKE;
    if (text_is(key, n, "dim")) return YEW_ATTR_DIM;
    if (text_is(key, n, "reverse")) return YEW_ATTR_REVERSE;
    return 0U;
}

static bool mono_value(ThemeCompile *c, FlNode *node, u16 *out)
{
    const char *text;
    size_t n;
    u16 bit;

    if (!require_string(c, node, "mono", &text, &n))
        return false;
    if (text_is(text, n, "plain")) {
        *out = 0U;
        return true;
    }
    bit = style_bit(text, n);
    if (bit == 0U || bit == YEW_ATTR_UNDERCURL || bit == YEW_ATTR_STRIKE) {
        theme_diag(c, node->sp,
                   "mono must be plain, bold, italic, underline, reverse, or dim");
        return false;
    }
    *out = bit;
    return true;
}

static void parse_style(ThemeCompile *c, FlNode *node, StyleSpec *style)
{
    static const char *const keys[] = {
        "fg", "bg", "bold", "italic", "underline", "undercurl",
        "strike", "dim", "reverse", "c256", "c16", "ul", "mono"
    };
    u32 i;

    style->c256 = -1;
    style->c16 = -1;
    style->present = true;
    if (!require_map(c, node, "theme style"))
        return;
    validate_keys(c, node, keys, (u32)YEW_ARRAY_LEN(keys));
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *kn = node->as.map.keys[i];
        FlNode *value = node->as.map.vals[i];
        size_t n;
        const char *key = node_string(c, kn, &n);
        u16 bit;

        if (key == NULL)
            continue;
        if (text_is(key, n, "fg") || text_is(key, n, "bg")) {
            ColorSpec *color = text_is(key, n, "fg") ? &style->fg : &style->bg;

            color->sp = value->sp;
            color->set = color_value(c, value, &color->color);
            continue;
        }
        bit = style_bit(key, n);
        if (bit != 0U) {
            bool enabled;

            if (require_bool(c, value, key, &enabled)) {
                if (enabled)
                    style->attrs |= bit;
                else
                    style->attrs &= (u16)~bit;
            }
            continue;
        }
        if (text_is(key, n, "c256") || text_is(key, n, "c16")) {
            i64 index;
            i64 limit = text_is(key, n, "c16") ? 15 : 255;

            if (require_int(c, value, key, &index)) {
                if (index < 0 || index > limit)
                    theme_diag(c, value->sp, "%.*s must be between 0 and %lld",
                               (int)n, key, (long long)limit);
                else if (limit == 15)
                    style->c16 = (i16)index;
                else
                    style->c256 = (i16)index;
            }
            continue;
        }
        if (text_is(key, n, "ul")) {
            const char *text;
            size_t text_n;

            if (require_string(c, value, "ul", &text, &text_n)) {
                if (text_is(text, text_n, "error")) style->ul = 1U;
                else if (text_is(text, text_n, "warn")) style->ul = 2U;
                else if (text_is(text, text_n, "info")) style->ul = 3U;
                else theme_diag(c, value->sp,
                                "ul must be error, warn, or info");
            }
            continue;
        }
        if (text_is(key, n, "mono")) {
            style->mono_set = mono_value(c, value, &style->mono);
            continue;
        }
    }
}

static YewColor color_256(YewColor color)
{
    if (color.tag != YEW_COLOR_RGB)
        return color;
    return (YewColor){YEW_COLOR_INDEXED,
                      yew_rgb_to_256(color.r, color.g, color.b), 0U, 0U};
}

static YewColor color_16(YewColor color)
{
    if (color.tag != YEW_COLOR_RGB)
        return color;
    return (YewColor){YEW_COLOR_INDEXED,
                      yew_rgb_to_16(color.r, color.g, color.b), 0U, 0U};
}

static void compile_style_tabs(ThemeEnt tab[YEW_THEME_RENDITION_COUNT],
                               const StyleSpec *style)
{
    YewColor fg = style->fg.set ? style->fg.color : color_default;
    YewColor bg = style->bg.set ? style->bg.color : color_default;
    u16 attrs = (u16)(style->attrs | ((u16)style->ul << YEW_CELL_UL_SHIFT));
    YewColor fg256 = color_256(fg);
    YewColor fg16 = color_16(fg);

    if (style->c256 >= 0)
        fg256 = (YewColor){YEW_COLOR_INDEXED, (u8)style->c256, 0U, 0U};
    if (style->c16 >= 0)
        fg16 = (YewColor){YEW_COLOR_INDEXED, (u8)style->c16, 0U, 0U};
    tab[YEW_THEME_TRUECOLOR] = (ThemeEnt){fg, bg, attrs};
    tab[YEW_THEME_256] = (ThemeEnt){fg256, color_256(bg), attrs};
    tab[YEW_THEME_16] = (ThemeEnt){fg16, color_16(bg), attrs};
    tab[YEW_THEME_MONO] =
        (ThemeEnt){color_default, color_default,
                   style->mono_set ? style->mono : 0U};
    tab[YEW_THEME_DUMB] = (ThemeEnt){color_default, color_default, 0U};
}

static void compile_style(Theme *theme, u8 attr, const StyleSpec *style)
{
    ThemeEnt tab[YEW_THEME_RENDITION_COUNT];
    u32 rendition;

    compile_style_tabs(tab, style);
    for (rendition = 0U; rendition < YEW_THEME_RENDITION_COUNT; rendition++)
        theme->tab[rendition][attr] = tab[rendition];
}

static void resolve_attrs(Theme *theme, const StyleSpec specs[YEW_ATTR__COUNT])
{
    u32 i;

    for (i = 0U; i < YEW_ATTR__COUNT; i++) {
        if (specs[i].present) {
            compile_style(theme, (u8)i, &specs[i]);
        } else {
            u32 rendition;
            u8 parent = yew_syn_attr_parent((u8)i);

            for (rendition = 0U; rendition < YEW_THEME_RENDITION_COUNT;
                 rendition++)
                theme->tab[rendition][i] = theme->tab[rendition][parent];
        }
    }
}

static void parse_attrs(ThemeCompile *c, FlNode *node, Theme *theme)
{
    StyleSpec specs[YEW_ATTR__COUNT];
    u32 i;

    (void)memset(specs, 0, sizeof(specs));
    if (!require_map(c, node, "attrs"))
        return;
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *kn = node->as.map.keys[i];
        size_t n;
        const char *key = key_string(c, kn, &n);
        u8 attr;

        if (key == NULL)
            continue;
        if (!yew_syn_attr_id(key, n, &attr)) {
            theme_diag(c, kn->sp, "unknown theme attr '%.*s'", (int)n, key);
            continue;
        }
        if (specs[attr].present) {
            theme_diag(c, kn->sp, "duplicate theme attr '%.*s'", (int)n, key);
            continue;
        }
        parse_style(c, node->as.map.vals[i], &specs[attr]);
        theme->explicit_attrs |= UINT64_C(1) << attr;
    }
    resolve_attrs(theme, specs);
}

static char *text_dup(const char *text, size_t n);

static void parse_ui(ThemeCompile *c, FlNode *node, Theme *theme)
{
    FlNode *ul;
    static const char *const ul_keys[] = {"error", "warn", "info"};
    u32 i;

    if (!require_map(c, node, "ui"))
        return;
    if (node->as.map.n != 0U)
        theme->ui = yew_xcalloc(node->as.map.n, sizeof(*theme->ui));
    for (i = 0U; i < node->as.map.n; i++) {
        FlNode *key_node = node->as.map.keys[i];
        FlNode *value = node->as.map.vals[i];
        const char *key;
        size_t key_n;

        key = key_string(c, key_node, &key_n);
        if (key == NULL || text_is(key, key_n, "ul"))
            continue;
        {
            u32 prior;

            for (prior = 0U; prior < theme->nui; prior++) {
                if (strlen(theme->ui[prior].name) == key_n &&
                    memcmp(theme->ui[prior].name, key, key_n) == 0) {
                    theme_diag(c, key_node->sp, "duplicate ui role '%.*s'",
                               (int)key_n, key);
                    break;
                }
            }
            if (prior != theme->nui)
                continue;
        }
        theme->ui[theme->nui].name = text_dup(key, key_n);
        if (text_is(key, key_n, "bg") || text_is(key, key_n, "fg")) {
            StyleSpec style;
            YewColor color;

            (void)memset(&style, 0, sizeof(style));
            style.c256 = -1;
            style.c16 = -1;
            if (color_value(c, value, &color)) {
                if (text_is(key, key_n, "bg")) {
                    style.bg.color = color;
                    style.bg.set = true;
                } else {
                    style.fg.color = color;
                    style.fg.set = true;
                }
                compile_style_tabs(theme->ui[theme->nui].tab, &style);
            }
        } else {
            StyleSpec style;

            (void)memset(&style, 0, sizeof(style));
            parse_style(c, value, &style);
            compile_style_tabs(theme->ui[theme->nui].tab, &style);
        }
        theme->nui++;
    }
    ul = map_find(c, node, "ul");
    if (ul == NULL) {
        theme_diag(c, node->sp, "ui.ul is required");
        return;
    }
    if (!require_map(c, ul, "ui.ul"))
        return;
    validate_keys(c, ul, ul_keys, (u32)YEW_ARRAY_LEN(ul_keys));
    for (i = 0U; i < YEW_THEME_UL_COUNT; i++) {
        FlNode *value = map_find(c, ul, ul_keys[i]);
        YewColor color;

        if (value == NULL) {
            theme_diag(c, ul->sp, "ui.ul.%s is required", ul_keys[i]);
            continue;
        }
        if (!color_value(c, value, &color))
            continue;
        theme->ul[YEW_THEME_TRUECOLOR][i] = color;
        theme->ul[YEW_THEME_256][i] = color_256(color);
        theme->ul[YEW_THEME_16][i] = color_16(color);
    }
}

static char *text_dup(const char *text, size_t n)
{
    char *copy = yew_xmalloc(n + 1U);

    if (n != 0U)
        (void)memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

static bool compile_document(ThemeCompile *c, Theme *out, const u8 *src,
                             size_t n)
{
    static const char *const top_keys[] = {
        "theme", "name", "kind", "palette", "ui", "attrs"
    };
    FlNode *top;
    FlNode *version;
    FlNode *name;
    FlNode *kind;
    FlNode *palette;
    FlNode *ui;
    FlNode *attrs;
    const char *text;
    size_t text_n;
    i64 schema;

    top = fl_parse_literal(&c->arena, c->dc, &c->in, (const char *)src, n,
                           c->file_id);
    if (top == NULL) {
        c->errors++;
        return false;
    }
    if (!require_map(c, top, "theme"))
        return false;
    validate_keys(c, top, top_keys, (u32)YEW_ARRAY_LEN(top_keys));
    version = map_find(c, top, "theme");
    name = map_find(c, top, "name");
    kind = map_find(c, top, "kind");
    palette = map_find(c, top, "palette");
    ui = map_find(c, top, "ui");
    attrs = map_find(c, top, "attrs");
    if (version == NULL)
        theme_diag(c, top->sp, "theme schema version is required");
    else if (require_int(c, version, "theme", &schema) && schema != 1)
        theme_diag(c, version->sp,
                   "unknown theme schema version %lld (this build understands 1)",
                   (long long)schema);
    if (require_string(c, name, "name", &text, &text_n))
        out->name = text_dup(text, text_n);
    if (require_string(c, kind, "kind", &text, &text_n)) {
        if (text_is(text, text_n, "dark")) out->kind = YEW_THEME_DARK;
        else if (text_is(text, text_n, "light")) out->kind = YEW_THEME_LIGHT;
        else theme_diag(c, kind->sp, "kind must be 'dark' or 'light'");
    }
    if (palette != NULL)
        parse_palette(c, palette);
    else
        theme_diag(c, top->sp, "palette is required");
    if (ui != NULL)
        parse_ui(c, ui, out);
    else
        theme_diag(c, top->sp, "ui is required");
    if (attrs != NULL)
        parse_attrs(c, attrs, out);
    else
        theme_diag(c, top->sp, "attrs is required");
    return c->errors == 0U;
}

void yew_theme_init(Theme *theme)
{
    if (theme != NULL)
        (void)memset(theme, 0, sizeof(*theme));
}

void yew_theme_free(Theme *theme)
{
    u32 i;

    if (theme == NULL)
        return;
    for (i = 0U; i < theme->nui; i++)
        yew_xfree(theme->ui[i].name);
    yew_xfree(theme->ui);
    yew_xfree(theme->name);
    (void)memset(theme, 0, sizeof(*theme));
}

bool yew_theme_compile(Theme *theme, const u8 *src, size_t n,
                       const char *path, DiagCtx *dc)
{
    Theme candidate;
    ThemeCompile c;
    bool ok;

    if (theme == NULL || dc == NULL || (src == NULL && n != 0U))
        return false;
    if (n > THEME_MAX_BYTES) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "theme exceeds the 1 MiB limit");
        return false;
    }
    yew_theme_init(&candidate);
    (void)memset(&c, 0, sizeof(c));
    arena_init(&c.arena);
    interner_init(&c.in, &c.arena);
    c.dc = dc;
    c.file_id = fl_diag_add_file(dc, path == NULL ? "<theme>" : path,
                                 (const char *)src, n);
    ok = compile_document(&c, &candidate, src, n);
    interner_free(&c.in);
    arena_free_all(&c.arena);
    if (!ok) {
        yew_theme_free(&candidate);
        return false;
    }
    yew_theme_free(theme);
    *theme = candidate;
    return true;
}

static char *path_join3(const char *a, const char *b, const char *c)
{
    size_t an = strlen(a);
    size_t bn = strlen(b);
    size_t cn = strlen(c);
    size_t n;
    char *path;

    if (an > SIZE_MAX - bn - cn - 3U)
        return NULL;
    n = an + bn + cn + 2U;
    path = yew_xmalloc(n + 1U);
    (void)snprintf(path, n + 1U, "%s/%s/%s", a, b, c);
    return path;
}

static bool valid_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (name == NULL || name[0] == '\0')
        return false;
    for (; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_'))
            return false;
    }
    return true;
}

typedef enum ThemeReadStatus {
    THEME_READ_OK = 0,
    THEME_READ_IO,
    THEME_READ_TOO_LARGE
} ThemeReadStatus;

static ThemeReadStatus read_file(const char *path, Bytebuf *out)
{
    FILE *file = fopen(path, "rb");
    u8 chunk[4096];

    if (file == NULL)
        return THEME_READ_IO;
    bytebuf_init(out);
    while (!feof(file)) {
        size_t n = fread(chunk, 1U, sizeof(chunk), file);

        if (n != 0U)
            bytebuf_append(out, chunk, n);
        if (out->len > THEME_MAX_BYTES) {
            (void)fclose(file);
            bytebuf_free(out);
            return THEME_READ_TOO_LARGE;
        }
        if (ferror(file)) {
            (void)fclose(file);
            bytebuf_free(out);
            return THEME_READ_IO;
        }
    }
    if (fclose(file) != 0) {
        bytebuf_free(out);
        return THEME_READ_IO;
    }
    return THEME_READ_OK;
}

static char *theme_filename(const char *name)
{
    size_t n = strlen(name);
    char *file = yew_xmalloc(n + 4U);

    (void)memcpy(file, name, n);
    (void)memcpy(file + n, ".fl", 4U);
    return file;
}

static char *discover_path(const char *name, const char *runtime_dir)
{
    char *filename = theme_filename(name);
    char *config = yew_xdg_config_dir();
    char *path = NULL;
    const char *runtime = runtime_dir;

    if (config != NULL) {
        path = path_join3(config, "themes", filename);
        yew_xfree(config);
        if (path != NULL && access(path, R_OK) == 0) {
            yew_xfree(filename);
            return path;
        }
        yew_xfree(path);
        path = NULL;
    }
    if (runtime == NULL || runtime[0] == '\0')
        runtime = getenv("YEW_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] != '\0') {
        path = path_join3(runtime, "themes", filename);
        if (path != NULL && access(path, R_OK) == 0) {
            yew_xfree(filename);
            return path;
        }
        yew_xfree(path);
    }
    path = path_join3(YEW_RUNTIME_DIR_DEFAULT, "themes", filename);
    if (path != NULL && access(path, R_OK) == 0) {
        yew_xfree(filename);
        return path;
    }
    yew_xfree(path);
    path = path_join3("runtime", "themes", filename);
    yew_xfree(filename);
    if (path != NULL && access(path, R_OK) == 0)
        return path;
    yew_xfree(path);
    return NULL;
}

bool yew_theme_select(Theme *theme, const char *name,
                      const char *runtime_dir, DiagCtx *dc)
{
    Theme candidate;
    char *path;
    Bytebuf src;
    ThemeReadStatus read_status;
    bool ok;

    if (theme == NULL || dc == NULL)
        return false;
    if (!valid_name(name)) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "invalid theme name '%s'", name == NULL ? "" : name);
        return false;
    }
    path = discover_path(name, runtime_dir);
    if (path == NULL) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "theme '%s' was not found", name);
        return false;
    }
    read_status = read_file(path, &src);
    if (read_status != THEME_READ_OK) {
        if (read_status == THEME_READ_TOO_LARGE) {
            fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                         "theme '%s' exceeds the 1 MiB limit", path);
            yew_xfree(path);
            return false;
        }
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "cannot read theme '%s': %s", path, strerror(errno));
        yew_xfree(path);
        return false;
    }
    yew_theme_init(&candidate);
    ok = yew_theme_compile(&candidate, src.data, src.len, path, dc);
    bytebuf_free(&src);
    yew_xfree(path);
    if (ok && strcmp(candidate.name, name) != 0) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "theme file '%s' declares name '%s'", name,
                     candidate.name);
        yew_theme_free(&candidate);
        return false;
    }
    if (ok) {
        yew_theme_free(theme);
        *theme = candidate;
    } else {
        yew_theme_free(&candidate);
    }
    return ok;
}

const ThemeEnt *yew_theme_table(const Theme *theme,
                                YewThemeRendition rendition)
{
    if (theme == NULL || rendition >= YEW_THEME_RENDITION_COUNT)
        return NULL;
    return theme->tab[rendition];
}

const char *yew_theme_name(const Theme *theme)
{
    return theme == NULL ? NULL : theme->name;
}

YewThemeKind yew_theme_kind(const Theme *theme)
{
    return theme != NULL && theme->kind == YEW_THEME_LIGHT
               ? YEW_THEME_LIGHT : YEW_THEME_DARK;
}

YewColor yew_theme_underline(const Theme *theme,
                             YewThemeRendition rendition,
                             YewThemeUnderline which)
{
    if (theme == NULL || rendition > YEW_THEME_16 ||
        (u32)which >= (u32)YEW_THEME_UL_COUNT)
        return color_default;
    return theme->ul[rendition][which];
}

const ThemeEnt *yew_theme_ui(const Theme *theme, const char *role,
                             YewThemeRendition rendition)
{
    u32 i;

    if (theme == NULL || role == NULL ||
        rendition >= YEW_THEME_RENDITION_COUNT)
        return NULL;
    for (i = 0U; i < theme->nui; i++) {
        if (strcmp(theme->ui[i].name, role) == 0)
            return &theme->ui[i].tab[rendition];
    }
    return NULL;
}

bool yew_theme_attr_explicit(const Theme *theme, SynAttr attr)
{
    return theme != NULL && attr < YEW_ATTR__COUNT &&
           (theme->explicit_attrs & (UINT64_C(1) << attr)) != 0U;
}

YewThemeRendition yew_theme_rendition(u8 render_tier,
                                      const char *(*getv)(const char *))
{
    const char *term = getv == NULL ? NULL : getv("TERM");

    if (term != NULL && strcmp(term, "dumb") == 0)
        return YEW_THEME_DUMB;
    if (getv != NULL && getv("NO_COLOR") != NULL)
        return YEW_THEME_MONO;
    if (render_tier == YEW_RENDER_TIER_TRUECOLOR)
        return YEW_THEME_TRUECOLOR;
    if (render_tier == YEW_RENDER_TIER_256)
        return YEW_THEME_256;
    return YEW_THEME_16;
}
