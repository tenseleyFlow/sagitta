#include "search/searchui.h"
#include "ui/cmdparse.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/select.h"
#include "unicode/coords.h"
#include "util/buf.h"

typedef struct Parser {
    Ed *ed;
    const char *line;
    size_t len;
    size_t at;
    Arena *arena;
    CmdErr *err;
} Parser;

typedef enum PartKind {
    PART_BARE,
    PART_SINGLE,
    PART_DOUBLE
} PartKind;

static void set_error(Parser *p, size_t lo, size_t hi, const char *fmt, ...)
{
    va_list ap;

    if (p->err == NULL || p->err->msg[0] != '\0')
        return;
    if (p->len != 0U) {
        if (lo >= p->len)
            lo = p->len - 1U;
        if (hi <= lo)
            hi = lo + 1U;
        if (hi > p->len)
            hi = p->len;
    } else {
        lo = 0U;
        hi = 0U;
    }
    p->err->tok_lo = lo > UINT32_MAX ? UINT32_MAX : (u32)lo;
    p->err->tok_hi = hi > UINT32_MAX ? UINT32_MAX : (u32)hi;
    va_start(ap, fmt);
    (void)vsnprintf(p->err->msg, sizeof(p->err->msg), fmt, ap);
    va_end(ap);
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

static void skip_ws(Parser *p)
{
    while (p->at < p->len && is_ws(p->line[p->at]))
        p->at++;
}

static TextBuf *active_text(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->win != NULL && ed->win->buf != NULL)
        return ed->win->buf->tb;
    return yew_ed_doc((Ed *)ed)->tb;
}

static Buffer *active_buffer(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->win != NULL && ed->win->buf != NULL)
        return ed->win->buf;
    return &ed->buffer;
}

static Win *active_win(Ed *ed)
{
    return ed == NULL ? NULL : ed->win;
}

static void append_text(Bytebuf *out, const TextBuf *tb, Span span)
{
    TextIter it;
    u64 left;

    if (tb == NULL || span.hi <= span.lo)
        return;
    left = span.hi - span.lo;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return;
    while (left != 0U) {
        const u8 *bytes;
        u64 avail;
        size_t take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &avail))
            break;
        if (avail > left)
            avail = left;
        take = avail > SIZE_MAX ? SIZE_MAX : (size_t)avail;
        bytebuf_append(out, bytes, take);
        left -= avail;
        if (left != 0U && !yew_textiter_advance(&it, tb))
            break;
    }
}

static const char *display_name(const Buffer *buf)
{
    const char *slash;

    if (buf == NULL || buf->path == NULL || buf->path[0] == '\0')
        return "[No Name]";
    slash = strrchr(buf->path, '/');
    return slash == NULL ? buf->path : slash + 1;
}

static void append_dirname(Bytebuf *out, const char *path)
{
    const char *slash;
    size_t n;

    slash = strrchr(path, '/');
    if (slash == NULL) {
        bytebuf_append(out, ".", 1U);
        return;
    }
    n = (size_t)(slash - path);
    while (n > 1U && path[n - 1U] == '/')
        n--;
    if (n == 0U)
        n = 1U;
    bytebuf_append(out, path, n);
}

