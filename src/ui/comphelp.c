#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.25: learning a command's subcommands and flags from its
 * `--help`.  See comphelp.h for the safety rules; this file is, in order,
 * the parser (§3), the policy (§1), the disk cache (§5), the in-memory
 * trees and the lazy descent (§4), the request queue and its jobs (§2,
 * §6), and `ed.shell.complete_forget`.
 */

#include "ui/comphelp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "fl/data.h"
#include "fl/diag.h"
#include "fl/gc.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "text/file.h"
#include "ui/cmdline.h"
#include "ui/compgen.h"
#include "ui/message.h"
#include "unicode/utf8.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/sort.h"
#include "util/xdg.h"

/* ================================================================ */
/* §3: the parser                                                    */
/* ================================================================ */

enum {
    HP_TERM_MAX = 256U,
    HP_DESC_MAX = 1024U,
    HP_SUBS_MAX = 512U,
    HP_FLAGS_MAX = 512U,
    HP_ALIASES_MAX = 8U,
    HP_VALUES_MAX = 64U,
    HP_NAME_MAX = 64U,
    HP_WORDS_MAX = 32U
};

typedef enum HelpSection {
    HS_GENERAL,  /* subcommands and flags                              */
    HS_COMMANDS, /* a heading naming commands: desc-less rows count too */
    HS_FLAGS,    /* "Options:", "Flags:" -- flag rows only             */
    HS_ARGS,     /* "positional arguments:" -- only the choice set     */
    HS_USAGE,    /* "Usage:" -- only rows spelled with the program     */
    HS_IGNORE    /* examples, keys, environment                        */
} HelpSection;

typedef struct HpSub {
    const char *name;
    const char *aliases[HP_ALIASES_MAX];
    u32 n_aliases;
    const char *desc;
} HpSub;

typedef struct HpFlag {
    const char *lng;
    char shrt;
    const char *desc;
    bool has_arg;
    bool arg_optional;
    bool global;
    YewSpecArgKind kind;
    const char **values;
    u32 n_values;
} HpFlag;

typedef struct HelpParse {
    Arena *a; /* scratch: every string until the spec is built */
    const char *const *words;
    u32 n_words;
    HelpSection section;
    bool global;
    /* The row being collected. */
    bool active;
    bool has_desc;
    bool tab_gap;
    u32 indent;
    u32 desc_col;
    char term[HP_TERM_MAX];
    size_t term_len;
    bool term_cut;
    char desc[HP_DESC_MAX];
    size_t desc_len;
    /* Column alignment of the section's rows, for a name that fills its
     * column and leaves ONE space before the description (cobra). */
    u32 align_indent;
    size_t align_off;
    /* argparse's {a,b,c}. */
    const char *choices[HP_SUBS_MAX];
    u32 n_choices;
    HpSub subs[HP_SUBS_MAX];
    u32 n_subs;
    HpFlag flags[HP_FLAGS_MAX];
    u32 n_flags;
} HelpParse;

static bool hp_blank(char c)
{
    return c == ' ' || c == '\t';
}

static char hp_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool hp_contains_ci(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    size_t i;
    size_t j;

    for (i = 0U; i + k <= n; i++) {
        for (j = 0U; j < k; j++) {
            if (hp_lower(s[i + j]) != needle[j])
                break;
        }
        if (j == k)
            return true;
    }
    return false;
}

/*
 * ANSI and overstrike (§3 pitfall).  CSI (`ESC [ … final`) and OSC
 * (`ESC ] … BEL|ST`) sequences and two-byte `ESC x` escapes are removed;
 * `X\bX` and `_\bX` keep the second glyph.  An ESC that starts nothing
 * recognisable is KEPT, so the row holding it is dropped later rather
 * than half-read.  CR is dropped (CRLF); other C0 controls but TAB and
 * LF become spaces.  Invalid UTF-8 is left for the cleaner of each field.
 */
static void hp_strip(const char *in, size_t len, Bytebuf *out)
{
    size_t i = 0U;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];

        if (c == 0x1BU && i + 1U < len && in[i + 1U] == '[') {
            i += 2U;
            while (i < len && !((unsigned char)in[i] >= 0x40U &&
                                (unsigned char)in[i] <= 0x7EU))
                i++;
            if (i < len)
                i++;
            continue;
        }
        if (c == 0x1BU && i + 1U < len && in[i + 1U] == ']') {
            i += 2U;
            while (i < len && in[i] != '\a' &&
                   !(in[i] == 0x1B && i + 1U < len && in[i + 1U] == '\\'))
                i++;
            if (i < len)
                i += in[i] == '\a' ? 1U : 2U;
            continue;
        }
        if (c == 0x1BU && i + 1U < len &&
            (unsigned char)in[i + 1U] >= 0x40U &&
            (unsigned char)in[i + 1U] <= 0x5FU) {
            i += 2U;
            continue;
        }
        if (c == '\b') {
            /* Take back the previous glyph (a whole UTF-8 sequence). */
            while (out->len != 0U &&
                   (out->data[out->len - 1U] & 0xC0U) == 0x80U)
                out->len--;
            if (out->len != 0U && out->data[out->len - 1U] != '\n')
                out->len--;
            i++;
            continue;
        }
        if (c == '\r') {
            i++;
            continue;
        }
        if (c < 0x20U && c != '\t' && c != '\n' && c != 0x1BU)
            c = ' ';
        if (c == 0x7FU)
            c = ' ';
        bytebuf_push_u8(out, (u8)c);
        i++;
    }
}

/* A pager field: whitespace runs collapsed, invalid UTF-8 and controls
 * shown as U+FFFD, cut at `max` bytes on a character boundary. */
static const char *hp_field(Arena *a, const char *s, size_t n, size_t max)
{
    Bytebuf b;
    const char *out;
    size_t i = 0U;
    bool space = false;

    bytebuf_init(&b);
    while (i < n && hp_blank(s[i]))
        i++;
    while (i < n) {
        u32 cp;
        size_t k = yew_utf8_decode((const u8 *)s + i, n - i, &cp);
        u8 enc[4];
        size_t en;

        if (k == 0U)
            k = 1U;
        if (hp_blank(s[i])) {
            space = true;
            i += k;
            continue;
        }
        if (yew_utf8_is_escape(cp) || cp < 0x20U || cp == 0x7FU) {
            (void)memcpy(enc, "\xEF\xBF\xBD", 3U);
            en = 3U;
        } else {
            (void)memcpy(enc, s + i, k);
            en = k;
        }
        if (b.len + (space ? 1U : 0U) + en > max)
            break;
        if (space && b.len != 0U)
            bytebuf_push_u8(&b, ' ');
        space = false;
        bytebuf_append(&b, enc, en);
        i += k;
    }
    out = arena_strndup(a, b.len == 0U ? "" : (const char *)b.data, b.len);
    bytebuf_free(&b);
    return out;
}

static bool hp_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

/* §3: a subcommand name is `^[a-z][a-z0-9_-]*$`. */
static bool hp_sub_name(const char *s, size_t n)
{
    size_t i;

    if (n == 0U || n > HP_NAME_MAX || !(s[0] >= 'a' && s[0] <= 'z'))
        return false;
    for (i = 1U; i < n; i++) {
        if (!hp_word_char(s[i]))
            return false;
    }
    return true;
}

