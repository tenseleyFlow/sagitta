#include "vt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/width.h"

enum {
    VT_PARSE_TEXT = 0,
    VT_PARSE_ESC,
    VT_PARSE_CSI,
    VT_PARSE_OSC,
    VT_PARSE_OSC_ESC,
    VT_ERROR_BYTES_MAX = VT_TRANSCRIPT_BYTES_MAX
};

static void bounded_append(Bytebuf *buf, const void *data, size_t n)
{
    size_t room;

    if (buf->len >= VT_TRANSCRIPT_BYTES_MAX)
        return;
    room = VT_TRANSCRIPT_BYTES_MAX - buf->len;
    bytebuf_append(buf, data, n < room ? n : room);
}

static SagColor default_color(void)
{
    SagColor c = {SAG_COLOR_DEFAULT, 0u, 0u, 0u};

    return c;
}

static VtCell blank_cell(void)
{
    VtCell c;

    memset(&c, 0, sizeof(c));
    c.w = 1u;
    return c;
}

static void text_reset(VtScreen *v)
{
    sag_gb_init(&v->gb);
    v->cluster_valid = false;
}

static void error_append(VtScreen *v, const char *kind,
                         const u8 *bytes, size_t n)
{
    size_t i;

    if (v->nerrors != UINT32_MAX)
        v->nerrors++;
    if (v->errors.len >= VT_ERROR_BYTES_MAX)
        return;
    bytebuf_printf(&v->errors, "%s:", kind);
    for (i = 0u; i < n && v->errors.len < VT_ERROR_BYTES_MAX; i++) {
        if (bytes[i] == 0x1bu)
            bytebuf_printf(&v->errors, " ESC");
        else if (bytes[i] >= 0x20u && bytes[i] <= 0x7eu)
            bytebuf_printf(&v->errors, " %c", (int)bytes[i]);
        else
            bytebuf_printf(&v->errors, " 0x%02x", (unsigned)bytes[i]);
    }
    bytebuf_push_u8(&v->errors, '\n');
    if (v->errors.len > VT_ERROR_BYTES_MAX)
        v->errors.len = VT_ERROR_BYTES_MAX;
}

static void unknown(VtScreen *v)
{
    error_append(v, "unknown sequence", v->seq, v->nseq);
}

static bool cell_is_tail(const VtScreen *v, int row, int col)
{
    return row >= 0 && row < v->rows && col >= 0 && col < v->cols &&
           v->cells[(size_t)row * (size_t)v->cols + (size_t)col].w == 0u;
}

static void clear_pair_at(VtScreen *v, int row, int col)
{
    VtCell blank = blank_cell();
    VtCell *cell;

    if (row < 0 || row >= v->rows || col < 0 || col >= v->cols)
        return;
    cell = &v->cells[(size_t)row * (size_t)v->cols + (size_t)col];
    if (cell->w == 2u && col + 1 < v->cols) {
        size_t tail = (size_t)row * (size_t)v->cols + (size_t)col + 1u;
        v->cells[(size_t)row * (size_t)v->cols + (size_t)col + 1u] = blank;
        v->glyph_off[tail] = 0u;
        v->glyph_len[tail] = 0u;
    } else if (cell->w == 0u && col > 0) {
        size_t head = (size_t)row * (size_t)v->cols + (size_t)col - 1u;
        v->cells[(size_t)row * (size_t)v->cols + (size_t)col - 1u] = blank;
        v->glyph_off[head] = 0u;
        v->glyph_len[head] = 0u;
    }
    v->glyph_off[(size_t)row * (size_t)v->cols + (size_t)col] = 0u;
    v->glyph_len[(size_t)row * (size_t)v->cols + (size_t)col] = 0u;
}