static bool append_expansion(Parser *p, size_t *at, Bytebuf *out)
{
    Buffer *buf = active_buffer(p->ed);
    TextBuf *tb = active_text(p->ed);
    Win *win = active_win(p->ed);
    size_t lo = *at;
    char code;

    (*at)++;
    if (*at >= p->len || is_ws(p->line[*at]) || p->line[*at] == '"') {
        if (buf == NULL || buf->path == NULL || buf->path[0] == '\0') {
            set_error(p, lo, lo + 1U, "buffer has no file name");
            return false;
        }
        bytebuf_append(out, buf->path, strlen(buf->path));
        return true;
    }
    code = p->line[(*at)++];
    switch (code) {
    case '%':
        bytebuf_append(out, "%", 1U);
        return true;
    case 'h': {
        const char *root = p->ed != NULL && p->ed->ws.dir != NULL ?
                           p->ed->ws.dir : ".";
        bytebuf_append(out, root, strlen(root));
        return true;
    }
    case 'b': {
        const char *name = display_name(buf);
        bytebuf_append(out, name, strlen(name));
        return true;
    }
    case 'l':
    case 'c': {
        char number[32];
        u64 value = 1U;

        if (tb != NULL && win != NULL && win->cs.curs.len != 0U &&
            win->cs.primary < win->cs.curs.len) {
            const Cursor *cur = &win->cs.curs.data[win->cs.primary];
            LineNo line = yew_textbuf_line_of(tb, cur->pos);

            if (code == 'l')
                value = line.v + 1U;
            else
                value = yew_off_to_gcol(tb,
                                        yew_textbuf_line_span(tb, line),
                                        cur->pos).v + 1U;
        }
        (void)snprintf(number, sizeof(number), "%llu",
                       (unsigned long long)value);
        bytebuf_append(out, number, strlen(number));
        return true;
    }
    case 's':
    {
        size_t before = out->len;

        if (tb == NULL || win == NULL || win->cs.curs.len == 0U ||
            win->cs.primary >= win->cs.curs.len ||
            win->cs.curs.data[win->cs.primary].anchor.v ==
                win->cs.curs.data[win->cs.primary].pos.v) {
            set_error(p, lo, *at, "%%s needs a selection");
            return false;
        }
        append_text(out, tb,
                    yew_sel_span(win, &win->cs.curs.data[win->cs.primary]));
        if (memchr(out->data + before, '\0', out->len - before) != NULL) {
            set_error(p, lo, *at,
                      "NUL byte is not valid in a command line");
            return false;
        }
        return true;
    }
    case 'p':
        if (buf == NULL || buf->path == NULL || buf->path[0] == '\0') {
            set_error(p, lo, *at, "buffer has no file name");
            return false;
        }
        if (buf->meta.realpath == NULL || buf->meta.realpath[0] == '\0') {
            set_error(p, lo, *at, "buffer has no file name");
            return false;
        }
        bytebuf_append(out, buf->meta.realpath, strlen(buf->meta.realpath));
        return true;
    case 'd':
        if (buf == NULL || buf->path == NULL || buf->path[0] == '\0') {
            set_error(p, lo, *at, "buffer has no file name");
            return false;
        }
        append_dirname(out, buf->path);
        return true;
    default:
        set_error(p, lo, *at, "unknown expansion '%%%c'", code);
        return false;
    }
}

static bool append_escape(Parser *p, size_t *at, PartKind part,
                          bool tolerant, Bytebuf *out)
{
    size_t lo = *at;
    char c;

    (*at)++;
    if (*at >= p->len) {
        if (tolerant) {
            bytebuf_append(out, "\\", 1U);
            return true;
        }
        set_error(p, lo, lo + 1U, "unknown escape '\\\\'");
        return false;
    }
    c = p->line[(*at)++];
    if (part == PART_BARE) {
        bytebuf_append(out, &c, 1U);
        return true;
    }
    if (c == '\\' || c == '"' || c == '%') {
        bytebuf_append(out, &c, 1U);
        return true;
    }
    if (c == 'n') {
        bytebuf_append(out, "\n", 1U);
        return true;
    }
    if (c == 't') {
        bytebuf_append(out, "\t", 1U);
        return true;
    }
    set_error(p, lo, *at, "unknown escape '\\%c'", c);
    return false;
}

static bool parse_token(Parser *p, bool tolerant, size_t stop,
                        char **value, Span *tok)
{
    Bytebuf out;
    size_t start = p->at;
    bool any = false;

    bytebuf_init(&out);
    while (p->at < stop && !is_ws(p->line[p->at])) {
        char c = p->line[p->at];

        any = true;
        if (c == '\'') {
            size_t quote = p->at++;

            while (p->at < stop && p->line[p->at] != '\'') {
                bytebuf_append(&out, p->line + p->at, 1U);
                p->at++;
            }
            if (p->at == stop) {
                if (!tolerant) {
                    set_error(p, quote, quote + 1U, "unterminated '");
                    bytebuf_free(&out);
                    return false;
                }
            } else {
                p->at++;
            }
        } else if (c == '"') {
            size_t quote = p->at++;

            while (p->at < stop && p->line[p->at] != '"') {
                if (p->line[p->at] == '\\') {
                    if (!append_escape(p, &p->at, PART_DOUBLE, tolerant,
                                       &out)) {
                        bytebuf_free(&out);
                        return false;
                    }
                } else if (p->line[p->at] == '%') {
                    if (!append_expansion(p, &p->at, &out)) {
                        if (!tolerant) {
                            bytebuf_free(&out);
                            return false;
                        }
                    }
                } else {
                    bytebuf_append(&out, p->line + p->at, 1U);
                    p->at++;
                }
            }
            if (p->at == stop) {
                if (!tolerant) {
                    set_error(p, quote, quote + 1U, "unterminated \"");
                    bytebuf_free(&out);
                    return false;
                }
            } else {
                p->at++;
            }
        } else if (c == '\\') {
            if (!append_escape(p, &p->at, PART_BARE, tolerant, &out)) {
                bytebuf_free(&out);
                return false;
            }
        } else if (c == '%') {
            if (!append_expansion(p, &p->at, &out)) {
                if (!tolerant) {
                    bytebuf_free(&out);
                    return false;
                }
            }
        } else {
            bytebuf_append(&out, p->line + p->at, 1U);
            p->at++;
        }
    }
    if (!any) {
        bytebuf_free(&out);
        return false;
    }
    bytebuf_append(&out, "", 1U);
    *value = arena_strndup(p->arena, (const char *)out.data, out.len - 1U);
    *tok = (Span){start, p->at};
    bytebuf_free(&out);
    return true;
}