static bool hp_long_name(const char *s, size_t n)
{
    size_t i;

    if (n == 0U || n > HP_NAME_MAX)
        return false;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
          (s[0] >= '0' && s[0] <= '9')))
        return false;
    for (i = 1U; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool hp_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static void hp_set_section(HelpParse *p, const char *s, size_t n)
{
    p->global = false;
    p->align_off = 0U;
    if (hp_contains_ci(s, n, "example") || hp_contains_ci(s, n, "keys") ||
        hp_contains_ci(s, n, "keybinding") ||
        hp_contains_ci(s, n, "key binding") ||
        hp_contains_ci(s, n, "shortcut") ||
        hp_contains_ci(s, n, "environment")) {
        p->section = HS_IGNORE;
        return;
    }
    if (n >= 5U && hp_contains_ci(s, 5U, "usage")) {
        p->section = HS_USAGE;
        return;
    }
    if (hp_contains_ci(s, n, "option") || hp_contains_ci(s, n, "flag")) {
        p->section = HS_FLAGS;
        p->global = hp_contains_ci(s, n, "global");
        return;
    }
    if (hp_contains_ci(s, n, "argument") ||
        hp_contains_ci(s, n, "positional")) {
        p->section = HS_ARGS;
        return;
    }
    p->section = hp_contains_ci(s, n, "command") ? HS_COMMANDS : HS_GENERAL;
}

/* ---------------------------------------------------------------- */
/* Rows -> subcommands                                                */
/* ---------------------------------------------------------------- */

static HpSub *hp_find_sub(HelpParse *p, const char *name, size_t n)
{
    u32 i;
    u32 k;

    for (i = 0U; i < p->n_subs; i++) {
        HpSub *s = &p->subs[i];

        if (strlen(s->name) == n && memcmp(s->name, name, n) == 0)
            return s;
        for (k = 0U; k < s->n_aliases; k++) {
            if (strlen(s->aliases[k]) == n &&
                memcmp(s->aliases[k], name, n) == 0)
                return s;
        }
    }
    return NULL;
}

static bool hp_in_choices(const HelpParse *p, const char *name, size_t n)
{
    u32 i;

    for (i = 0U; i < p->n_choices; i++) {
        if (strlen(p->choices[i]) == n &&
            memcmp(p->choices[i], name, n) == 0)
            return true;
    }
    return false;
}

static HpSub *hp_add_sub(HelpParse *p, const char *name, size_t n)
{
    HpSub *s = hp_find_sub(p, name, n);

    /* Found by an alias of another row: not the same subcommand. */
    if (s != NULL)
        return strlen(s->name) == n && memcmp(s->name, name, n) == 0 ? s
                                                                       : NULL;
    /* Its own program is not one of its subcommands (a usage line). */
    if (p->n_words != 0U && strlen(p->words[0]) == n &&
        memcmp(p->words[0], name, n) == 0)
        return NULL;
    if (p->n_subs == HP_SUBS_MAX)
        return NULL;
    s = &p->subs[p->n_subs++];
    (void)memset(s, 0, sizeof(*s));
    s->name = arena_strndup(p->a, name, n);
    return s;
}

static void hp_add_alias(HelpParse *p, HpSub *s, const char *name, size_t n)
{
    HpSub *other;

    if (!hp_sub_name(name, n) || s->n_aliases == HP_ALIASES_MAX)
        return;
    other = hp_find_sub(p, name, n);
    if (other != NULL) {
        u32 at = (u32)(other - p->subs);
        u32 k;

        /* argparse lists an alias among the choices (`{chat,run}`) and
         * only its row says `chat (run)`: a bare, undescribed choice of
         * that name was the alias all along. */
        if (other == s || other->desc != NULL || other->n_aliases != 0U ||
            strlen(other->name) != n || memcmp(other->name, name, n) != 0)
            return;
        if (s > other)
            s--;
        for (k = at; k + 1U < p->n_subs; k++)
            p->subs[k] = p->subs[k + 1U];
        p->n_subs--;
    }
    s->aliases[s->n_aliases++] = arena_strndup(p->a, name, n);
}

/* `[aliases: a, b]` (yargs) in a description. */
static void hp_desc_aliases(HelpParse *p, HpSub *s, const char *d, size_t n)
{
    static const char tag[] = "[aliases:";
    size_t i;

    for (i = 0U; i + sizeof(tag) - 1U <= n; i++) {
        if (memcmp(d + i, tag, sizeof(tag) - 1U) == 0)
            break;
    }
    if (i + sizeof(tag) - 1U > n)
        return;
    i += sizeof(tag) - 1U;
    while (i < n && d[i] != ']') {
        size_t start;

        while (i < n && (hp_blank(d[i]) || d[i] == ','))
            i++;
        start = i;
        while (i < n && d[i] != ',' && d[i] != ']' && !hp_blank(d[i]))
            i++;
        if (i > start)
            hp_add_alias(p, s, d + start, i - start);
    }
}

/*
 * A subcommand term: ONE name, then only aliases and bracketed metas --
 * `build`, `build, b`, `build|b`, `chat (run)`, `build [options] <file>`.
 * Anything else (a second plain word) is prose, not a row.
 */
static void hp_sub_row(HelpParse *p, const char *t, size_t n, const char *d,
                       size_t dn)
{
    const char *aliases[HP_ALIASES_MAX];
    size_t alias_len[HP_ALIASES_MAX];
    u32 n_alias = 0U;
    size_t i = 0U;
    size_t name_end;
    HpSub *s;

    while (i < n && hp_word_char(t[i]))
        i++;
    name_end = i;
    if (!hp_sub_name(t, name_end))
        return;
    while (i < n) {
        if (t[i] == ',' || t[i] == '|' || hp_blank(t[i])) {
            i++;
            continue;
        }
        if (t[i] == '(' ) {
            /* argparse's `chat (run)`: the parenthesised names are
             * aliases. */
            i++;
            while (i < n && t[i] != ')') {
                size_t start;

                while (i < n && (hp_blank(t[i]) || t[i] == ','))
                    i++;
                start = i;
                while (i < n && hp_word_char(t[i]))
                    i++;
                if (i > start && n_alias < HP_ALIASES_MAX) {
                    aliases[n_alias] = t + start;
                    alias_len[n_alias++] = i - start;
                } else if (i == start && i < n && t[i] != ')') {
                    return;
                }
            }
            if (i >= n)
                return;
            i++;
            continue;
        }
        if (t[i] == '[' || t[i] == '<') {
            char close = t[i] == '[' ? ']' : '>';
            const char *end = memchr(t + i, close, n - i);

            if (end == NULL)
                return;
            i = (size_t)(end - t) + 1U;
            /* `<files...>`, `[args]...` */
            while (i < n && t[i] == '.')
                i++;
            continue;
        }
        if (i > 0U && (t[i - 1U] == ',' || t[i - 1U] == '|' ||
                       (i > 1U && t[i - 2U] == ',' && hp_blank(t[i - 1U])))) {
            size_t start = i;

            while (i < n && hp_word_char(t[i]))
                i++;
            if (i == start || n_alias == HP_ALIASES_MAX)
                return;
            aliases[n_alias] = t + start;
            alias_len[n_alias++] = i - start;
            continue;
        }
        return; /* a second plain word: prose */
    }
    if (p->section == HS_ARGS && !hp_in_choices(p, t, name_end))
        return;
    if (dn == 0U && p->section != HS_COMMANDS &&
        !hp_in_choices(p, t, name_end))
        return;
    s = hp_add_sub(p, t, name_end);
    if (s == NULL)
        return;
    for (i = 0U; i < n_alias; i++)
        hp_add_alias(p, s, aliases[i], alias_len[i]);
    if (dn != 0U) {
        hp_desc_aliases(p, s, d, dn);
        if (s->desc == NULL || s->desc[0] == '\0')
            s->desc = hp_field(p->a, d, dn, YEW_COMPHELP_DESC_MAX);
    }
}

/* `{a,b,c}`: argparse's choice line.  Its names are subcommands even
 * when no row below describes them. */
static bool hp_choice_line(HelpParse *p, const char *t, size_t n)
{
    size_t i = 1U;

    if (n < 3U || t[0] != '{' || t[n - 1U] != '}')
        return false;
    while (i < n - 1U) {
        size_t start = i;

        while (i < n - 1U && t[i] != ',')
            i++;
        if (hp_sub_name(t + start, i - start)) {
            if (!hp_in_choices(p, t + start, i - start) &&
                p->n_choices < HP_SUBS_MAX)
                p->choices[p->n_choices++] =
                    arena_strndup(p->a, t + start, i - start);
            (void)hp_add_sub(p, t + start, i - start);
        }
        i++;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Rows -> flags                                                      */
/* ---------------------------------------------------------------- */

static const char *const cobra_types[] = {
    "string", "strings", "stringArray", "stringSlice", "stringToString",
    "int", "ints", "int8", "int16", "int32", "int64", "uint", "uints",
    "uint8", "uint16", "uint32", "uint64", "float", "float32", "float64",
    "duration", "durations", "ip", "ipSlice", "ipNet", "bytesHex",
    "bytesBase64", "count", "value", "list"
};

static bool hp_is_type_word(const char *s, size_t n)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(cobra_types); i++) {
        if (strlen(cobra_types[i]) == n && memcmp(cobra_types[i], s, n) == 0)
            return true;
    }
    return false;
}

static bool hp_all_caps(const char *s, size_t n)
{
    size_t i;

    if (n == 0U || !(s[0] >= 'A' && s[0] <= 'Z'))
        return false;
    for (i = 1U; i < n; i++) {
        char c = s[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

/* Trailing `...` (repeatable) and `,` (a spelling list) come off. */
static size_t hp_trim_tail(const char *s, size_t n)
{
    while (n > 0U && (s[n - 1U] == ',' || s[n - 1U] == '.'))
        n--;
    return n;
}

static bool hp_is_meta(const char *s, size_t n)
{
    n = hp_trim_tail(s, n);
    if (n >= 2U && ((s[0] == '<' && s[n - 1U] == '>') ||
                    (s[0] == '[' && s[n - 1U] == ']') ||
                    (s[0] == '{' && s[n - 1U] == '}')))
        return true;
    return hp_all_caps(s, n) || (hp_is_type_word(s, n) && n != 0U) ||
           (n == 4U && memcmp(s, "bool", 4U) == 0);
}

static bool hp_eq_ci(const char *s, size_t n, const char *w)
{
    size_t k = strlen(w);
    size_t i;

    if (n != k)
        return false;
    for (i = 0U; i < n; i++) {
        if (hp_lower(s[i]) != w[i])
            return false;
    }
    return true;
}

static bool hp_suffix_ci(const char *s, size_t n, const char *w)
{
    size_t k = strlen(w);

    return n > k && hp_eq_ci(s + n - k, k, w) &&
           (s[n - k - 1U] == '_' || s[n - k - 1U] == '-');
}

/* A value list `a,b,c` or `a|b|c`: every item a plain token. */
static bool hp_split_values(HelpParse *p, const char *s, size_t n, char sep,
                            HpFlag *f)
{
    const char *vals[HP_VALUES_MAX];
    size_t lens[HP_VALUES_MAX];
    u32 count = 0U;
    size_t i = 0U;
    u32 k;

    while (i <= n) {
        size_t start;
        size_t end;

        while (i < n && (hp_blank(s[i]) || s[i] == '"' || s[i] == '\''))
            i++;
        start = i;
        while (i < n && s[i] != sep)
            i++;
        end = i;
        while (end > start && (hp_blank(s[end - 1U]) || s[end - 1U] == '"' ||
                               s[end - 1U] == '\''))
            end--;
        if (end == start || count == HP_VALUES_MAX)
            return false;
        for (k = (u32)start; k < end; k++) {
            if (!(hp_alnum(s[k]) || s[k] == '-' || s[k] == '_' ||
                  s[k] == '.' || s[k] == '+' || s[k] == '/' || s[k] == ':'))
                return false;
        }
        vals[count] = s + start;
        lens[count++] = end - start;
        i++;
    }
    if (count < 2U)
        return false;
    f->values = arena_alloc(p->a, count * sizeof(*f->values),
                            sizeof(void *));
    for (k = 0U; k < count; k++)
        f->values[k] = arena_strndup(p->a, vals[k], lens[k]);
    f->n_values = count;
    f->kind = YEW_SPEC_ARG_VALUES;
    return true;
}

/* `[possible values: a, b]` (clap), `[choices: "a", "b"]` (yargs), or
 * `(a|b|c)` in a description. */
static bool hp_desc_values(HelpParse *p, const char *d, size_t n, HpFlag *f)
{
    static const char *const tags[] = {"[possible values:", "[choices:"};
    size_t i;
    size_t t;

    for (t = 0U; t < YEW_ARRAY_LEN(tags); t++) {
        size_t k = strlen(tags[t]);

        for (i = 0U; i + k <= n; i++) {
            if (memcmp(d + i, tags[t], k) == 0) {
                const char *close = memchr(d + i + k, ']', n - i - k);

                if (close != NULL &&
                    hp_split_values(p, d + i + k,
                                    (size_t)(close - (d + i + k)), ',', f))
                    return true;
            }
        }
    }
    for (i = 0U; i < n; i++) {
        if (d[i] == '(') {
            const char *close = memchr(d + i, ')', n - i);

            if (close != NULL &&
                memchr(d + i, '|', (size_t)(close - (d + i))) != NULL &&
                hp_split_values(p, d + i + 1U, (size_t)(close - (d + i)) - 1U,
                                '|', f))
                return true;
        }
    }
    return false;
}

static void hp_meta_kind(HelpParse *p, const char *m, size_t n, const char *d,
                         size_t dn, HpFlag *f)
{
    n = hp_trim_tail(m, n);
    f->kind = YEW_SPEC_ARG_NONE;
    if (n >= 2U && m[0] == '{' && m[n - 1U] == '}' &&
        hp_split_values(p, m + 1, n - 2U, ',', f))
        return;
    if (n >= 2U && (m[0] == '[' || m[0] == '<') &&
        memchr(m, '|', n) != NULL &&
        hp_split_values(p, m + 1, n - 2U, '|', f))
        return;
    if (n >= 2U && (m[0] == '<' || m[0] == '[')) {
        m++;
        n -= 2U;
    }
    if (hp_eq_ci(m, n, "file") || hp_eq_ci(m, n, "path") ||
        hp_eq_ci(m, n, "files") || hp_eq_ci(m, n, "paths") ||
        hp_eq_ci(m, n, "filename") || hp_eq_ci(m, n, "pathname") ||
        hp_suffix_ci(m, n, "file") || hp_suffix_ci(m, n, "path")) {
        f->kind = YEW_SPEC_ARG_PATH;
        return;
    }
    if (hp_eq_ci(m, n, "dir") || hp_eq_ci(m, n, "directory") ||
        hp_eq_ci(m, n, "dirs") || hp_eq_ci(m, n, "folder") ||
        hp_suffix_ci(m, n, "dir") || hp_suffix_ci(m, n, "directory")) {
        f->kind = YEW_SPEC_ARG_DIR;
        return;
    }
    (void)hp_desc_values(p, d, dn, f);
}

static bool hp_flag_known(const HelpParse *p, const char *lng, char shrt)
{
    u32 i;

    for (i = 0U; i < p->n_flags; i++) {
        if (lng != NULL && p->flags[i].lng != NULL &&
            strcmp(p->flags[i].lng, lng) == 0)
            return true;
        if (shrt != '\0' && p->flags[i].shrt == shrt)
            return true;
    }
    return false;
}

typedef struct HpSpelling {
    const char *s; /* name without dashes */
    size_t n;
    bool is_long;
} HpSpelling;

/*
 * §3 "Classifying a term": split on `, ` and on spaces before `-`; each
 * part is `-x`, `--long`, `--long=META`, `--long[=META]`, `--long META`
 * or `-x META`.  A multi-letter single-dash spelling (`-name`) is Go's
 * flag package, which also accepts `--name`; outside that layout it is
 * dropped rather than invented as a long flag.
 */
static void hp_flag_row(HelpParse *p, const char *t, size_t n, const char *d,
                        size_t dn, bool go)
{
    HpSpelling sp[8];
    u32 n_sp = 0U;
    const char *meta = NULL;
    size_t meta_len = 0U;
    bool optional = false;
    size_t i = 0U;
    HpFlag proto;
    u32 k;
    const char *desc;

    while (i < n) {
        size_t start;
        size_t len;
        const char *w;

        while (i < n && hp_blank(t[i]))
            i++;
        start = i;
        while (i < n && !hp_blank(t[i]))
            i++;
        if (i == start)
            break;
        w = t + start;
        len = i - start;
        if (w[0] == '-' && len >= 2U && w[1] != ',') {
            const char *name = w[1] == '-' ? w + 2 : w + 1;
            size_t nn = len - (size_t)(name - w);
            const char *eq;
            bool is_long = w[1] == '-';

            nn = hp_trim_tail(name, nn);
            eq = memchr(name, '[', nn);
            if (eq != NULL && eq + 1 < name + nn && eq[1] == '=') {
                optional = true;
                if (meta == NULL) {
                    meta = eq + 2;
                    meta_len = (size_t)(name + nn - (eq + 2));
                    if (meta_len != 0U && meta[meta_len - 1U] == ']')
                        meta_len--;
                }
                nn = (size_t)(eq - name);
            } else if ((eq = memchr(name, '=', nn)) != NULL) {
                if (meta == NULL) {
                    meta = eq + 1;
                    meta_len = (size_t)(name + nn - (eq + 1));
                }
                nn = (size_t)(eq - name);
            }
            if (!is_long && nn > 1U) {
                if (!go)
                    continue; /* `-name` outside Go's layout */
                is_long = true;
            }
            if (n_sp < YEW_ARRAY_LEN(sp) &&
                ((is_long && hp_long_name(name, nn)) ||
                 (!is_long && nn == 1U && hp_alnum(name[0])))) {
                sp[n_sp].s = name;
                sp[n_sp].n = nn;
                sp[n_sp].is_long = is_long;
                n_sp++;
            }
            continue;
        }
        if (hp_is_meta(w, len)) {
            if (meta == NULL) {
                meta = w;
                meta_len = hp_trim_tail(w, len);
            }
            continue;
        }
        break; /* prose after the spellings: stop reading the term */
    }
    if (n_sp == 0U)
        return;
    (void)memset(&proto, 0, sizeof(proto));
    proto.global = p->global;
    proto.arg_optional = optional;
    if (meta != NULL && meta_len != 0U &&
        !(meta_len == 4U && memcmp(meta, "bool", 4U) == 0)) {
        proto.has_arg = true;
        hp_meta_kind(p, meta, meta_len, d, dn, &proto);
    } else {
        proto.arg_optional = false;
        /* yargs puts the type after the description: `[string]`,
         * `[number]`, `[array]`, and `[choices: "a", "b"]`. */
        if (hp_contains_ci(d, dn, "[string]") ||
            hp_contains_ci(d, dn, "[number]") ||
            hp_contains_ci(d, dn, "[array]") ||
            hp_contains_ci(d, dn, "[choices:")) {
            proto.has_arg = true;
            proto.kind = YEW_SPEC_ARG_NONE;
            if (hp_contains_ci(d, dn, "[choices:"))
                (void)hp_desc_values(p, d, dn, &proto);
        }
    }
    desc = dn == 0U ? NULL : hp_field(p->a, d, dn, YEW_COMPHELP_DESC_MAX);
    /* The first short and the first long are one flag (`-o, --output`);
     * any further spelling is a flag of its own with the same argument
     * and description.  A spelling an earlier row already gave is not
     * repeated. */
    {
        int first_short = -1;
        int first_long = -1;

        for (k = 0U; k < n_sp; k++) {
            if (sp[k].is_long && first_long < 0)
                first_long = (int)k;
            if (!sp[k].is_long && first_short < 0)
                first_short = (int)k;
        }
        for (k = 0U; k < n_sp && p->n_flags < HP_FLAGS_MAX; k++) {
            HpFlag f = proto;

            f.desc = desc;
            if ((int)k == first_long && first_short >= 0)
                continue; /* rides with the first short */
            if (sp[k].is_long) {
                f.lng = arena_strndup(p->a, sp[k].s, sp[k].n);
            } else {
                f.shrt = sp[k].s[0];
                if ((int)k == first_short && first_long >= 0)
                    f.lng = arena_strndup(p->a, sp[first_long].s,
                                          sp[first_long].n);
            }
            if (f.lng != NULL && hp_flag_known(p, f.lng, '\0'))
                f.lng = NULL;
            if (f.shrt != '\0' && hp_flag_known(p, NULL, f.shrt))
                f.shrt = '\0';
            if (f.lng == NULL && f.shrt == '\0')
                continue;
            p->flags[p->n_flags++] = f;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Rows                                                               */
/* ---------------------------------------------------------------- */

static void hp_classify(HelpParse *p)
{
    const char *t = p->term;
    size_t n = p->term_len;
    const char *d = p->desc;
    size_t dn = p->has_desc ? p->desc_len : 0U;
    size_t i = 0U;
    u32 w;
    bool prefixed = false;

    /* §3: a term still holding an ESC after stripping is not read. */
    if (p->section == HS_IGNORE || n == 0U || p->term_cut ||
        memchr(t, 0x1B, n) != NULL)
        return;
    /*
     * A row spelled with the program's own name (`fac -w <dir>`, yargs'
     * `opencode acp`, argparse's usage): the words after the name and the
     * asked-for subcommand path are the term.
     */
    for (w = 0U; w < p->n_words; w++) {
        size_t k = strlen(p->words[w]);
        size_t j = i;

        while (j < n && hp_blank(t[j]))
            j++;
        if (n - j >= k && memcmp(t + j, p->words[w], k) == 0 &&
            (j + k == n || hp_blank(t[j + k]))) {
            i = j + k;
            prefixed = true;
        } else {
            break;
        }
    }
    if (prefixed) {
        while (i < n && hp_blank(t[i]))
            i++;
        t += i;
        n -= i;
        if (n == 0U)
            return;
    } else if (p->section == HS_USAGE && t[0] != '-') {
        /* A usage section's rows are synopses; only its flag rows (Go's
         * `usage:` line is followed directly by them) and rows spelled
         * with the program's name say anything. */
        return;
    }
    if (t[0] == '-') {
        hp_flag_row(p, t, n, d, dn, p->tab_gap);
        return;
    }
    if (!prefixed && hp_choice_line(p, t, n))
        return;
    if (p->section == HS_FLAGS)
        return;
    hp_sub_row(p, t, n, d, dn);
}

static void hp_finish(HelpParse *p)
{
    if (!p->active)
        return;
    p->active = false;
    hp_classify(p);
}

static void hp_desc_append(HelpParse *p, const char *s, size_t n)
{
    size_t room;

    if (p->desc_len != 0U && p->desc_len < sizeof(p->desc) - 1U)
        p->desc[p->desc_len++] = ' ';
    room = sizeof(p->desc) - 1U - p->desc_len;
    if (n > room)
        n = room;
    (void)memcpy(p->desc + p->desc_len, s, n);
    p->desc_len += n;
    p->desc[p->desc_len] = '\0';
}

/* The gap between a term and its description: two or more spaces, or a
 * tab.  Returns the offset of the gap, or n. */
static size_t hp_gap(const char *s, size_t n, bool *tab)
{
    size_t i;

    for (i = 0U; i < n; i++) {
        if (s[i] == '\t') {
            *tab = true;
            return i;
        }
        if (s[i] == ' ' && i + 1U < n && s[i + 1U] == ' ') {
            *tab = false;
            return i;
        }
    }
    return n;
}

static void hp_start(HelpParse *p, u32 indent, const char *line, size_t ws,
                     const char *body, size_t bn)
{
    bool tab = false;
    size_t g = hp_gap(body, bn, &tab);
    size_t tl = g;
    size_t dstart = bn;

    if (g < bn) {
        dstart = g;
        while (dstart < bn && hp_blank(body[dstart]))
            dstart++;
    } else if (p->align_off > ws && p->align_indent == indent &&
               p->align_off < ws + bn && line[p->align_off - 1U] == ' ' &&
               !hp_blank(line[p->align_off]) &&
               memchr(body, ' ', p->align_off - 1U - ws) == NULL) {
        /* cobra pads names to one column; the longest leaves a single
         * space before its description. */
        tl = p->align_off - 1U - ws;
        dstart = p->align_off - ws;
    }
    p->active = true;
    p->indent = indent;
    p->tab_gap = tab;
    p->term_cut = tl >= sizeof(p->term);
    if (p->term_cut)
        tl = sizeof(p->term) - 1U;
    (void)memcpy(p->term, body, tl);
    while (tl > 0U && hp_blank(p->term[tl - 1U]))
        tl--;
    p->term[tl] = '\0';
    p->term_len = tl;
    p->desc_len = 0U;
    p->desc[0] = '\0';
    p->has_desc = dstart < bn;
    p->desc_col = 0U;
    if (p->has_desc) {
        hp_desc_append(p, body + dstart, bn - dstart);
        p->desc_col = indent + (u32)dstart;
        if (!tab && memchr(line, '\t', ws + dstart) == NULL) {
            p->align_indent = indent;
            p->align_off = ws + dstart;
        }
    }
}

/* Does an indented line read as a row of its own (term, gap, desc)? */
static bool hp_rowlike(const char *s, size_t n)
{
    bool tab = false;
    size_t g = hp_gap(s, n, &tab);
    size_t i = g;

    if (g == 0U || g >= n)
        return false;
    while (i < n && hp_blank(s[i]))
        i++;
    return i < n;
}

static void hp_line(HelpParse *p, const char *s, size_t n)
{
    size_t ws = 0U;
    u32 indent = 0U;

    while (n > 0U && hp_blank(s[n - 1U]))
        n--;
    if (n == 0U) {
        hp_finish(p);
        return;
    }
    while (ws < n && hp_blank(s[ws])) {
        indent = s[ws] == '\t' ? (indent / 8U + 1U) * 8U : indent + 1U;
        ws++;
    }
    if (ws == 0U || (indent < 2U && s[0] != '\t')) {
        hp_finish(p);
        hp_set_section(p, s + ws, n - ws);
        return;
    }
    if (p->active && indent > p->indent) {
        if (!p->has_desc) {
            /* Go's flag package and clap's long help: the description is
             * on the next line, indented further (Go's after a tab). */
            p->has_desc = true;
            p->desc_col = indent;
            if (s[ws - 1U] == '\t')
                p->tab_gap = true;
            hp_desc_append(p, s + ws, n - ws);
            return;
        }
        /* Indented to the description column, or prose that is not a
         * row of its own: more description.  A line that starts a flag
         * short of that column is the next row (GNU's layout). */
        if (indent >= p->desc_col ||
            (!hp_rowlike(s + ws, n - ws) && s[ws] != '-')) {
            hp_desc_append(p, s + ws, n - ws);
            return;
        }
    }
    hp_finish(p);
    hp_start(p, indent, s, ws, s + ws, n - ws);
}

static YewCompSpec *hp_build(HelpParse *p, const char *origin)
{
    YewCompSpec *spec;
    Arena *a;
    YewSpecNode *root;
    u32 i;

    if (p->n_subs == 0U && p->n_flags == 0U)
        return NULL;
    spec = yew_compspec_new(origin);
    a = yew_compspec_arena(spec);
    root = yew_compspec_root_mut(spec);
    if (p->n_flags != 0U) {
        YewSpecFlag *flags = arena_alloc(a, p->n_flags * sizeof(*flags),
                                         sizeof(void *));

        for (i = 0U; i < p->n_flags; i++) {
            const HpFlag *f = &p->flags[i];
            YewSpecFlag *o = &flags[i];

            (void)memset(o, 0, sizeof(*o));
            o->lng = f->lng == NULL ? NULL : arena_strdup(a, f->lng);
            if (f->shrt != '\0') {
                char *s = arena_alloc(a, 2U, 1U);

                s[0] = f->shrt;
                s[1] = '\0';
                o->shrt = s;
            }
            o->desc = f->desc == NULL || f->desc[0] == '\0'
                          ? NULL
                          : arena_strdup(a, f->desc);
            o->global = f->global;
            if (f->has_arg) {
                YewSpecArg *arg = arena_alloc(a, sizeof(*arg),
                                              sizeof(void *));

                (void)memset(arg, 0, sizeof(*arg));
                arg->kind = f->kind;
                if (f->kind == YEW_SPEC_ARG_VALUES) {
                    YewSpecValue *vals = arena_alloc(
                        a, f->n_values * sizeof(*vals), sizeof(void *));
                    u32 k;

                    for (k = 0U; k < f->n_values; k++) {
                        vals[k].value = arena_strdup(a, f->values[k]);
                        vals[k].desc = NULL;
                    }
                    arg->values = vals;
                    arg->n_values = f->n_values;
                }
                o->arg = arg;
                o->arg_optional = f->arg_optional;
            }
        }
        root->flags = flags;
        root->n_flags = p->n_flags;
    }
    if (p->n_subs != 0U) {
        YewSpecNode *subs = arena_alloc(a, p->n_subs * sizeof(*subs),
                                        sizeof(void *));

        for (i = 0U; i < p->n_subs; i++) {
            const HpSub *s = &p->subs[i];
            YewSpecNode *o = &subs[i];

            (void)memset(o, 0, sizeof(*o));
            o->name = arena_strdup(a, s->name);
            o->desc = s->desc == NULL || s->desc[0] == '\0'
                          ? NULL
                          : arena_strdup(a, s->desc);
            if (s->n_aliases != 0U) {
                const char **al = arena_alloc(
                    a, ((size_t)s->n_aliases + 1U) * sizeof(*al),
                    sizeof(void *));
                u32 k;

                for (k = 0U; k < s->n_aliases; k++)
                    al[k] = arena_strdup(a, s->aliases[k]);
                al[s->n_aliases] = NULL;
                o->aliases = al;
                o->n_aliases = s->n_aliases;
            }
            o->parent = root;
        }
        root->subs = subs;
        root->n_subs = p->n_subs;
    }
    return spec;
}

YewCompSpec *yew_comphelp_parse(const char *origin, const char *const *words,
                                u32 n_words, const char *text, size_t len)
{
    Arena scratch;
    Bytebuf clean;
    HelpParse *p;
    YewCompSpec *spec;
    size_t at = 0U;

    if (text == NULL)
        return NULL;
    if (len > YEW_COMPHELP_COLLECT_MAX)
        len = YEW_COMPHELP_COLLECT_MAX;
    arena_init(&scratch);
    bytebuf_init(&clean);
    hp_strip(text, len, &clean);
    p = yew_xcalloc(1U, sizeof(*p));
    p->a = &scratch;
    p->words = words;
    p->n_words = n_words > HP_WORDS_MAX ? HP_WORDS_MAX : n_words;
    p->section = HS_GENERAL;
    while (at < clean.len) {
        const char *line = (const char *)clean.data + at;
        const char *nl = memchr(line, '\n', clean.len - at);
        size_t ll = nl == NULL ? clean.len - at : (size_t)(nl - line);

        hp_line(p, line, ll);
        at += ll + 1U;
    }
    hp_finish(p);
    spec = hp_build(p, origin == NULL ? "help" : origin);
    yew_xfree(p);
    bytebuf_free(&clean);
    arena_free_all(&scratch);
    return spec;
}

/* ================================================================ */
/* §1: policy                                                        */
/* ================================================================ */

YewHelpPolicy yew_comphelp_policy(Ed *ed)
{
    OptVal v;

    if (ed == NULL)
        return YEW_HELP_POLICY_OFF;
    if (!yew_opt_get(ed, NULL, NULL, "shell.complete_help", 19U, &v) ||
        (v.type != (u8)YEW_OPT_ENUM && v.type != (u8)YEW_OPT_STR))
        return YEW_HELP_POLICY_NATIVE;
    if (v.as.str.len == 3U && memcmp(v.as.str.s, "all", 3U) == 0)
        return YEW_HELP_POLICY_ALL;
    if (v.as.str.len == 3U && memcmp(v.as.str.s, "off", 3U) == 0)
        return YEW_HELP_POLICY_OFF;
    return YEW_HELP_POLICY_NATIVE;
}

/* §1: never these, whatever the policy says.  Matched on the basename;
 * every `mkfs.*` too. */
static const char *const help_denylist[] = {
    "rm", "rmdir", "dd", "shred", "srm", "wipefs", "mkfs", "fdisk",
    "diskutil", "format", "shutdown", "reboot", "halt", "poweroff", "kill",
    "killall", "pkill", "su", "sudo", "doas", "passwd", "chsh", "login",
    "logout", "exec", "reset"
};

bool yew_comphelp_denied(const char *name)
{
    size_t i;

    if (name == NULL)
        return true;
    if (strncmp(name, "mkfs.", 5U) == 0)
        return true;
    for (i = 0U; i < YEW_ARRAY_LEN(help_denylist); i++) {
        if (strcmp(name, help_denylist[i]) == 0)
            return true;
    }
    return false;
}

YewHelpExeKind yew_comphelp_exe_kind(const char *path)
{
    static const u8 macho[][4] = {
        {0xFEU, 0xEDU, 0xFAU, 0xCEU}, {0xFEU, 0xEDU, 0xFAU, 0xCFU},
        {0xCEU, 0xFAU, 0xEDU, 0xFEU}, {0xCFU, 0xFAU, 0xEDU, 0xFEU},
        {0xCAU, 0xFEU, 0xBAU, 0xBEU}};
    u8 head[4] = {0U, 0U, 0U, 0U};
    struct stat st;
    ssize_t got;
    size_t i;
    int fd;

    if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(path, X_OK) != 0)
        return YEW_HELP_EXE_NONE;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return YEW_HELP_EXE_NONE;
    do {
        got = read(fd, head, sizeof(head));
    } while (got < 0 && errno == EINTR);
    (void)close(fd);
    if (got >= 2 && head[0] == '#' && head[1] == '!')
        return YEW_HELP_EXE_SCRIPT;
    if (got < 4)
        return YEW_HELP_EXE_OTHER;
    if (memcmp(head, "\x7F" "ELF", 4U) == 0)
        return YEW_HELP_EXE_NATIVE;
    for (i = 0U; i < YEW_ARRAY_LEN(macho); i++) {
        if (memcmp(head, macho[i], 4U) == 0)
            return YEW_HELP_EXE_NATIVE;
    }
    return YEW_HELP_EXE_OTHER;
}

bool yew_comphelp_kind_allowed(YewHelpPolicy policy, YewHelpExeKind kind)
{
    switch (policy) {
    case YEW_HELP_POLICY_NATIVE:
        return kind == YEW_HELP_EXE_NATIVE;
    case YEW_HELP_POLICY_ALL:
        return kind != YEW_HELP_EXE_NONE;
    case YEW_HELP_POLICY_OFF:
    default:
        return false;
    }
}

/* ================================================================ */
/* State                                                             */
/* ================================================================ */

enum {
    HELP_MEM_MAX = 64U,
    HELP_QUEUE_MAX = 8U,
    HELP_MEMO_MAX = 64U,
    /* §6: at most one help job in flight -- a prewarm or an asked-for
     * descent alike -- and it counts toward compgen's cap of four. */
    HELP_INFLIGHT_MAX = 1U,
    /* A cache file past this is not one we wrote. */
    HELP_FILE_MAX = 1024U * 1024U
};

typedef struct HelpLoaded {
    char *path;        /* "auth" or "auth login" */
    YewCompSpec *spec; /* NULL: that help was negative */
} HelpLoaded;

typedef struct HelpEntry {
    char *real;
    i64 mtime;
    i64 size;
    char *exec;        /* the resolved path a request runs */
    YewCompSpec *root; /* NULL: negative */
    HelpLoaded *loaded;
    u32 n_loaded;
    u32 cap_loaded;
    u64 used;
} HelpEntry;

typedef struct HelpReq {
    char key[17];
    char *exec;
    char *real;
    i64 mtime;
    i64 size;
    char **subs;
    u32 n_subs;
    bool inflight;
    u32 job_id;
} HelpReq;

/* One resolution of a typed command word, held for one prompt. */
typedef struct HelpMemo {
    char *word;
    char *exec; /* NULL: nothing runnable by that name */
    char *real;
    i64 mtime;
    i64 size;
    YewHelpExeKind kind;
} HelpMemo;

typedef struct HelpOwner {
    char key[17];
    bool completed;
} HelpOwner;

static struct {
    HelpEntry v[HELP_MEM_MAX];
    u32 n;
    u64 clock;
    HelpReq q[HELP_QUEUE_MAX];
    u32 nq;
    HelpMemo memo[HELP_MEMO_MAX];
    u32 n_memo;
    char *notice;
    bool write_reported;
    u32 spawns;
    i64 timeout_ms;
    char last_argv[1024];
} help;

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

static char *join_path(const char *dir, const char *name)
{
    size_t nd = strlen(dir);
    size_t nn = strlen(name);
    char *out = yew_xmalloc(nd + nn + 2U);

    (void)memcpy(out, dir, nd);
    out[nd] = '/';
    (void)memcpy(out + nd + 1U, name, nn + 1U);
    return out;
}

static void notice_queue(const char *msg)
{
    if (help.notice == NULL)
        help.notice = yew_xstrdup(msg);
}

bool yew_comphelp_notice(Ed *ed)
{
    if (ed == NULL || help.notice == NULL)
        return false;
    yew_msg(ed, YEW_MSG_WARN, "%s", help.notice);
    yew_xfree(help.notice);
    help.notice = NULL;
    return true;
}

static void entry_drop(HelpEntry *e)
{
    u32 i;

    for (i = 0U; i < e->n_loaded; i++) {
        yew_compspec_free(e->loaded[i].spec);
        yew_xfree(e->loaded[i].path);
    }
    yew_xfree(e->loaded);
    yew_compspec_free(e->root);
    yew_xfree(e->real);
    yew_xfree(e->exec);
    (void)memset(e, 0, sizeof(*e));
}

static void req_drop_at(u32 at)
{
    HelpReq *r = &help.q[at];
    u32 i;

    for (i = 0U; i < r->n_subs; i++)
        yew_xfree(r->subs[i]);
    yew_xfree(r->subs);
    yew_xfree(r->exec);
    yew_xfree(r->real);
    for (i = at; i + 1U < help.nq; i++)
        help.q[i] = help.q[i + 1U];
    help.nq--;
    (void)memset(&help.q[help.nq], 0, sizeof(help.q[help.nq]));
}

static void memo_clear(void)
{
    u32 i;

    for (i = 0U; i < help.n_memo; i++) {
        yew_xfree(help.memo[i].word);
        yew_xfree(help.memo[i].exec);
        yew_xfree(help.memo[i].real);
    }
    help.n_memo = 0U;
}

void yew_comphelp_prompt_closed(void)
{
    u32 i = 0U;

    memo_clear();
    /* Unspawned requests belonged to the prompt that asked; an answer
     * already on its way still lands in the cache. */
    while (i < help.nq) {
        if (help.q[i].inflight)
            i++;
        else
            req_drop_at(i);
    }
}

void yew_comphelp_reset(void)
{
    u32 i;

    for (i = 0U; i < help.n; i++)
        entry_drop(&help.v[i]);
    help.n = 0U;
    while (help.nq != 0U)
        req_drop_at(help.nq - 1U);
    memo_clear();
    yew_xfree(help.notice);
    help.notice = NULL;
    help.write_reported = false;
    help.spawns = 0U;
    help.last_argv[0] = '\0';
}

u32 yew_comphelp_test_spawns(void)
{
    return help.spawns;
}

void yew_comphelp_test_set_timeout_ms(i64 ms)
{
    help.timeout_ms = ms;
}

const char *yew_comphelp_test_last_argv(void)
{
    return help.last_argv;
}

u32 yew_comphelp_inflight(void)
{
    u32 i;
    u32 n = 0U;

    for (i = 0U; i < help.nq; i++) {
        if (help.q[i].inflight)
            n++;
    }
    return n;
}

u32 yew_comphelp_queued(void)
{
    return help.nq - yew_comphelp_inflight();
}

static int req_find(const char *key)
{
    u32 i;

    for (i = 0U; i < help.nq; i++) {
        if (strcmp(help.q[i].key, key) == 0)
            return (int)i;
    }
    return -1;
}

bool yew_comphelp_awaiting(const char *key)
{
    return key != NULL && key[0] != '\0' && req_find(key) >= 0;
}

/* ================================================================ */
/* §5: keys and the disk cache                                       */
/* ================================================================ */

static u64 fnv_bytes(u64 h, const void *data, size_t n)
{
    const u8 *p = data;
    size_t i;

    for (i = 0U; i < n; i++) {
        h ^= (u64)p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

void yew_comphelp_key(const char *real, i64 mtime, i64 size,
                      const char *const *subs, u32 n_subs, char out[17])
{
    u64 h = UINT64_C(0xcbf29ce484222325);
    char num[32];
    int n;
    u32 i;

    /* realpath \0 st_mtime \0 st_size \0 sub \0 sub … */
    h = fnv_bytes(h, real, strlen(real) + 1U);
    n = snprintf(num, sizeof(num), "%lld", (long long)mtime);
    h = fnv_bytes(h, num, (size_t)n + 1U);
    n = snprintf(num, sizeof(num), "%lld", (long long)size);
    h = fnv_bytes(h, num, (size_t)n + 1U);
    for (i = 0U; i < n_subs; i++)
        h = fnv_bytes(h, subs[i], strlen(subs[i]) + 1U);
    (void)snprintf(out, 17U, "%016llx", (unsigned long long)h);
}

char *yew_comphelp_cache_dir(void)
{
    char *root = yew_xdg_cache_dir();
    char *dir;

    if (root == NULL)
        return NULL;
    dir = join_path(root, "completions/help");
    yew_xfree(root);
    return dir;
}

static bool cache_file_name(const char *name)
{
    size_t i;

    if (strlen(name) != 19U || strcmp(name + 16, ".fl") != 0)
        return false;
    for (i = 0U; i < 16U; i++) {
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    }
    return true;
}

static char *cache_path(const char *key)
{
    char *dir = yew_comphelp_cache_dir();
    char name[32];
    char *path;

    if (dir == NULL)
        return NULL;
    (void)snprintf(name, sizeof(name), "%s.fl", key);
    path = join_path(dir, name);
    yew_xfree(dir);
    return path;
}

static bool read_small(const char *path, Bytebuf *out)
{
    char chunk[4096];
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > (off_t)HELP_FILE_MAX) {
        (void)close(fd);
        return false;
    }
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            (void)close(fd);
            return false;
        }
        if (n == 0)
            break;
        if (out->len + (size_t)n > HELP_FILE_MAX) {
            (void)close(fd);
            return false;
        }
        bytebuf_append(out, chunk, (size_t)n);
    }
    (void)close(fd);
    return true;
}

typedef struct HelpDoc {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
} HelpDoc;

static void doc_quiet(void *ctx, FlDiagLevel level, FlSpan sp,
                      const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static void doc_init(HelpDoc *d)
{
    (void)memset(d, 0, sizeof(*d));
    arena_init(&d->arena);
    interner_init(&d->in, &d->arena);
    fl_diag_init(&d->dc, &d->arena);
    /* A cache file is ours to judge, never the terminal's to print. */
    fl_diag_set_sink(&d->dc, doc_quiet, NULL);
    (void)fl_vm_init(&d->vm, &d->arena, &d->in, &d->dc);
}

static void doc_free(HelpDoc *d)
{
    fl_vm_free(&d->vm);
    interner_free(&d->in);
    arena_free_all(&d->arena);
}

static bool map_str_is(const FlMap *m, const char *key, const char *want)
{
    FlValue k;
    FlValue v;
    u32 cursor = 0U;

    while (fl_map_iter(m, &cursor, &k, &v)) {
        const FlStr *ks = (const FlStr *)k.as.o;

        if (k.t != (u8)FL_STR || ks->len != strlen(key) ||
            memcmp(ks->b, key, ks->len) != 0)
            continue;
        if (v.t != (u8)FL_STR)
            return false;
        return ((const FlStr *)v.as.o)->len == strlen(want) &&
               memcmp(((const FlStr *)v.as.o)->b, want, strlen(want)) == 0;
    }
    return false;
}

static bool map_get_key(const FlMap *m, const char *key, FlValue *out)
{
    FlValue k;
    FlValue v;
    u32 cursor = 0U;

    while (fl_map_iter(m, &cursor, &k, &v)) {
        const FlStr *ks = (const FlStr *)k.as.o;

        if (k.t == (u8)FL_STR && ks->len == strlen(key) &&
            memcmp(ks->b, key, ks->len) == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

/*
 * Read and VALIDATE one cache file against the inputs of its key: any
 * mismatch, any unknown key, any parse error -- a torn write included --
 * is a miss, never a spec.  `*ok` says whether it was a hit; `*out` is
 * the tree (NULL on a hit for a negative result).
 */
static void cache_read(const char *key, const char *real, i64 mtime,
                       i64 size, const char *const *subs, u32 n_subs,
                       YewCompSpec **out, bool *ok)
{
    static const char *const known[] = {"help_cache", "exe",   "mtime",
                                        "size",       "argv",  "empty",
                                        "node"};
    char *path = cache_path(key);
    Bytebuf src;
    HelpDoc d;
    FlValue root;
    FlValue v;
    const FlMap *m;
    bool empty = false;
    u32 cursor = 0U;
    FlValue k;
    u32 i;

    *out = NULL;
    *ok = false;
    if (path == NULL)
        return;
    bytebuf_init(&src);
    if (!read_small(path, &src)) {
        bytebuf_free(&src);
        yew_xfree(path);
        return;
    }
    yew_xfree(path);
    doc_init(&d);
    root = fl_data_read(&d.vm, src.data == NULL ? "" : (const char *)src.data,
                        src.len, &d.dc);
    bytebuf_free(&src);
    if (fl_diag_errors(&d.dc) != 0U || root.t != (u8)FL_MAP)
        goto done;
    m = (const FlMap *)root.as.o;
    while (fl_map_iter(m, &cursor, &k, &v)) {
        const FlStr *ks = (const FlStr *)k.as.o;
        size_t j;
        bool found = false;

        if (k.t != (u8)FL_STR)
            goto done;
        for (j = 0U; j < YEW_ARRAY_LEN(known); j++) {
            if (ks->len == strlen(known[j]) &&
                memcmp(ks->b, known[j], ks->len) == 0)
                found = true;
        }
        if (!found)
            goto done;
    }
    if (!map_get_key(m, "help_cache", &v) || v.t != (u8)FL_INT ||
        v.as.i != 1 || !map_str_is(m, "exe", real) ||
        !map_get_key(m, "mtime", &v) || v.t != (u8)FL_INT ||
        v.as.i != mtime || !map_get_key(m, "size", &v) ||
        v.t != (u8)FL_INT || v.as.i != size ||
        !map_get_key(m, "empty", &v) || v.t != (u8)FL_BOOL)
        goto done;
    empty = v.as.b;
    if (!map_get_key(m, "argv", &v) || v.t != (u8)FL_LIST ||
        ((const FlList *)v.as.o)->n != n_subs)
        goto done;
    for (i = 0U; i < n_subs; i++) {
        FlValue s = ((const FlList *)v.as.o)->v[i];

        if (s.t != (u8)FL_STR ||
            ((const FlStr *)s.as.o)->len != strlen(subs[i]) ||
            memcmp(((const FlStr *)s.as.o)->b, subs[i], strlen(subs[i])) != 0)
            goto done;
    }
    if (empty) {
        *ok = !map_get_key(m, "node", &v);
        goto done;
    }
    if (!map_get_key(m, "node", &v))
        goto done;
    {
        char origin[PATH_MAX + 64];
        YewCompSpec *spec;

        (void)snprintf(origin, sizeof(origin), "help:%s", real);
        spec = yew_compspec_read_node(origin, &v, NULL, 0U);
        if (spec == NULL)
            goto done;
        *out = spec;
        *ok = true;
    }
done:
    doc_free(&d);
}

typedef struct PruneRow {
    char *name;
    i64 mtime;
} PruneRow;

static int prune_cmp(const void *a, const void *b, void *ctx)
{
    const PruneRow *x = a;
    const PruneRow *y = b;

    (void)ctx;
    if (x->mtime != y->mtime)
        return x->mtime < y->mtime ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* §5: past YEW_COMPHELP_CACHE_MAX files, the oldest by mtime go until
 * YEW_COMPHELP_CACHE_KEEP remain.  Only files we name are counted. */
static void cache_prune(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    PruneRow *rows = NULL;
    u32 n = 0U;
    u32 cap = 0U;
    u32 i;

    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        char *path;
        struct stat st;

        if (!cache_file_name(e->d_name))
            continue;
        path = join_path(dir, e->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (n == cap) {
                cap = cap == 0U ? 256U : cap * 2U;
                rows = yew_xrealloc(rows, cap * sizeof(*rows));
            }
            rows[n].name = yew_xstrdup(e->d_name);
            rows[n].mtime = (i64)st.st_mtime;
            n++;
        }
        yew_xfree(path);
    }
    (void)closedir(d);
    if (n > YEW_COMPHELP_CACHE_MAX) {
        yew_sort_stable(rows, n, sizeof(*rows), prune_cmp, NULL);
        for (i = 0U; i < n - YEW_COMPHELP_CACHE_KEEP; i++) {
            char *path = join_path(dir, rows[i].name);

            (void)unlink(path);
            yew_xfree(path);
        }
    }
    for (i = 0U; i < n; i++)
        yew_xfree(rows[i].name);
    yew_xfree(rows);
}

/* The atomic write the state files use (temp in the same directory,
 * fsync, rename): a torn file is never read back as a spec. */
static void cache_write(const HelpReq *r, const YewCompSpec *spec)
{
    char *dir = yew_comphelp_cache_dir();
    char *path;
    HelpDoc d;
    FlMap *m;
    FlList *argv;
    Bytebuf out;
    u32 i;

    if (dir == NULL)
        return;
    /* Created on first write only. */
    if (!yew_mkdirs(dir, 0700)) {
        if (!help.write_reported) {
            help.write_reported = true;
            notice_queue("help cache: cannot create its directory; "
                         "learned completions last one session");
        }
        yew_xfree(dir);
        return;
    }
    doc_init(&d);
    m = fl_map_new(&d.vm);
#define PUT(k_, v_)                                                          \
    (void)fl_map_set(&d.vm, m,                                             \
                     FL_OBJ_V(FL_STR, fl_str_new(&d.vm, (k_),               \
                                                 (u32)strlen(k_))),         \
                     (v_))
    PUT("help_cache", FL_INT_V(1));
    PUT("exe", FL_OBJ_V(FL_STR, fl_str_new(&d.vm, r->real,
                                           (u32)strlen(r->real))));
    PUT("mtime", FL_INT_V(r->mtime));
    PUT("size", FL_INT_V(r->size));
    argv = fl_list_new(&d.vm);
    for (i = 0U; i < r->n_subs; i++)
        (void)fl_list_push(&d.vm, argv,
                           FL_OBJ_V(FL_STR,
                                    fl_str_new(&d.vm, r->subs[i],
                                               (u32)strlen(r->subs[i]))));
    PUT("argv", FL_OBJ_V(FL_LIST, argv));
    PUT("empty", FL_BOOL_V(spec == NULL));
    if (spec != NULL)
        PUT("node", yew_compspec_write_node(&d.vm, yew_compspec_root(spec)));
#undef PUT
    bytebuf_init(&out);
    bytebuf_printf(&out, "# yew: learned from `%s%s --help`; "
                         "ed.shell.complete_forget drops it\n",
                   base_of(r->exec), r->n_subs == 0U ? "" : " …");
    fl_data_write(&out, FL_OBJ_V(FL_MAP, m), 0U);
    bytebuf_push_u8(&out, '\n');
    path = cache_path(r->key);
    if (path != NULL &&
        !yew_save_committed(yew_file_write_atomic(path, out.data, out.len,
                                                  0600)) &&
        !help.write_reported) {
        help.write_reported = true;
        notice_queue("help cache: cannot write; learned completions last "
                     "one session");
    }
    yew_xfree(path);
    bytebuf_free(&out);
    doc_free(&d);
    cache_prune(dir);
    yew_xfree(dir);
}

/* ================================================================ */
/* In memory: trees, grafts                                          */
/* ================================================================ */

static HelpEntry *entry_find(const char *real, i64 mtime, i64 size)
{
    u32 i;

    for (i = 0U; i < help.n; i++) {
        HelpEntry *e = &help.v[i];

        if (e->mtime == mtime && e->size == size &&
            strcmp(e->real, real) == 0) {
            e->used = ++help.clock;
            return e;
        }
    }
    return NULL;
}

static HelpEntry *entry_of_spec(const YewCompSpec *spec)
{
    u32 i;

    for (i = 0U; spec != NULL && i < help.n; i++) {
        if (help.v[i].root == spec)
            return &help.v[i];
    }
    return NULL;
}

bool yew_comphelp_owns(const YewCompSpec *spec)
{
    return entry_of_spec(spec) != NULL;
}

/* A new entry, evicting the least recently used past HELP_MEM_MAX. */
static HelpEntry *entry_add(const char *real, i64 mtime, i64 size,
                            const char *exec)
{
    HelpEntry *e;
    u32 i;

    /* An older identity of the same file is stale: it goes. */
    for (i = 0U; i < help.n; i++) {
        if (strcmp(help.v[i].real, real) == 0) {
            entry_drop(&help.v[i]);
            help.v[i] = help.v[help.n - 1U];
            (void)memset(&help.v[help.n - 1U], 0, sizeof(help.v[0]));
            help.n--;
            break;
        }
    }
    if (help.n == HELP_MEM_MAX) {
        u32 oldest = 0U;

        for (i = 1U; i < help.n; i++) {
            if (help.v[i].used < help.v[oldest].used)
                oldest = i;
        }
        entry_drop(&help.v[oldest]);
        help.v[oldest] = help.v[help.n - 1U];
        (void)memset(&help.v[help.n - 1U], 0, sizeof(help.v[0]));
        help.n--;
    }
    e = &help.v[help.n++];
    (void)memset(e, 0, sizeof(*e));
    e->real = yew_xstrdup(real);
    e->mtime = mtime;
    e->size = size;
    e->exec = yew_xstrdup(exec);
    e->used = ++help.clock;
    return e;
}

/* The names from the root down to `node` (not including the root). */
static u32 node_path(const YewSpecNode *node, const char **names, u32 cap)
{
    const YewSpecNode *n;
    u32 depth = 0U;
    u32 i;

    for (n = node; n != NULL && n->parent != NULL; n = n->parent) {
        if (depth == cap || n->name == NULL)
            return UINT32_MAX;
        names[depth++] = n->name;
    }
    for (i = 0U; i < depth / 2U; i++) {
        const char *t = names[i];

        names[i] = names[depth - 1U - i];
        names[depth - 1U - i] = t;
    }
    return depth;
}

static char *path_string(const char *const *names, u32 n)
{
    Bytebuf b;
    char *out;
    u32 i;

    bytebuf_init(&b);
    for (i = 0U; i < n; i++) {
        if (i != 0U)
            bytebuf_push_u8(&b, ' ');
        bytebuf_append(&b, names[i], strlen(names[i]));
    }
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

static bool entry_loaded(const HelpEntry *e, const char *path)
{
    u32 i;

    for (i = 0U; i < e->n_loaded; i++) {
        if (strcmp(e->loaded[i].path, path) == 0)
            return true;
    }
    return false;
}

/* The node at `names` under the entry's root, or NULL. */
static YewSpecNode *entry_node(HelpEntry *e, const char *const *names,
                               u32 n)
{
    const YewSpecNode *node = yew_compspec_root(e->root);
    u32 i;

    for (i = 0U; node != NULL && i < n; i++) {
        const YewSpecNode *next = NULL;
        u32 k;

        for (k = 0U; k < node->n_subs; k++) {
            if (strcmp(node->subs[k].name, names[i]) == 0) {
                next = &node->subs[k];
                break;
            }
        }
        node = next;
    }
    /* The tree is ours (entry-owned arenas); only a graft writes it. */
    return (YewSpecNode *)node;
}

/*
 * §4: hang a subcommand's own help under its node.  The node keeps its
 * name, aliases and the description its parent gave it; it gains the
 * child's flags, arguments and subcommands, whose parent pointers are
 * moved onto it so ancestors' `global` flags still reach them.
 * Takes `child` (NULL records a negative answer).
 */
static void entry_graft(HelpEntry *e, const char *const *names, u32 n,
                        YewCompSpec *child)
{
    YewSpecNode *node = entry_node(e, names, n);
    char *path = path_string(names, n);

    if (node == NULL || entry_loaded(e, path)) {
        yew_compspec_free(child);
        yew_xfree(path);
        return;
    }
    if (e->n_loaded == e->cap_loaded) {
        e->cap_loaded = e->cap_loaded == 0U ? 4U : e->cap_loaded * 2U;
        e->loaded = yew_xrealloc(e->loaded,
                                 e->cap_loaded * sizeof(*e->loaded));
    }
    e->loaded[e->n_loaded].path = path;
    e->loaded[e->n_loaded].spec = child;
    e->n_loaded++;
    if (child != NULL) {
        YewSpecNode *croot = yew_compspec_root_mut(child);
        u32 k;

        node->flags = croot->flags;
        node->n_flags = croot->n_flags;
        node->args = croot->args;
        node->n_args = croot->n_args;
        node->subs = croot->subs;
        node->n_subs = croot->n_subs;
        for (k = 0U; k < croot->n_subs; k++)
            ((YewSpecNode *)&croot->subs[k])->parent = node;
    }
}

/* ================================================================ */
/* Resolution of the command word                                   */
/* ================================================================ */

/*
 * 57.23's EXEC rule: the first $PATH element holding an executable
 * regular file of that name wins.  Relative (and empty) elements are
 * skipped -- `.` on $PATH would run whatever binary the workspace holds.
 * A word with a `/` is the user's own explicit path, from the directory
 * `:!` commands run in.
 */
static char *resolve_exec(Ed *ed, const char *word)
{
    const char *env;
    const char *p;

    if (strchr(word, '/') != NULL) {
        struct stat st;
        char *path = word[0] == '/' ? yew_xstrdup(word)
                                    : join_path(yew_ws_root(ed), word);

        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            access(path, X_OK) == 0)
            return path;
        yew_xfree(path);
        return NULL;
    }
    env = getenv("PATH");
    if (env == NULL)
        return NULL;
    p = env;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon == NULL ? strlen(p) : (size_t)(colon - p);

        if (n != 0U && p[0] == '/') {
            char *dir = yew_xmalloc(n + 1U);
            char *path;
            struct stat st;

            (void)memcpy(dir, p, n);
            dir[n] = '\0';
            path = join_path(dir, word);
            yew_xfree(dir);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                access(path, X_OK) == 0)
                return path;
            yew_xfree(path);
        }
        if (colon == NULL)
            return NULL;
        p = colon + 1;
    }
}

static const HelpMemo *memo_for(Ed *ed, const char *word)
{
    HelpMemo *m;
    struct stat st;
    u32 i;

    for (i = 0U; i < help.n_memo; i++) {
        if (strcmp(help.memo[i].word, word) == 0)
            return &help.memo[i];
    }
    if (help.n_memo == HELP_MEMO_MAX) {
        yew_xfree(help.memo[0].word);
        yew_xfree(help.memo[0].exec);
        yew_xfree(help.memo[0].real);
        for (i = 0U; i + 1U < help.n_memo; i++)
            help.memo[i] = help.memo[i + 1U];
        help.n_memo--;
    }
    m = &help.memo[help.n_memo++];
    (void)memset(m, 0, sizeof(*m));
    m->word = yew_xstrdup(word);
    m->exec = resolve_exec(ed, word);
    if (m->exec != NULL) {
        m->real = yew_xrealpath(m->exec);
        if (m->real == NULL || stat(m->real, &st) != 0) {
            yew_xfree(m->exec);
            yew_xfree(m->real);
            m->exec = NULL;
            m->real = NULL;
        } else {
            m->mtime = (i64)st.st_mtime;
            m->size = (i64)st.st_size;
            m->kind = yew_comphelp_exe_kind(m->real);
        }
    }
    return m;
}

/* Every §1 rule a (resolved) command must pass before it may be asked. */
static bool may_ask(Ed *ed, const char *word, const HelpMemo *m)
{
    YewHelpPolicy policy = yew_comphelp_policy(ed);

    if (policy == YEW_HELP_POLICY_OFF || m->exec == NULL)
        return false;
    if (yew_comphelp_denied(base_of(word)) ||
        yew_comphelp_denied(base_of(m->exec)) ||
        yew_comphelp_denied(base_of(m->real)))
        return false;
    if (!yew_comphelp_kind_allowed(policy, m->kind))
        return false;
    /* A spec wins (57.24), whichever of its names reaches it. */
    if (yew_compspec_get(ed, base_of(word)) != NULL ||
        yew_compspec_get(ed, base_of(m->real)) != NULL)
        return false;
    return true;
}

static void queue_request(const char *key, const char *exec,
                          const char *real, i64 mtime, i64 size,
                          const char *const *subs, u32 n_subs)
{
    HelpReq *r;
    u32 i;

    if (req_find(key) >= 0)
        return;
    if (help.nq == HELP_QUEUE_MAX) {
        /* The oldest unspawned request is the least likely still
         * wanted. */
        for (i = 0U; i < help.nq; i++) {
            if (!help.q[i].inflight) {
                req_drop_at(i);
                break;
            }
        }
        if (help.nq == HELP_QUEUE_MAX)
            return;
    }
    r = &help.q[help.nq++];
    (void)memset(r, 0, sizeof(*r));
    (void)memcpy(r->key, key, 17U);
    r->exec = yew_xstrdup(exec);
    r->real = yew_xstrdup(real);
    r->mtime = mtime;
    r->size = size;
    if (n_subs != 0U) {
        r->subs = yew_xcalloc(n_subs, sizeof(*r->subs));
        for (i = 0U; i < n_subs; i++)
            r->subs[i] = yew_xstrdup(subs[i]);
    }
    r->n_subs = n_subs;
}

bool yew_comphelp_lookup(Ed *ed, const char *word, YewHelpLookup *out)
{
    const HelpMemo *m;
    HelpEntry *e;
    YewCompSpec *spec;
    char key[17];
    bool ok;

    if (out == NULL)
        return false;
    (void)memset(out, 0, sizeof(*out));
    if (ed == NULL || word == NULL || word[0] == '\0' ||
        yew_comphelp_policy(ed) == YEW_HELP_POLICY_OFF ||
        yew_comphelp_denied(base_of(word)))
        return false;
    m = memo_for(ed, word);
    if (!may_ask(ed, word, m))
        return false;
    e = entry_find(m->real, m->mtime, m->size);
    if (e != NULL) {
        yew_xfree(e->exec);
        e->exec = yew_xstrdup(m->exec);
        out->spec = e->root;
        return e->root != NULL;
    }
    yew_comphelp_key(m->real, m->mtime, m->size, NULL, 0U, key);
    cache_read(key, m->real, m->mtime, m->size, NULL, 0U, &spec, &ok);
    if (ok) {
        e = entry_add(m->real, m->mtime, m->size, m->exec);
        e->root = spec;
        out->spec = spec;
        return spec != NULL;
    }
    queue_request(key, m->exec, m->real, m->mtime, m->size, NULL, 0U);
    (void)memcpy(out->key, key, sizeof(key));
    out->pending = yew_comphelp_awaiting(key);
    return false;
}

YewHelpDescend yew_comphelp_descend(Ed *ed, const YewCompSpec *spec,
                                    const YewSpecNode *node, char key[17])
{
    HelpEntry *e = entry_of_spec(spec);
    const char *names[YEW_COMPHELP_DEPTH_MAX];
    YewCompSpec *child;
    char *path;
    u32 depth;
    bool ok;

    if (key != NULL)
        key[0] = '\0';
    if (e == NULL || node == NULL)
        return YEW_HELP_DESCEND_NONE;
    depth = node_path(node, names, YEW_ARRAY_LEN(names));
    /* The root, or deeper than we ever ask: what is there is all. */
    if (depth == 0U || depth == UINT32_MAX)
        return YEW_HELP_DESCEND_READY;
    path = path_string(names, depth);
    ok = entry_loaded(e, path);
    yew_xfree(path);
    if (ok)
        return YEW_HELP_DESCEND_READY;
    {
        char k[17];

        yew_comphelp_key(e->real, e->mtime, e->size, names, depth, k);
        cache_read(k, e->real, e->mtime, e->size, names, depth, &child, &ok);
        if (ok) {
            entry_graft(e, names, depth, child);
            return YEW_HELP_DESCEND_CHANGED;
        }
        /* Policy may have changed since the root was learned. */
        if (yew_comphelp_policy(ed) != YEW_HELP_POLICY_OFF)
            queue_request(k, e->exec, e->real, e->mtime, e->size, names,
                          depth);
        if (key != NULL && yew_comphelp_awaiting(k))
            (void)memcpy(key, k, 17U);
    }
    return YEW_HELP_DESCEND_PENDING;
}

/* ================================================================ */
/* §2, §6: the jobs                                                  */
/* ================================================================ */

static void store_answer(Ed *ed, const HelpReq *r, YewCompSpec *spec)
{
    HelpEntry *e;

    (void)ed;
    if (r->n_subs == 0U) {
        e = entry_add(r->real, r->mtime, r->size, r->exec);
        e->root = spec;
        return;
    }
    e = entry_find(r->real, r->mtime, r->size);
    if (e == NULL || e->root == NULL) {
        /* The root left memory meanwhile; the disk has the answer. */
        yew_compspec_free(spec);
        return;
    }
    entry_graft(e, (const char *const *)r->subs, r->n_subs, spec);
}

static void help_complete(void *owner_ptr, Ed *ed, const YewJob *job)
{
    HelpOwner *o = owner_ptr;
    const char *words[1U + YEW_COMPHELP_DEPTH_MAX];
    char origin[PATH_MAX + 64];
    YewCompSpec *spec;
    HelpReq *r;
    int at;
    u32 i;

    o->completed = true;
    at = req_find(o->key);
    if (at < 0 || !help.q[at].inflight)
        return; /* reset underneath it: nothing asked for this any more */
    r = &help.q[at];
    words[0] = base_of(r->exec);
    for (i = 0U; i < r->n_subs && i < YEW_COMPHELP_DEPTH_MAX; i++)
        words[i + 1U] = r->subs[i];
    (void)snprintf(origin, sizeof(origin), "help:%s", r->real);
    /* §2: stdout, else stderr (Go's flag package and BSD tools print
     * usage there).  The exit status says nothing: many tools exit 1 or
     * 2 after printing help. */
    spec = yew_comphelp_parse(origin, words, 1U + i,
                              job->collect.data == NULL
                                  ? ""
                                  : (const char *)job->collect.data,
                              job->collect.len);
    if (spec == NULL)
        spec = yew_comphelp_parse(origin, words, 1U + i,
                                  job->collect_err.data == NULL
                                      ? ""
                                      : (const char *)job->collect_err.data,
                                  job->collect_err.len);
    cache_write(r, spec);
    store_answer(ed, r, spec);
    req_drop_at((u32)at);
    /* 57.24 §5.4: refilter an open menu that asked for this key; the
     * line itself is never touched. */
    yew_cmdline_compgen_arrived(ed, o->key);
}

static void help_destroy(void *owner_ptr)
{
    HelpOwner *o = owner_ptr;

    if (!o->completed) {
        /* Evicted for the user's own job, or torn down: no answer, so
         * nothing cached; a later lookup may ask again. */
        int at = req_find(o->key);

        if (at >= 0 && help.q[at].inflight)
            req_drop_at((u32)at);
    }
    yew_xfree(o);
}

static const YewJobCallbackOps help_ops = {help_complete, help_destroy};

bool yew_comphelp_idle_ready(void)
{
    u32 i;
    u32 flying = yew_comphelp_inflight();

    if (flying >= HELP_INFLIGHT_MAX ||
        yew_compgen_inflight() + flying >= YEW_COMPGEN_MAX_INFLIGHT)
        return false;
    for (i = 0U; i < help.nq; i++) {
        if (!help.q[i].inflight)
            return true;
    }
    return false;
}

/* Re-check, at the moment of running, everything the lookup checked:
 * the file may have changed, the option may have. */
static bool still_allowed(Ed *ed, const HelpReq *r)
{
    YewHelpPolicy policy = yew_comphelp_policy(ed);
    struct stat st;
    char *real;
    bool same;

    if (policy == YEW_HELP_POLICY_OFF ||
        yew_comphelp_denied(base_of(r->exec)) ||
        yew_comphelp_denied(base_of(r->real)) ||
        yew_compspec_get(ed, base_of(r->real)) != NULL ||
        yew_compspec_get(ed, base_of(r->exec)) != NULL)
        return false;
    real = yew_xrealpath(r->exec);
    same = real != NULL && strcmp(real, r->real) == 0 &&
           stat(real, &st) == 0 && (i64)st.st_mtime == r->mtime &&
           (i64)st.st_size == r->size;
    yew_xfree(real);
    return same && yew_comphelp_kind_allowed(policy,
                                             yew_comphelp_exe_kind(r->real));
}

u32 yew_comphelp_idle(Ed *ed)
{
    static const char *const env_set[] = {
        "NO_COLOR=1", "TERM=dumb", "COLUMNS=200", "PAGER=cat",
        "MANPAGER=cat", "GIT_PAGER=cat", NULL};
    char *argv[YEW_COMPHELP_DEPTH_MAX + 3U];
    HelpReq *r = NULL;
    YewJobSpec spec;
    HelpOwner *owner;
    char err[256];
    u32 i;
    u32 id;

    if (ed == NULL || !yew_comphelp_idle_ready())
        return 0U;
    for (i = 0U; i < help.nq; i++) {
        if (!help.q[i].inflight) {
            r = &help.q[i];
            break;
        }
    }
    if (r == NULL)
        return 0U;
    if (!still_allowed(ed, r)) {
        req_drop_at(i);
        return 0U;
    }
    /* §2: the RESOLVED path, never the bare name -- $PATH could change
     * between resolution and spawn -- then only subcommand names the
     * parent's help listed, then --help. */
    argv[0] = r->exec;
    for (i = 0U; i < r->n_subs && i < YEW_COMPHELP_DEPTH_MAX; i++)
        argv[i + 1U] = r->subs[i];
    argv[i + 1U] = "--help";
    argv[i + 2U] = NULL;
    {
        Bytebuf b;
        u32 k;

        bytebuf_init(&b);
        for (k = 0U; argv[k] != NULL; k++)
            bytebuf_printf(&b, "%s%s", k == 0U ? "" : " ", argv[k]);
        (void)snprintf(help.last_argv, sizeof(help.last_argv), "%.*s",
                       (int)(b.len > 1000U ? 1000U : b.len),
                       b.data == NULL ? "" : (const char *)b.data);
        bytebuf_free(&b);
    }
    owner = yew_xcalloc(1U, sizeof(*owner));
    (void)memcpy(owner->key, r->key, 17U);
    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = argv;
    /* A neutral, unwritable directory: a tool's --help has no business
     * with the workspace, and the answer is cached per executable, not
     * per directory. */
    spec.cwd = "/";
    spec.sink = YEW_SINK_CALLBACK;
    spec.internal = true;
    spec.evictable = true;
    spec.timeout_ms = help.timeout_ms > 0 ? help.timeout_ms
                                          : (i64)YEW_COMPHELP_TIMEOUT_MS;
    spec.collect_max = YEW_COMPHELP_COLLECT_MAX;
    spec.env_set = env_set;
    spec.display = base_of(r->exec);
    spec.callback_owner = owner;
    spec.callback_ops = &help_ops;
    /* No stdin source: the pipe closes at once, so a tool waiting on
     * input sees EOF and exits. */
    help.spawns++;
    id = yew_job_spawn(ed, &spec, err, sizeof(err));
    if (id == 0U) {
        /* Remember the failure for the session (not on disk: it was not
         * the tool's answer) so the next keystroke does not retry. */
        store_answer(ed, r, NULL);
        yew_xfree(owner);
        req_drop_at((u32)(r - help.q));
        return 0U;
    }
    r->inflight = true;
    r->job_id = id;
    return 1U;
}

/* ================================================================ */
/* §5: ed.shell.complete_forget                                      */
/* ================================================================ */

/* The `exe` of one cache file, heap-owned, or NULL when unreadable. */
static char *cache_file_exe(const char *path)
{
    Bytebuf src;
    HelpDoc d;
    FlValue root;
    FlValue v;
    char *out = NULL;

    bytebuf_init(&src);
    if (!read_small(path, &src)) {
        bytebuf_free(&src);
        return NULL;
    }
    doc_init(&d);
    root = fl_data_read(&d.vm, src.data == NULL ? "" : (const char *)src.data,
                        src.len, &d.dc);
    if (fl_diag_errors(&d.dc) == 0U && root.t == (u8)FL_MAP &&
        map_get_key((const FlMap *)root.as.o, "exe", &v) &&
        v.t == (u8)FL_STR) {
        const FlStr *s = (const FlStr *)v.as.o;

        out = yew_xmalloc((size_t)s->len + 1U);
        (void)memcpy(out, s->b, s->len);
        out[s->len] = '\0';
    }
    doc_free(&d);
    bytebuf_free(&src);
    return out;
}

i64 yew_comphelp_forget(Ed *ed, const char *name, char *err, size_t errsz)
{
    char *dir = yew_comphelp_cache_dir();
    char *real = NULL;
    bool all = name == NULL || name[0] == '\0';
    DIR *d;
    struct dirent *e;
    i64 removed = 0;
    u32 i;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (!all) {
        char *exec = resolve_exec(ed, name);

        if (exec != NULL)
            real = yew_xrealpath(exec);
        yew_xfree(exec);
    }
    /* Memory first: a forgotten tool must re-learn in THIS session. */
    i = 0U;
    while (i < help.n) {
        HelpEntry *h = &help.v[i];

        if (all || strcmp(base_of(h->real), name) == 0 ||
            (real != NULL && strcmp(h->real, real) == 0)) {
            entry_drop(h);
            help.v[i] = help.v[help.n - 1U];
            (void)memset(&help.v[help.n - 1U], 0, sizeof(help.v[0]));
            help.n--;
        } else {
            i++;
        }
    }
    memo_clear();
    if (dir == NULL) {
        yew_xfree(real);
        return 0;
    }
    d = opendir(dir);
    if (d == NULL) {
        int saved = errno;

        yew_xfree(dir);
        yew_xfree(real);
        if (saved == ENOENT)
            return 0; /* never written: nothing to forget */
        if (err != NULL && errsz != 0U)
            (void)snprintf(err, errsz, "cannot read the help cache: %s",
                           strerror(saved));
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char *path;
        bool match = all;

        if (!cache_file_name(e->d_name))
            continue;
        path = join_path(dir, e->d_name);
        if (!match) {
            char *exe = cache_file_exe(path);

            match = exe != NULL &&
                    (strcmp(base_of(exe), name) == 0 ||
                     (real != NULL && strcmp(exe, real) == 0));
            yew_xfree(exe);
        }
        if (match && unlink(path) == 0)
            removed++;
        yew_xfree(path);
    }
    (void)closedir(d);
    yew_xfree(dir);
    yew_xfree(real);
    return removed;
}

CmdStatus yew_comphelp_run_forget(CmdCtx *cx)
{
    char name[256];
    char err[256];
    i64 n;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    name[0] = '\0';
    if (cx->sarg != NULL && cx->sarg_len != 0U) {
        size_t lo = 0U;
        size_t hi = cx->sarg_len;

        while (lo < hi && hp_blank(cx->sarg[lo]))
            lo++;
        while (hi > lo && hp_blank(cx->sarg[hi - 1U]))
            hi--;
        if (hi - lo >= sizeof(name)) {
            yew_msg(cx->ed, YEW_MSG_ERROR,
                    "ed.shell.complete_forget: name too long");
            return YEW_CMD_ERR_ARG;
        }
        (void)memcpy(name, cx->sarg + lo, hi - lo);
        name[hi - lo] = '\0';
    }
    n = yew_comphelp_forget(cx->ed, name, err, sizeof(err));
    if (n < 0) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "ed.shell.complete_forget: %s", err);
        return YEW_CMD_ERR_IO;
    }
    if (name[0] == '\0')
        yew_msg(cx->ed, YEW_MSG_INFO,
                "forgot all learned help (%lld cache file%s)",
                (long long)n, n == 1 ? "" : "s");
    else
        yew_msg(cx->ed, YEW_MSG_INFO,
                "forgot learned help for %s (%lld cache file%s)", name,
                (long long)n, n == 1 ? "" : "s");
    return YEW_CMD_OK;
}