static void cell_store_bytes(VtScreen *v, size_t off, VtCell *cell,
                             const u8 *bytes, size_t n)
{
    Bytebuf compact;
    u8 *owned = NULL;
    size_t count = (size_t)v->rows * (size_t)v->cols;
    size_t old_off = v->glyph_off[off];
    size_t old_len = v->glyph_len[off];
    size_t limit;
    size_t i;

    if (n > sizeof(cell->g)) {
        owned = sag_xmalloc(n);
        memcpy(owned, bytes, n);
        bytes = owned;
    }
    memset(cell->g, 0, sizeof(cell->g));
    cell->nb = (u8)(n < sizeof(cell->g) ? n : sizeof(cell->g));
    if (n != 0u)
        memcpy(cell->g, bytes, cell->nb);
    v->glyph_len[off] = 0u;
    v->glyph_off[off] = 0u;
    if (old_len != 0u && old_off + old_len == v->glyphs.len)
        v->glyphs.len = old_off;
    if (n > sizeof(cell->g)) {
        limit = count > SIZE_MAX / VT_CLUSTER_BYTES_MAX
                    ? SIZE_MAX
                    : count * VT_CLUSTER_BYTES_MAX;
        if (v->glyphs.len > limit - n) {
            bytebuf_init(&compact);
            for (i = 0u; i < count; i++) {
                size_t live = v->glyph_len[i];
                size_t live_off = v->glyph_off[i];

                if (live == 0u)
                    continue;
                v->glyph_off[i] = compact.len;
                bytebuf_append(&compact, v->glyphs.data + live_off, live);
            }
            bytebuf_free(&v->glyphs);
            v->glyphs = compact;
        }
        v->glyph_off[off] = v->glyphs.len;
        v->glyph_len[off] = n;
        bytebuf_append(&v->glyphs, bytes, n);
    }
    free(owned);
}

static void write_cluster_start(VtScreen *v, const u8 *bytes, size_t n)
{
    VtCell head;
    VtCell tail;
    int width = sag_cluster_width(bytes, n);
    size_t off;

    if (!v->alt) {
        v->primary_written = true;
        bounded_append(&v->primary, bytes, n);
        if (!v->allow_primary_text)
            error_append(v, "write to primary screen", bytes, n);
        return;
    }
    if (width != 1 && width != 2) {
        error_append(v, "nonprinting text", bytes, n);
        return;
    }
    if (v->cur_r < 0 || v->cur_r >= v->rows || v->cur_c < 0 ||
        v->cur_c >= v->cols || width > v->cols - v->cur_c) {
        error_append(v, "text write would autowrap", bytes, n);
        return;
    }
    if (cell_is_tail(v, v->cur_r, v->cur_c)) {
        error_append(v, "write onto wide tail", bytes, n);
        return;
    }
    clear_pair_at(v, v->cur_r, v->cur_c);
    if (width == 2 && v->cur_c + 1 < v->cols)
        clear_pair_at(v, v->cur_r, v->cur_c + 1);
    memset(&head, 0, sizeof(head));
    head.fg = v->fg;
    head.bg = v->bg;
    head.attrs = v->attrs;
    head.w = (u8)width;
    off = (size_t)v->cur_r * (size_t)v->cols + (size_t)v->cur_c;
    cell_store_bytes(v, off, &head, bytes, n);
    if (n == 1u && bytes[0] == ' ') {
        head.nb = 0u;
        v->glyph_len[off] = 0u;
    }
    v->cells[off] = head;
    if (width == 2) {
        memset(&tail, 0, sizeof(tail));
        tail.fg = v->fg;
        tail.bg = v->bg;
        tail.attrs = v->attrs;
        v->cells[off + 1u] = tail;
        v->glyph_off[off + 1u] = 0u;
        v->glyph_len[off + 1u] = 0u;
    }
    v->cluster_r = v->cur_r;
    v->cluster_c = v->cur_c;
    v->cluster_valid = true;
    v->cur_c += width;
}