static bool parse_u64(const char *s, size_t len, size_t *at, u64 *out)
{
    u64 n = 0U;
    size_t start = *at;

    while (*at < len && isdigit((unsigned char)s[*at])) {
        u32 digit = (u32)(s[*at] - '0');

        if (n > (UINT64_MAX - digit) / 10U)
            n = UINT64_MAX;
        else
            n = n * 10U + digit;
        (*at)++;
    }
    if (*at == start)
        return false;
    *out = n;
    return true;
}

static bool cursor_line(Parser *p, i64 *line)
{
    TextBuf *tb = active_text(p->ed);
    Win *win = active_win(p->ed);

    if (tb == NULL || win == NULL || win->cs.curs.len == 0U ||
        win->cs.primary >= win->cs.curs.len)
        *line = 0;
    else
        *line = (i64)yew_textbuf_line_of(
            tb, win->cs.curs.data[win->cs.primary].pos).v;
    return true;
}

static bool parse_address(Parser *p, LineNo *out, Span *tok)
{
    TextBuf *tb = active_text(p->ed);
    size_t start = p->at;
    size_t base_end;
    u64 count = tb == NULL ? 1U : yew_textbuf_line_count(tb);
    i64 line;
    bool relative = false;

    if (p->at >= p->len)
        return false;
    if (isdigit((unsigned char)p->line[p->at])) {
        u64 display;

        (void)parse_u64(p->line, p->len, &p->at, &display);
        line = display == 0U ? -1 :
               display > (u64)INT64_MAX ? INT64_MAX :
               (i64)(display - 1U);
    } else if (p->line[p->at] == '.') {
        p->at++;
        (void)cursor_line(p, &line);
    } else if (p->line[p->at] == '$') {
        p->at++;
        line = count == 0U ? 0 : (i64)(count - 1U);
    } else if (p->line[p->at] == '+' || p->line[p->at] == '-') {
        (void)cursor_line(p, &line);
        relative = true;
    } else if (p->line[p->at] == '\'' && p->at + 1U < p->len &&
               p->line[p->at + 1U] >= 'a' &&
               p->line[p->at + 1U] <= 'z') {
        /* Sprint 21 §7: `'a` resolves through the buffer's 26-slot name
         * table.  An unset name is an error naming the mark, not a
         * silent line 0 — addressing the wrong line is worse than
         * refusing. */
        u8 name = (u8)p->line[p->at + 1U];
        Buffer *buf = active_buffer(p->ed);
        TextBuf *tb = active_text(p->ed);
        ByteOff at;

        p->at += 2U;
        if (buf == NULL || tb == NULL ||
            !yew_ed_mark_get(p->ed, buf, name, &at)) {
            set_error(p, start, p->at, "mark not set");
            return false;
        }
        line = (i64)yew_textbuf_line_of(tb, at).v;
    } else if (p->line[p->at] == '/' || p->line[p->at] == '?') {
        /*
         * Sprint 21: `/pat/` and `?pat?` address the next (previous)
         * line matching the pattern, searching from the cursor.  The
         * engine is Sprint 20's; no pattern logic lives here.
         */
        char close = p->line[p->at++];
        size_t pat_lo = p->at;
        size_t pat_hi;
        TextBuf *tb = active_text(p->ed);
        Arena arena;
        YewRe *re;
        i64 from_line = 0;

        while (p->at < p->len && p->line[p->at] != close) {
            if (p->line[p->at] == '\\' && p->at + 1U < p->len)
                p->at++;
            p->at++;
        }
        pat_hi = p->at;
        if (p->at < p->len)
            p->at++;
        if (tb == NULL || pat_hi <= pat_lo) {
            set_error(p, start, p->at, "empty pattern address");
            return false;
        }
        (void)cursor_line(p, &from_line);
        arena_init(&arena);
        re = yew_search_compile(&arena, p->line + pat_lo, pat_hi - pat_lo,
                                &p->ed->search_opts, NULL);
        if (re == NULL) {
            arena_free_all(&arena);
            set_error(p, start, p->at, "bad pattern in address");
            return false;
        }
        {
            YewReInput in = yew_re_input_textbuf(tb);
            YewReMatch m;
            u64 nlines = yew_textbuf_line_count(tb);
            bool found;

            (void)memset(&m, 0, sizeof(m));
            if (close == '/') {
                u64 next = (u64)from_line + 1U;
                u64 at = next < nlines
                         ? yew_textbuf_line_start(tb, LINENO(next)).v
                         : yew_textbuf_len(tb);

                found = yew_re_search(re, &in, BYTEOFF(at), &m);
                if (!found) /* wrap, as a search does */
                    found = yew_re_search(re, &in, BYTEOFF(0U), &m);
            } else {
                u64 at = yew_textbuf_line_start(tb,
                                                LINENO((u64)from_line)).v;

                found = yew_re_search_back(re, &in, BYTEOFF(at), &m);
                if (!found)
                    found = yew_re_search_back(re, &in,
                                               BYTEOFF(yew_textbuf_len(tb)),
                                               &m);
            }
            arena_free_all(&arena);
            if (!found) {
                set_error(p, start, p->at, "pattern not found");
                return false;
            }
            line = (i64)yew_textbuf_line_of(tb, BYTEOFF(m.g[0].lo)).v;
        }
    } else {
        return false;
    }
    base_end = p->at;
    do {
        char sign;
        u64 amount = 1U;

        if (relative) {
            sign = p->line[p->at++];
            relative = false;
        } else if (p->at < p->len &&
                   (p->line[p->at] == '+' || p->line[p->at] == '-')) {
            sign = p->line[p->at++];
        } else {
            break;
        }
        if (p->at < p->len && isdigit((unsigned char)p->line[p->at]))
            (void)parse_u64(p->line, p->len, &p->at, &amount);
        if (amount > (u64)INT64_MAX)
            line = sign == '+' ? INT64_MAX : INT64_MIN;
        else if (sign == '+')
            line = line > INT64_MAX - (i64)amount ? INT64_MAX :
                   line + (i64)amount;
        else
            line = line < INT64_MIN + (i64)amount ? INT64_MIN :
                   line - (i64)amount;
    } while (p->at < p->len);
    if (p->at == start)
        p->at = base_end;
    *tok = (Span){start, p->at};
    if (line < 0 || (u64)line >= count) {
        u64 shown = line < 0 ? 0U : (u64)line + 1U;

        set_error(p, start, p->at,
                  "line %llu past end of buffer (%llu lines)",
                  (unsigned long long)shown, (unsigned long long)count);
        return false;
    }
    *out = LINENO((u64)line);
    return true;
}

static bool parse_range(Parser *p, CmdRange *range)
{
    TextBuf *tb = active_text(p->ed);
    Win *win = active_win(p->ed);
    size_t start = p->at;
    Span lo_tok;
    Span hi_tok;

    *range = (CmdRange){YEW_RANGE_NONE, LINENO(0U), LINENO(0U), false,
                        {0U, 0U}};
    if (p->at >= p->len)
        return true;
    if (p->line[p->at] == '%') {
        u64 count = tb == NULL ? 1U : yew_textbuf_line_count(tb);

        p->at++;
        range->kind = YEW_RANGE_BUFFER;
        range->lo = LINENO(0U);
        range->hi = LINENO(count == 0U ? 0U : count - 1U);
        range->given = true;
        range->tok = (Span){start, p->at};
        return true;
    }
    if (p->at + 5U <= p->len &&
        memcmp(p->line + p->at, "'<,'>", 5U) == 0) {
        const Cursor *cur;
        LineNo a;
        LineNo b;

        p->at += 5U;
        if (tb == NULL || win == NULL || win->cs.curs.len == 0U ||
            win->cs.primary >= win->cs.curs.len ||
            win->cs.curs.data[win->cs.primary].anchor.v ==
                win->cs.curs.data[win->cs.primary].pos.v) {
            set_error(p, start, p->at, "'<,'> needs a selection");
            return false;
        }
        cur = &win->cs.curs.data[win->cs.primary];
        a = yew_textbuf_line_of(tb, cur->anchor);
        b = yew_textbuf_line_of(tb, cur->pos);
        range->kind = YEW_RANGE_SELECTION;
        range->lo = a.v < b.v ? a : b;
        range->hi = a.v > b.v ? a : b;
        range->given = true;
        range->tok = (Span){start, p->at};
        return true;
    }
    if (!parse_address(p, &range->lo, &lo_tok)) {
        if (p->err != NULL && p->err->msg[0] != '\0')
            return false;
        p->at = start;
        return true;
    }
    range->hi = range->lo;
    range->kind = YEW_RANGE_LINES;
    range->given = true;
    if (p->at < p->len && p->line[p->at] == ',') {
        p->at++;
        if (!parse_address(p, &range->hi, &hi_tok)) {
            if (p->err != NULL && p->err->msg[0] == '\0')
                set_error(p, p->at == p->len ? p->at - 1U : p->at,
                          p->at, "missing range address");
            return false;
        }
    }
    range->tok = (Span){start, p->at};
    if (range->lo.v > range->hi.v) {
        set_error(p, start, p->at, "backwards range (%llu,%llu)",
                  (unsigned long long)(range->lo.v + 1U),
                  (unsigned long long)(range->hi.v + 1U));
        return false;
    }
    return true;
}