static void append_cluster(VtScreen *v, const u8 *bytes, size_t n)
{
    VtCell *head;
    const u8 *old;
    size_t old_n;
    u8 *joined;
    int old_width;
    int width;

    if (!v->cluster_valid || v->cluster_r < 0 ||
        v->cluster_r >= v->rows || v->cluster_c < 0 ||
        v->cluster_c >= v->cols) {
        error_append(v, "combining text without base", bytes, n);
        return;
    }
    head = &v->cells[(size_t)v->cluster_r * (size_t)v->cols +
                     (size_t)v->cluster_c];
    old = vt_cell_bytes(v, head, &old_n);
    if (n > SIZE_MAX - old_n) {
        error_append(v, "grapheme too long", bytes, n);
        return;
    }
    if (old_n + n > VT_CLUSTER_BYTES_MAX) {
        error_append(v, "grapheme exceeds byte limit", bytes, n);
        return;
    }
    joined = sag_xmalloc(old_n + n);
    if (old_n != 0u)
        memcpy(joined, old, old_n);
    memcpy(joined + old_n, bytes, n);
    old_width = head->w;
    width = sag_cluster_width(joined, old_n + n);
    if (width == 2 && old_width == 1) {
        size_t tail_off;
        VtCell tail;

        if (v->cluster_c + 1 >= v->cols) {
            error_append(v, "text write would autowrap", bytes, n);
            free(joined);
            return;
        }
        clear_pair_at(v, v->cluster_r, v->cluster_c + 1);
        memset(&tail, 0, sizeof(tail));
        tail.fg = head->fg;
        tail.bg = head->bg;
        tail.attrs = head->attrs;
        tail_off = (size_t)v->cluster_r * (size_t)v->cols +
                   (size_t)v->cluster_c + 1u;
        v->cells[tail_off] = tail;
        head->w = 2u;
        v->cur_c++;
    }
    cell_store_bytes(v,
                     (size_t)v->cluster_r * (size_t)v->cols +
                         (size_t)v->cluster_c,
                     head, joined, old_n + n);
    free(joined);
}

static void text_cp(VtScreen *v, u32 cp)
{
    u8 bytes[SAG_UTF8_MAX];
    size_t n = sag_utf8_encode(cp, bytes);
    bool boundary;

    if (sag_utf8_is_escape(cp)) {
        error_append(v, "invalid UTF-8", bytes, n);
        text_reset(v);
        return;
    }
    if (!v->alt) {
        v->primary_written = true;
        bounded_append(&v->primary, bytes, n);
        if (!v->allow_primary_text)
            error_append(v, "write to primary screen", bytes, n);
        return;
    }
    boundary = sag_gb_boundary(&v->gb, cp);
    if (boundary)
        write_cluster_start(v, bytes, n);
    else
        append_cluster(v, bytes, n);
}

static void flush_incomplete_utf8(VtScreen *v)
{
    u8 i;
    u8 n = sag_utf8_finish(&v->u8dec);

    for (i = 0u; i < n; i++)
        text_cp(v, v->u8dec.out[i]);
}

static void text_byte(VtScreen *v, u8 byte)
{
    u8 i;
    u8 n = sag_utf8_push(&v->u8dec, byte);

    for (i = 0u; i < n; i++)
        text_cp(v, v->u8dec.out[i]);
}