static const char *short_name(const char *name)
{
    return strncmp(name, "ed.", 3U) == 0 ? name + 3U : name;
}

static CmdId resolve_name(Parser *p, const char *name, Span tok)
{
    CmdId exact_abbrev = YEW_CMD_NONE;
    CmdId exact_full = YEW_CMD_NONE;
    CmdId prefix = YEW_CMD_NONE;
    const char *abbrev_matches[6];
    const char *matches[6];
    u32 nabbrev = 0U;
    u32 nmatch = 0U;
    u32 i;

    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);
        CmdId id = {i + 1U};
        const CmdEntry *entry = yew_cmd_entry(id);
        const char *candidate;

        if (desc == NULL || entry == NULL ||
            (desc->flags & YEW_CMD_INTERNAL) != 0U)
            continue;
        candidate = short_name(desc->name);
        if (entry->abbrev != NULL && strcmp(entry->abbrev, name) == 0) {
            if (nabbrev < YEW_ARRAY_LEN(abbrev_matches))
                abbrev_matches[nabbrev] = candidate;
            nabbrev++;
            exact_abbrev = id;
        }
        if ((strcmp(candidate, name) == 0 || strcmp(desc->name, name) == 0) &&
            exact_full.v == 0U)
            exact_full = id;
        if (strncmp(candidate, name, strlen(name)) == 0) {
            if (nmatch < YEW_ARRAY_LEN(matches))
                matches[nmatch] = candidate;
            nmatch++;
            prefix = id;
        }
    }
    if (nabbrev == 1U)
        return exact_abbrev;
    if (nabbrev > 1U) {
        Bytebuf msg;
        u32 shown = nabbrev < 6U ? nabbrev : 6U;

        bytebuf_init(&msg);
        bytebuf_append(&msg, "ambiguous: ", 11U);
        for (i = 0U; i < shown; i++) {
            if (i != 0U)
                bytebuf_append(&msg, ", ", 2U);
            bytebuf_append(&msg, abbrev_matches[i],
                           strlen(abbrev_matches[i]));
        }
        bytebuf_append(&msg, "", 1U);
        set_error(p, (size_t)tok.lo, (size_t)tok.hi, "%s",
                  (const char *)msg.data);
        bytebuf_free(&msg);
        return YEW_CMD_NONE;
    }
    if (exact_full.v != 0U)
        return exact_full;
    if (nmatch == 1U)
        return prefix;
    if (nmatch > 1U) {
        Bytebuf msg;
        u32 shown = nmatch < 6U ? nmatch : 6U;

        bytebuf_init(&msg);
        bytebuf_append(&msg, "ambiguous: ", 11U);
        for (i = 0U; i < shown; i++) {
            if (i != 0U)
                bytebuf_append(&msg, ", ", 2U);
            bytebuf_append(&msg, matches[i], strlen(matches[i]));
        }
        bytebuf_append(&msg, "", 1U);
        set_error(p, (size_t)tok.lo, (size_t)tok.hi, "%s",
                  (const char *)msg.data);
        bytebuf_free(&msg);
    } else {
        set_error(p, (size_t)tok.lo, (size_t)tok.hi,
                  "unknown command '%s' (try Tab)", name);
    }
    return YEW_CMD_NONE;
}

static const char *command_label(const CmdDesc *desc)
{
    return desc == NULL ? "" : short_name(desc->name);
}

static bool apply_policy(Parser *p, const CmdEntry *entry, CmdRange *range,
                         Span name_tok)
{
    TextBuf *tb = active_text(p->ed);
    u64 count = tb == NULL ? 1U : yew_textbuf_line_count(tb);
    i64 current = 0;
    const char *name = command_label(&entry->cmd);

    if (range->given) {
        if (entry->range_policy == YEW_RP_FORBID) {
            set_error(p, (size_t)range->tok.lo, (size_t)range->tok.hi,
                      ":%s takes no range", name);
            return false;
        }
        return true;
    }
    if (entry->range_policy == YEW_RP_REQUIRED) {
        set_error(p, (size_t)name_tok.lo, (size_t)name_tok.hi,
                  ":%s requires a range", name);
        return false;
    }
    if (entry->range_policy == YEW_RP_LINE) {
        (void)cursor_line(p, &current);
        range->kind = YEW_RANGE_LINES;
        range->lo = LINENO((u64)current);
        range->hi = range->lo;
    } else if (entry->range_policy == YEW_RP_BUFFER) {
        range->kind = YEW_RANGE_BUFFER;
        range->lo = LINENO(0U);
        range->hi = LINENO(count == 0U ? 0U : count - 1U);
    }
    return true;
}

static void argspec_bounds(const CmdEntry *entry, u32 *min, u32 *max)
{
    const char *spec = entry->argspec == NULL ? "" : entry->argspec;
    size_t n = strlen(spec);

    if (n == 0U) {
        *min = 0U;
        *max = 0U;
    } else if (n > 1U || spec[n - 1U] == '*') {
        bool repeat = spec[n - 1U] == '*';

        *min = (u32)(repeat ? n - 1U : n);
        *max = repeat ? UINT32_MAX : (u32)n;
    } else {
        switch (entry->cmd.arity) {
        case YEW_ARITY_NONE:
            *min = 0U;
            *max = 0U;
            break;
        case YEW_ARITY_OPT_INT:
        case YEW_ARITY_OPT_STR:
            *min = 0U;
            *max = 1U;
            break;
        default:
            *min = 1U;
            *max = 1U;
            break;
        }
    }
}

static bool validate_arity(Parser *p, const CmdEntry *entry, u32 nargs,
                           const Span *tokens, Span name_tok)
{
    u32 min;
    u32 max;
    const char *name = command_label(&entry->cmd);

    argspec_bounds(entry, &min, &max);
    if (nargs < min) {
        set_error(p, (size_t)name_tok.lo, (size_t)name_tok.hi,
                  ":%s requires %u argument%s", name, min,
                  min == 1U ? "" : "s");
        return false;
    }
    if (nargs > max) {
        Span tok = tokens[max + 1U];

        set_error(p, (size_t)tok.lo, (size_t)tok.hi,
                  max == 0U ? ":%s takes no arguments" :
                              ":%s takes %u argument%s",
                  name, max, max == 1U ? "" : "s");
        return false;
    }
    return true;
}

/* `:!cmd` and `:<range>!cmd`.  Everything after the bang is ONE argument,
 * taken verbatim: a shell command is not a token list, and splitting it
 * here would change what the user typed. */
static bool finish_bang(Parser *p, CmdParse *out, const char *cmdname)
{
    size_t rest = p->at + 1U;
    size_t n = p->len - rest;

    out->command = yew_cmd_lookup(cmdname, (u32)strlen(cmdname));
    if (out->command.v == 0U) {
        set_error(p, p->at, p->at + 1U, "%s is not registered", cmdname);
        return false;
    }
    out->name_tok = (Span){p->at, p->at + 1U};
    while (n != 0U && is_ws(p->line[rest])) {
        rest++;
        n--;
    }
    if (n == 0U) {
        set_error(p, p->at, p->at + 1U, "%s needs a command", cmdname);
        return false;
    }
    /* argv[0] is the command NAME and arguments start at index 1 — that is
     * the contract yew_ed_invoke_parsed reads.  Putting the shell command
     * at index 0 makes an ARITY_STR command see no argument at all. */
    out->argv.v = arena_alloc(p->arena, 2U * sizeof(char *),
                              sizeof(char *));
    out->argv.v[0] = arena_strdup(p->arena, cmdname);
    out->argv.v[1] = arena_strndup(p->arena, p->line + rest, n);
    out->argv.n = 2U;
    out->arg_tok = arena_alloc(p->arena, 2U * sizeof(Span), sizeof(Span));
    out->arg_tok[0] = out->name_tok;
    out->arg_tok[1] = (Span){rest, p->len};
    return true;
}

/*
 * `:s/pat/rep/flags` has no space after the command name, so the normal
 * grammar — name, whitespace, arguments — never reaches it.  The whole
 * body from the delimiter onward is ONE opaque argument, split on the
 * delimiter by search_cmds.c; the tokenizer must not try to understand
 * a `/` inside a regex.  Same shape as `:!`, for the same reason.
 */
static bool finish_body(Parser *p, CmdParse *out, const char *cmdname,
                        Span name_tok)
{
    size_t rest = p->at;
    size_t n = p->len - rest;

    out->command = yew_cmd_lookup(cmdname, (u32)strlen(cmdname));
    if (out->command.v == 0U) {
        set_error(p, name_tok.lo, name_tok.hi, "%s is not registered",
                  cmdname);
        return false;
    }
    out->name_tok = name_tok;
    out->argv.v = arena_alloc(p->arena, 2U * sizeof(char *),
                              sizeof(char *));
    out->argv.v[0] = arena_strdup(p->arena, cmdname);
    out->argv.v[1] = arena_strndup(p->arena, p->line + rest, n);
    out->argv.n = 2U;
    out->arg_tok = arena_alloc(p->arena, 2U * sizeof(Span), sizeof(Span));
    out->arg_tok[0] = out->name_tok;
    out->arg_tok[1] = (Span){rest, p->len};
    return true;
}