static bool parse_uint(const u8 *s, size_t n, unsigned *out)
{
    size_t i;
    unsigned value = 0u;

    if (n == 0u)
        return false;
    for (i = 0u; i < n; i++) {
        unsigned digit;

        if (s[i] < '0' || s[i] > '9')
            return false;
        digit = (unsigned)(s[i] - '0');
        if (value > (UINT_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool single_param(const u8 *s, size_t n, unsigned fallback,
                         unsigned *out)
{
    if (n == 0u) {
        *out = fallback;
        return true;
    }
    return parse_uint(s, n, out);
}

static void probe_record(VtScreen *v, u8 probe)
{
    if (v->nprobes < SAG_ARRAY_LEN(v->probe_order))
        v->probe_order[v->nprobes++] = probe;
    v->probes |= probe;
}

static void probe_reply(VtScreen *v, u8 probe)
{
    probe_record(v, probe);
    if (probe == VT_PROBE_KITTY) {
        if (v->profile != VT_PROFILE_NOKITTY && v->profile != VT_PROFILE_DUMB)
            bounded_append(&v->replies, "\033[?0u", 5u);
    } else if (probe == VT_PROBE_SYNC) {
        if (v->profile == VT_PROFILE_MODERN ||
            v->profile == VT_PROFILE_NOKITTY)
            bounded_append(&v->replies, "\033[?2026;2$y", 11u);
        else if (v->profile == VT_PROFILE_NOSYNC)
            bounded_append(&v->replies, "\033[?2026;0$y", 11u);
    } else if (probe == VT_PROBE_DA && v->profile != VT_PROFILE_DUMB) {
        bounded_append(&v->replies, "\033[?62;22c", 9u);
    }
}

static bool parse_sgr(VtScreen *v, const u8 *s, size_t n)
{
    unsigned p[32];
    size_t np = 0u;
    size_t start = 0u;
    size_t i;
    SagColor fg = v->fg;
    SagColor bg = v->bg;
    u16 attrs = v->attrs;

    if (n == 0u)
        p[np++] = 0u;
    for (i = 0u; i <= n && n != 0u; i++) {
        if (i != n && s[i] != ';')
            continue;
        if (np >= SAG_ARRAY_LEN(p))
            return false;
        if (i == start)
            p[np++] = 0u;
        else if (i - start == 3u && memcmp(s + start, "4:3", 3u) == 0)
            p[np++] = 0x403u;
        else if (!parse_uint(s + start, i - start, &p[np++]))
            return false;
        start = i + 1u;
    }
    for (i = 0u; i < np; i++) {
        unsigned x = p[i];

        if (x == 0u) {
            fg = default_color(); bg = default_color(); attrs = 0u;
        } else if (x == 1u) attrs |= SAG_ATTR_BOLD;
        else if (x == 2u) attrs |= SAG_ATTR_DIM;
        else if (x == 3u) attrs |= SAG_ATTR_ITALIC;
        else if (x == 4u) {
            attrs &= (u16)~SAG_ATTR_UNDERCURL; attrs |= SAG_ATTR_UNDERLINE;
        } else if (x == 0x403u) {
            attrs &= (u16)~SAG_ATTR_UNDERLINE; attrs |= SAG_ATTR_UNDERCURL;
        } else if (x == 5u) attrs |= SAG_ATTR_BLINK;
        else if (x == 7u) attrs |= SAG_ATTR_REVERSE;
        else if (x == 8u) attrs |= SAG_ATTR_CONCEAL;
        else if (x == 9u) attrs |= SAG_ATTR_STRIKE;
        else if (x == 53u) attrs |= SAG_ATTR_OVERLINE;
        else if (x == 22u) attrs &= (u16)~(SAG_ATTR_BOLD | SAG_ATTR_DIM);
        else if (x == 23u) attrs &= (u16)~SAG_ATTR_ITALIC;
        else if (x == 24u) attrs &= (u16)~(SAG_ATTR_UNDERLINE | SAG_ATTR_UNDERCURL);
        else if (x == 25u) attrs &= (u16)~SAG_ATTR_BLINK;
        else if (x == 27u) attrs &= (u16)~SAG_ATTR_REVERSE;
        else if (x == 28u) attrs &= (u16)~SAG_ATTR_CONCEAL;
        else if (x == 29u) attrs &= (u16)~SAG_ATTR_STRIKE;
        else if (x == 55u) attrs &= (u16)~SAG_ATTR_OVERLINE;
        else if (x >= 30u && x <= 37u)
            fg = (SagColor){SAG_COLOR_INDEXED, (u8)(x - 30u), 0u, 0u};
        else if (x >= 90u && x <= 97u)
            fg = (SagColor){SAG_COLOR_INDEXED, (u8)(x - 82u), 0u, 0u};
        else if (x >= 40u && x <= 47u)
            bg = (SagColor){SAG_COLOR_INDEXED, (u8)(x - 40u), 0u, 0u};
        else if (x >= 100u && x <= 107u)
            bg = (SagColor){SAG_COLOR_INDEXED, (u8)(x - 92u), 0u, 0u};
        else if (x == 39u) fg = default_color();
        else if (x == 49u) bg = default_color();
        else if ((x == 38u || x == 48u) && i + 2u < np && p[i + 1u] == 5u &&
                 p[i + 2u] <= 255u) {
            SagColor color = {SAG_COLOR_INDEXED, (u8)p[i + 2u], 0u, 0u};
            if (x == 38u) fg = color; else bg = color;
            i += 2u;
        } else if ((x == 38u || x == 48u) && i + 4u < np && p[i + 1u] == 2u &&
                   p[i + 2u] <= 255u && p[i + 3u] <= 255u &&
                   p[i + 4u] <= 255u) {
            SagColor color = {SAG_COLOR_RGB, (u8)p[i + 2u],
                              (u8)p[i + 3u], (u8)p[i + 4u]};
            if (x == 38u) fg = color; else bg = color;
            i += 4u;
        } else {
            return false;
        }
    }
    v->fg = fg;
    v->bg = bg;
    v->attrs = attrs;
    return true;
}

static bool exact(const u8 *s, size_t n, const char *lit)
{
    size_t want = strlen(lit);

    return n == want && memcmp(s, lit, n) == 0;
}

static void set_mode(VtScreen *v, u32 bit, bool enabled)
{
    if (enabled)
        v->modes |= bit;
    else
        v->modes &= ~bit;
}

static void csi_dispatch(VtScreen *v)
{
    const u8 *body = v->seq + 2u;
    size_t nbody = v->nseq - 3u;
    u8 final = v->seq[v->nseq - 1u];
    unsigned value;
    int row;
    int col;

    if (final == 'u' && exact(body, nbody, "?")) {
        probe_reply(v, VT_PROBE_KITTY);
    } else if (final == 'p' && exact(body, nbody, "?2026$")) {
        probe_reply(v, VT_PROBE_SYNC);
    } else if (final == 'c' && nbody == 0u) {
        probe_reply(v, VT_PROBE_DA);
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?1049")) {
        v->alt = final == 'h';
        if (v->alt) {
            size_t i;
            VtCell blank = blank_cell();
            for (i = 0u; i < (size_t)v->rows * (size_t)v->cols; i++)
                v->cells[i] = blank;
            memset(v->glyph_off, 0,
                   (size_t)v->rows * (size_t)v->cols * sizeof(*v->glyph_off));
            memset(v->glyph_len, 0,
                   (size_t)v->rows * (size_t)v->cols * sizeof(*v->glyph_len));
            v->cur_r = 0; v->cur_c = 0;
        }
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?25")) {
        v->cur_vis = final == 'h';
    } else if (final == 'q' && nbody == 2u && body[1] == (u8)' ' &&
               (body[0] == (u8)'0' || body[0] == (u8)'2' ||
                body[0] == (u8)'6')) {
        v->cursor_shape = (u8)(body[0] - (u8)'0');
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?2004")) {
        set_mode(v, VT_MODE_BRACKETED_PASTE, final == 'h');
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?1002")) {
        set_mode(v, VT_MODE_BUTTON_MOUSE, final == 'h');
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?1006")) {
        set_mode(v, VT_MODE_SGR_MOUSE, final == 'h');
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?1004")) {
        set_mode(v, VT_MODE_FOCUS, final == 'h');
    } else if ((final == 'h' || final == 'l') && exact(body, nbody, "?2026")) {
        if (final == 'h') {
            if (v->in_sync)
                error_append(v, "nested synchronized update", v->seq, v->nseq);
            else
                v->in_sync = true;
        } else if (!v->in_sync && !v->allow_idempotent_restore) {
            error_append(v, "unmatched synchronized update", v->seq, v->nseq);
        } else if (v->in_sync) {
            v->in_sync = false;
            v->nsync_pairs++;
        }
    } else if (final == 'u' && nbody >= 2u && body[0] == '>' &&
               parse_uint(body + 1u, nbody - 1u, &value)) {
        if (v->ksp >= (int)SAG_ARRAY_LEN(v->kitty))
            error_append(v, "kitty stack overflow", v->seq, v->nseq);
        else
            v->kitty[v->ksp++] = value;
    } else if (final == 'u' && exact(body, nbody, "<")) {
        if (v->ksp != 0)
            v->ksp--;
    } else if (final == 'H') {
        const u8 *semi = memchr(body, ';', nbody);
        bool ok;

        if (semi == NULL) {
            ok = single_param(body, nbody, 1u, &value);
            row = ok ? (value > (unsigned)INT_MAX ? INT_MAX : (int)value) : 1;
            col = 1;
        } else {
            size_t left = (size_t)(semi - body);
            unsigned r;
            unsigned c;
            ok = memchr(semi + 1, ';', nbody - left - 1u) == NULL &&
                 single_param(body, left, 1u, &r) &&
                 single_param(semi + 1, nbody - left - 1u, 1u, &c);
            row = ok ? (int)(r > (unsigned)INT_MAX ? INT_MAX : r) : 1;
            col = ok ? (int)(c > (unsigned)INT_MAX ? INT_MAX : c) : 1;
        }
        if (!ok) unknown(v);
        else {
            if (row < 1) row = 1;
            if (col < 1) col = 1;
            v->cur_r = row > v->rows ? v->rows - 1 : row - 1;
            v->cur_c = col > v->cols ? v->cols - 1 : col - 1;
        }
    } else if (final == 'C' && single_param(body, nbody, 1u, &value)) {
        unsigned step = value == 0u ? 1u : value;
        if (step >= (unsigned)(v->cols - v->cur_c))
            v->cur_c = v->cols - 1;
        else
            v->cur_c += (int)step;
    } else if (final == 'K' && (nbody == 0u || exact(body, nbody, "0"))) {
        VtCell blank = blank_cell();
        int c;

        blank.fg = v->fg;
        blank.bg = default_color();
        blank.attrs = v->attrs;
        for (c = v->cur_c; c < v->cols; c++)
            v->cells[(size_t)v->cur_r * (size_t)v->cols + (size_t)c] = blank;
        for (c = v->cur_c; c < v->cols; c++) {
            size_t off = (size_t)v->cur_r * (size_t)v->cols + (size_t)c;
            v->glyph_off[off] = 0u;
            v->glyph_len[off] = 0u;
        }
    } else if (final == 'm' && parse_sgr(v, body, nbody)) {
        /* parsed */
    } else {
        unknown(v);
    }
    text_reset(v);
}

static void seq_push(VtScreen *v, u8 byte)
{
    if (v->nseq < sizeof(v->seq)) {
        v->seq[v->nseq++] = byte;
        return;
    }
    error_append(v, "sequence too long", v->seq, v->nseq);
    v->parse_state = VT_PARSE_TEXT;
    v->nseq = 0u;
    text_reset(v);
}

static void osc_dispatch(VtScreen *v)
{
    static const u8 prefix[] = {0x1bu, (u8)']', (u8)'5', (u8)'2', (u8)';'};
    bool valid = v->nseq >= sizeof(prefix) + 4U &&
                 memcmp(v->seq, prefix, sizeof(prefix)) == 0 &&
                 v->seq[v->nseq - 2U] == 0x1bu &&
                 v->seq[v->nseq - 1U] == (u8)'\\';
    size_t i;

    for (i = sizeof(prefix); valid && i + 2U < v->nseq; i++) {
        u8 byte = v->seq[i];

        if (byte == (u8)'?')
            valid = false;
        else if (byte < 0x20u || byte > 0x7eu)
            valid = false;
    }
    if (!valid) {
        unknown(v);
    } else {
        v->nosc52++;
        if (v->in_sync)
            v->nosc52_in_sync++;
    }
    text_reset(v);
}

void vt_feed(VtScreen *v, const u8 *b, size_t n)
{
    size_t i;

    if (v == NULL || (b == NULL && n != 0u))
        return;
    for (i = 0u; i < n; i++) {
        u8 byte = b[i];

        if (v->parse_state == VT_PARSE_TEXT) {
            if (byte == 0x1bu) {
                flush_incomplete_utf8(v);
                text_reset(v);
                v->nseq = 0u;
                seq_push(v, byte);
                v->parse_state = VT_PARSE_ESC;
            } else if (byte < 0x20u || byte == 0x7fu) {
                flush_incomplete_utf8(v);
                if (!v->alt && (byte == '\r' || byte == '\n' || byte == '\t')) {
                    v->primary_written = true;
                    bounded_append(&v->primary, &byte, 1u);
                    if (!v->allow_primary_text)
                        error_append(v, "write to primary screen", &byte, 1u);
                } else {
                    error_append(v, "unknown control byte", &byte, 1u);
                }
                text_reset(v);
            } else {
                text_byte(v, byte);
            }
        } else if (v->parse_state == VT_PARSE_ESC) {
            seq_push(v, byte);
            if (byte == '[') {
                v->parse_state = VT_PARSE_CSI;
            } else if (byte == ']') {
                v->parse_state = VT_PARSE_OSC;
            } else {
                if (byte == '7') {
                    v->saved_r = v->cur_r; v->saved_c = v->cur_c;
                    v->saved_valid = true;
                } else if (byte == '8') {
                    if (v->saved_valid) {
                        v->cur_r = v->saved_r; v->cur_c = v->saved_c;
                    }
                } else {
                    unknown(v);
                }
                v->parse_state = VT_PARSE_TEXT;
                v->nseq = 0u;
                text_reset(v);
            }
        } else if (v->parse_state == VT_PARSE_CSI) {
            seq_push(v, byte);
            if (byte >= 0x40u && byte <= 0x7eu) {
                csi_dispatch(v);
                v->parse_state = VT_PARSE_TEXT;
                v->nseq = 0u;
            }
        } else if (v->parse_state == VT_PARSE_OSC) {
            seq_push(v, byte);
            if (byte == 0x1bu)
                v->parse_state = VT_PARSE_OSC_ESC;
        } else {
            seq_push(v, byte);
            if (byte == (u8)'\\')
                osc_dispatch(v);
            else
                unknown(v);
            v->parse_state = VT_PARSE_TEXT;
            v->nseq = 0u;
        }
    }
}

void vt_init(VtScreen *v, int rows, int cols)
{
    if (v == NULL)
        return;
    memset(v, 0, sizeof(*v));
    bytebuf_init(&v->errors);
    bytebuf_init(&v->replies);
    bytebuf_init(&v->glyphs);
    bytebuf_init(&v->primary);
    sag_utf8_dec_init(&v->u8dec);
    sag_gb_init(&v->gb);
    v->profile = VT_PROFILE_MODERN;
    v->cur_vis = true;
    vt_resize(v, rows, cols);
}

void vt_free(VtScreen *v)
{
    if (v == NULL)
        return;
    free(v->cells);
    bytebuf_free(&v->errors);
    bytebuf_free(&v->replies);
    bytebuf_free(&v->glyphs);
    bytebuf_free(&v->primary);
    free(v->glyph_off);
    free(v->glyph_len);
    memset(v, 0, sizeof(*v));
}

void vt_resize(VtScreen *v, int rows, int cols)
{
    VtCell *cells;
    size_t *glyph_off;
    size_t *glyph_len;
    VtCell blank = blank_cell();
    int copy_rows;
    int copy_cols;
    int r;
    int c;

    if (v == NULL || rows <= 0 || cols <= 0)
        return;
    cells = sag_xcalloc((size_t)rows * (size_t)cols, sizeof(*cells));
    glyph_off = sag_xcalloc((size_t)rows * (size_t)cols, sizeof(*glyph_off));
    glyph_len = sag_xcalloc((size_t)rows * (size_t)cols, sizeof(*glyph_len));
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            cells[(size_t)r * (size_t)cols + (size_t)c] = blank;
    copy_rows = rows < v->rows ? rows : v->rows;
    copy_cols = cols < v->cols ? cols : v->cols;
    for (r = 0; r < copy_rows; r++)
        memcpy(cells + (size_t)r * (size_t)cols,
               v->cells + (size_t)r * (size_t)v->cols,
               (size_t)copy_cols * sizeof(*cells));
    for (r = 0; r < copy_rows; r++) {
        memcpy(glyph_off + (size_t)r * (size_t)cols,
               v->glyph_off + (size_t)r * (size_t)v->cols,
               (size_t)copy_cols * sizeof(*glyph_off));
        memcpy(glyph_len + (size_t)r * (size_t)cols,
               v->glyph_len + (size_t)r * (size_t)v->cols,
               (size_t)copy_cols * sizeof(*glyph_len));
    }
    for (r = 0; r < rows; r++) {
        size_t last = (size_t)r * (size_t)cols + (size_t)(cols - 1);

        if (cells[last].w == 2u) {
            cells[last] = blank;
            glyph_off[last] = 0u;
            glyph_len[last] = 0u;
        }
        for (c = 0; c < cols; c++) {
            size_t off = (size_t)r * (size_t)cols + (size_t)c;

            if (cells[off].w == 0u &&
                (c == 0 || cells[off - 1u].w != 2u)) {
                cells[off] = blank;
                glyph_off[off] = 0u;
                glyph_len[off] = 0u;
            }
        }
    }
    free(v->cells);
    free(v->glyph_off);
    free(v->glyph_len);
    v->cells = cells;
    v->glyph_off = glyph_off;
    v->glyph_len = glyph_len;
    v->rows = rows;
    v->cols = cols;
    if (v->cur_r >= rows) v->cur_r = rows - 1;
    if (v->cur_c >= cols) v->cur_c = cols - 1;
    text_reset(v);
}

bool vt_profile_from_name(const char *name, VtProfile *out)
{
    VtProfile profile;

    if (name == NULL || out == NULL)
        return false;
    if (strcmp(name, "modern") == 0) profile = VT_PROFILE_MODERN;
    else if (strcmp(name, "nokitty") == 0) profile = VT_PROFILE_NOKITTY;
    else if (strcmp(name, "nosync") == 0) profile = VT_PROFILE_NOSYNC;
    else if (strcmp(name, "dumb") == 0) profile = VT_PROFILE_DUMB;
    else return false;
    *out = profile;
    return true;
}

void vt_set_profile(VtScreen *v, VtProfile profile)
{
    if (v != NULL)
        v->profile = profile;
}

void vt_take_replies(VtScreen *v, Bytebuf *out)
{
    if (v == NULL || out == NULL)
        return;
    bytebuf_append(out, v->replies.data, v->replies.len);
    v->replies.len = 0u;
}

u32 vt_take_queries(VtScreen *v)
{
    u32 queries;

    if (v == NULL)
        return 0u;
    queries = v->probes;
    v->probes = 0u;
    return queries;
}

const u8 *vt_cell_bytes(const VtScreen *v, const VtCell *cell, size_t *len)
{
    ptrdiff_t index;

    if (len != NULL)
        *len = 0u;
    if (v == NULL || cell == NULL || v->cells == NULL)
        return NULL;
    index = cell - v->cells;
    if (index < 0 || (size_t)index >= (size_t)v->rows * (size_t)v->cols)
        return NULL;
    if (v->glyph_len[index] != 0u) {
        if (len != NULL)
            *len = v->glyph_len[index];
        return v->glyphs.data + v->glyph_off[index];
    }
    if (len != NULL)
        *len = cell->nb;
    return cell->g;
}

bool vt_set_cell(VtScreen *v, int row, int col, const u8 *bytes, size_t n,
                 SagColor fg, SagColor bg, u16 attrs, u8 width)
{
    VtCell cell;
    size_t off;

    if (v == NULL || row < 0 || row >= v->rows || col < 0 || col >= v->cols ||
        (bytes == NULL && n != 0u) || n > VT_CLUSTER_BYTES_MAX || width > 2u)
        return false;
    memset(&cell, 0, sizeof(cell));
    cell.fg = fg;
    cell.bg = bg;
    cell.attrs = attrs;
    cell.w = width;
    off = (size_t)row * (size_t)v->cols + (size_t)col;
    cell_store_bytes(v, off, &cell, bytes, n);
    v->cells[off] = cell;
    return true;
}

void vt_set_primary_policy(VtScreen *v, bool allow_text)
{
    if (v != NULL)
        v->allow_primary_text = allow_text;
}

void vt_set_restore_policy(VtScreen *v, bool allow_idempotent_restore)
{
    if (v != NULL)
        v->allow_idempotent_restore = allow_idempotent_restore;
}