/* `:fl` owns the rest of the line as source.  Tokenizing it as E-mode
 * arguments would split ordinary Fletch expressions at every space and
 * require shell-style quoting around a language that already has strings. */
static bool finish_rest(Parser *p, CmdParse *out, const char *cmdname,
                        Span name_tok)
{
    skip_ws(p);
    if (p->at == p->len) {
        set_error(p, (size_t)name_tok.lo, (size_t)name_tok.hi,
                  ":fl needs Fletch source");
        return false;
    }
    return finish_body(p, out, cmdname, name_tok);
}

bool yew_cmd_parse(Ed *ed, const char *line, size_t len, Arena *a,
                   CmdParse *out)
{
    Parser p;
    char **values;
    Span *tokens;
    size_t cap;
    size_t name_start;
    char *name;
    u32 n = 0U;
    CmdId id;
    const CmdEntry *entry;

    if (line == NULL || a == NULL || out == NULL)
        return false;
    memset(out, 0, sizeof(*out));
    p = (Parser){ed, line, len, 0U, a, &out->err};
    {
        const char *nul = memchr(line, '\0', len);

        if (nul != NULL) {
            size_t at = (size_t)(nul - line);

            set_error(&p, at, at + 1U,
                      "NUL byte is not valid in a command line");
            return false;
        }
    }
    skip_ws(&p);
    if (p.at < len && line[p.at] == ':') {
        p.at++;
        skip_ws(&p);
    }
    if (p.at < len && line[p.at] == '!')
        return finish_bang(&p, out, "ed.shell.run");
    if (!parse_range(&p, &out->range))
        return false;
    skip_ws(&p);
    if (p.at < len && line[p.at] == '!')
        return finish_bang(&p, out, "ed.shell.run");
    name_start = p.at;
    if (p.at >= len ||
        !(isalpha((unsigned char)line[p.at]) || line[p.at] == '_')) {
        set_error(&p, p.at, p.at + 1U, "unknown command '' (try Tab)");
        return false;
    }
    p.at++;
    while (p.at < len &&
           (isalnum((unsigned char)line[p.at]) || line[p.at] == '_' ||
            line[p.at] == '.'))
        p.at++;
    out->name_tok = (Span){name_start, p.at};
    name = arena_strndup(a, line + name_start, p.at - name_start);
    if (p.at < len && line[p.at] == '!') {
        out->bang = true;
        p.at++;
        out->name_tok.hi = p.at;
    }
    if (p.at < len && !is_ws(line[p.at])) {
        if (strcmp(name, "s") == 0)
            return finish_body(&p, out, "ed.search.replace",
                               out->name_tok);
        if (strcmp(name, "g") == 0)
            return finish_body(&p, out, "ed.search.global",
                               out->name_tok);
        set_error(&p, name_start, p.at + 1U,
                  "unknown command '%s' (try Tab)", name);
        return false;
    }
    if (strcmp(name, "fl") == 0)
        return finish_rest(&p, out, "ed.fl.eval", out->name_tok);
    if (strcmp(name, "macro") == 0) {
        char *verb;
        Span verb_tok;
        Bytebuf qualified;

        skip_ws(&p);
        if (p.at == len || !parse_token(&p, false, len, &verb, &verb_tok)) {
            set_error(&p, name_start, p.at,
                      "macro requires edit, name, or reload");
            return false;
        }
        if (strcmp(verb, "edit") != 0 && strcmp(verb, "name") != 0 &&
            strcmp(verb, "reload") != 0) {
            set_error(&p, (size_t)verb_tok.lo, (size_t)verb_tok.hi,
                      "unknown macro action '%s'", verb);
            return false;
        }
        bytebuf_init(&qualified);
        bytebuf_append(&qualified, "ed.macro.", 9U);
        bytebuf_append(&qualified, verb, strlen(verb));
        bytebuf_push_u8(&qualified, 0U);
        name = arena_strdup(a, (const char *)qualified.data);
        bytebuf_free(&qualified);
        out->name_tok.hi = verb_tok.hi;
    }
    if (strcmp(name, "r") == 0) {
        size_t bang = p.at;

        while (bang < len && is_ws(line[bang]))
            bang++;
        if (bang < len && line[bang] == '!') {
            p.at = bang;
            return finish_bang(&p, out, "ed.shell.read");
        }
    }
    /* The traditional spelling is a compatibility alias for Fletch's
     * complete config reload; it does not accept a legacy file operand. */
    if (strcmp(name, "source") == 0)
        name = arena_strdup(a, "config.reload");
    id = resolve_name(&p, name, out->name_tok);
    if (id.v == 0U)
        return false;
    out->command = id;
    entry = yew_cmd_entry(id);
    if (entry == NULL)
        return false;
    cap = len / 2U + 2U;
    values = arena_alloc(a, cap * sizeof(*values), _Alignof(char *));
    tokens = arena_alloc(a, cap * sizeof(*tokens), _Alignof(Span));
    values[n] = arena_strdup(a, entry->cmd.name);
    tokens[n++] = out->name_tok;
    for (;;) {
        char *arg;
        Span tok;

        skip_ws(&p);
        if (p.at == len)
            break;
        if (!parse_token(&p, false, len, &arg, &tok))
            return false;
        values[n] = arg;
        tokens[n] = tok;
        n++;
    }
    if (!validate_arity(&p, entry, n - 1U, tokens, out->name_tok) ||
        !apply_policy(&p, entry, &out->range, out->name_tok))
        return false;
    out->argv = (CmdArgv){values, n};
    out->arg_tok = tokens;
    return true;
}

static bool loose_name(Parser *p, size_t cursor, char **name, Span *tok,
                       CmdRange *range)
{
    size_t start;

    skip_ws(p);
    if (p->at < p->len && p->line[p->at] == ':') {
        p->at++;
        skip_ws(p);
    }
    if (!parse_range(p, range)) {
        p->err = NULL;
        p->at = 0U;
        skip_ws(p);
        if (p->at < p->len && p->line[p->at] == ':') {
            p->at++;
            skip_ws(p);
        }
    }
    skip_ws(p);
    start = p->at;
    while (p->at < p->len && !is_ws(p->line[p->at]) &&
           p->line[p->at] != '!')
        p->at++;
    if (cursor < start)
        cursor = start;
    if (cursor < p->at)
        p->at = cursor;
    *tok = (Span){start, p->at};
    *name = arena_strndup(p->arena, p->line + start, p->at - start);
    return p->at > start;
}

bool yew_cmd_parse_point(Ed *ed, const char *line, size_t len,
                         size_t cursor, Arena *a, CmdParsePoint *out)
{
    Parser p;
    CmdErr ignored = {0};
    char *name = NULL;
    Span name_tok = {0U, 0U};
    CmdId command = YEW_CMD_NONE;
    u32 index = 0U;

    if (line == NULL || a == NULL || out == NULL)
        return false;
    if (memchr(line, '\0', len) != NULL)
        return false;
    if (cursor > len)
        cursor = len;
    memset(out, 0, sizeof(*out));
    p = (Parser){ed, line, len, 0U, a, &ignored};
    (void)loose_name(&p, cursor, &name, &name_tok, &out->range);
    if (name != NULL && name[0] != '\0') {
        CmdErr saved = ignored;

        memset(&ignored, 0, sizeof(ignored));
        command = resolve_name(&p, name, name_tok);
        ignored = saved;
    }
    if (cursor <= name_tok.hi || p.at >= cursor) {
        out->token = name_tok;
        out->stem = name == NULL ? arena_strdup(a, "") : name;
        out->token_index = 0U;
        out->command = command;
        out->command_known = command.v != 0U;
        return true;
    }
    p.at = (size_t)name_tok.hi;
    if (p.at < len && line[p.at] == '!')
        p.at++;
    for (;;) {
        size_t before_ws = p.at;
        size_t token_start;
        char *value;
        Span tok;

        skip_ws(&p);
        token_start = p.at;
        if (cursor <= token_start) {
            index++;
            out->token = (Span){cursor, cursor};
            out->stem = arena_strdup(a, "");
            break;
        }
        if (!parse_token(&p, true, cursor, &value, &tok)) {
            out->token = (Span){cursor, cursor};
            out->stem = arena_strdup(a, "");
            break;
        }
        index++;
        if (cursor <= p.at || p.at == len) {
            out->token = tok;
            out->stem = value;
            break;
        }
        if (p.at == before_ws)
            break;
    }
    out->token_index = index == 0U ? 1U : index;
    out->command = command;
    out->command_known = command.v != 0U;
    return true;
}

Span yew_range_span(const TextBuf *tb, const CmdRange *range)
{
    Span last;

    if (tb == NULL || range == NULL || range->kind == YEW_RANGE_NONE ||
        yew_textbuf_len(tb) == 0U)
        return (Span){0U, 0U};
    if (range->lo.v >= yew_textbuf_line_count(tb) ||
        range->hi.v >= yew_textbuf_line_count(tb) ||
        range->lo.v > range->hi.v)
        return (Span){0U, 0U};
    last = yew_textbuf_line_span(tb, range->hi);
    return (Span){yew_textbuf_line_start(tb, range->lo).v, last.hi};
}
