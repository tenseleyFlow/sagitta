#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "term/grid.h"
#include "term/render.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum { MAX_OPS = 64 };

typedef struct {
    size_t bsu;
    size_t esu;
} VtCounts;

static bool fail(char *why, size_t cap, const char *message, size_t at)
{
    (void)snprintf(why, cap, "%s at byte %zu", message, at);
    return false;
}

static bool digits(const u8 *s, size_t begin, size_t end)
{
    size_t i;

    if (begin == end)
        return false;
    for (i = begin; i < end; i++) {
        if (s[i] < (u8)'0' || s[i] > (u8)'9')
            return false;
    }
    return true;
}

/* Sprint 6's VT accepts only these renderer-owned CSI forms. */
static bool vt_stream_closed(const u8 *s, size_t len, VtCounts *counts,
                             char *why, size_t why_cap)
{
    size_t i = 0u;

    memset(counts, 0, sizeof(*counts));
    while (i < len) {
        size_t body;
        size_t final;
        u8 command;

        if (s[i] == 0x1bu) {
            if (i + 2u >= len || s[i + 1u] != (u8)'[')
                return fail(why, why_cap, "non-CSI escape", i);
            body = i + 2u;
            final = body;
            while (final < len && !((s[final] >= (u8)'@' &&
                                     s[final] <= (u8)'~')))
                final++;
            if (final == len)
                return fail(why, why_cap, "unterminated CSI", i);
            command = s[final];
            if (command == (u8)'h' || command == (u8)'l') {
                bool is_25 = final - body == 3u &&
                             memcmp(s + body, "?25", 3u) == 0;
                bool is_2026 = final - body == 5u &&
                               memcmp(s + body, "?2026", 5u) == 0;

                if (!is_25 && !is_2026)
                    return fail(why, why_cap, "unsupported private mode", i);
                if (is_2026) {
                    if (command == (u8)'h')
                        counts->bsu++;
                    else
                        counts->esu++;
                }
            } else if (command == (u8)'H') {
                size_t semi = body;

                if (body != final) {
                    while (semi < final && s[semi] != (u8)';')
                        semi++;
                    if (semi == final || !digits(s, body, semi) ||
                        !digits(s, semi + 1u, final))
                        return fail(why, why_cap, "invalid CUP", i);
                }
            } else if (command == (u8)'C') {
                if (!digits(s, body, final))
                    return fail(why, why_cap, "invalid CUF", i);
            } else if (command == (u8)'K') {
                if (body != final)
                    return fail(why, why_cap, "invalid EL", i);
            } else if (command == (u8)'m') {
                size_t j;

                if (body == final)
                    return fail(why, why_cap, "empty SGR", i);
                for (j = body; j < final; j++) {
                    if (!((s[j] >= (u8)'0' && s[j] <= (u8)'9') ||
                          s[j] == (u8)';' || s[j] == (u8)':'))
                        return fail(why, why_cap, "invalid SGR parameter", i);
                }
            } else {
                return fail(why, why_cap, "unsupported CSI command", i);
            }
            i = final + 1u;
            continue;
        }
        if (s[i] < 0x20u || s[i] == 0x7fu)
            return fail(why, why_cap, "raw control in render stream", i);
        i++;
    }
    return true;
}

static bool grid_invariants(const Grid *g, char *why, size_t why_cap,
                            size_t at)
{
    u16 row;

    for (row = 0u; row < g->rows; row++) {
        u16 col;

        for (col = 0u; col < g->cols; col++) {
            size_t index = (size_t)row * g->cols + col;
            const Cell *cell = &g->back[index];
            bool differs = !sag_cell_eq(&g->front[index], cell);
            bool covered = row >= g->dmg_lo && row < g->dmg_hi &&
                           col >= g->dmg[row].lo && col < g->dmg[row].hi;

            if (cell->w > 2u)
                return fail(why, why_cap, "invalid cell width", at);
            if (cell->w == 0u &&
                (col == 0u || g->back[index - 1u].w != 2u))
                return fail(why, why_cap, "orphan continuation", at);
            if (cell->w == 2u &&
                ((u16)(col + 1u) >= g->cols ||
                 g->back[index + 1u].w != 0u))
                return fail(why, why_cap, "wide head without tail", at);
            if (cell->w == 0u) {
                const Cell *head = &g->back[index - 1u];

                if (memcmp(&cell->fg, &head->fg, sizeof(cell->fg)) != 0 ||
                    memcmp(&cell->bg, &head->bg, sizeof(cell->bg)) != 0 ||
                    cell->attrs != head->attrs)
                    return fail(why, why_cap,
                                "continuation style differs from head", at);
            }
            if (differs && !covered)
                return fail(why, why_cap, "damage misses changed cell", at);
        }
    }
    if (g->rows != 0u && g->cols != 0u &&
        g->back[(size_t)g->cur_row * g->cols + g->cur_col].w == 0u)
        return fail(why, why_cap, "cursor on continuation", at);
    return true;
}

static SagColor fuzz_color(u8 a, u8 b, u8 c)
{
    SagColor color;

    memset(&color, 0, sizeof(color));
    color.tag = (u8)(a % 3u);
    color.r = b;
    color.g = c;
    color.b = (u8)(a ^ b ^ c);
    return color;
}

static void put_choice(Grid *g, u16 row, u16 col, u8 choice,
                       SagColor fg, SagColor bg, u16 attrs)
{
    static const u8 ascii[] = {'x'};
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};
    static const u8 emoji[] = {0xf0u, 0x9fu, 0x98u, 0x80u};
    static const u8 combining[] = {0xccu, 0x81u};
    static const u8 invalid[] = {0xffu};
    static const u8 zwj[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u
    };
    const u8 *text = ascii;
    size_t len = sizeof(ascii);

    switch (choice % 6u) {
    case 1u: text = cjk; len = sizeof(cjk); break;
    case 2u: text = emoji; len = sizeof(emoji); break;
    case 3u: text = combining; len = sizeof(combining); break;
    case 4u: text = invalid; len = sizeof(invalid); break;
    case 5u: text = zwj; len = sizeof(zwj); break;
    default: break;
    }
    (void)sag_grid_put(g, row, col, text, len, fg, bg, attrs);
}

static bool check_grid(const u8 *data, size_t len,
                       char *why, size_t why_cap)
{
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    Bytebuf output;
    TtyCaps caps;
    size_t pos = 0u;
    size_t ops = 0u;
    bool ok = true;

    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!sag_grid_init(&grid, &interner, 8u, 16u)) {
        interner_free(&interner);
        arena_free_all(&arena);
        return fail(why, why_cap, "grid init failed", 0u);
    }
    memset(&caps, 0, sizeof(caps));
    caps.truecolor = true;
    caps.sync_output = true;
    sag_render_init(&render, &caps, NULL);
    bytebuf_init(&output);

    while (pos < len && ops < MAX_OPS) {
        u8 op = data[pos++];
        u8 a = pos < len ? data[pos++] : op;
        u8 b = pos < len ? data[pos++] : (u8)(op * 17u);
        u8 c = pos < len ? data[pos++] : (u8)(op * 31u);
        u16 row = (u16)(a % grid.rows);
        u16 col = (u16)(b % grid.cols);
        SagColor fg = fuzz_color(a, b, c);
        SagColor bg = fuzz_color(c, a, b);
        u16 attrs = (u16)(((u16)a << 8u | b) & 0x03ffu);
        VtCounts counts;
        size_t emitted;

        switch (op % 5u) {
        case 0u:
            put_choice(&grid, row, col, c, fg, bg, attrs);
            break;
        case 1u: {
            static const u8 text[] = "ab\xe6\xbc\xa2\xf0\x9f\x98\x80";
            (void)sag_grid_puts(&grid, row, col, text, sizeof(text) - 1u,
                                fg, bg, attrs);
            break;
        }
        case 2u: {
            Cell fill = grid.blank;

            fill.fg = fg;
            fill.bg = bg;
            fill.attrs = attrs;
            fill.utf8[0] = (u8)('a' + c % 26u);
            sag_grid_fill(&grid, row, col,
                          (u16)(col + 1u + c % (grid.cols - col)), fill);
            break;
        }
        case 3u:
            sag_grid_cursor(&grid, row, col, (c & 1u) != 0u);
            break;
        default:
            if (!sag_grid_resize(&grid, (u16)(1u + a % 12u),
                                 (u16)(1u + b % 20u))) {
                ok = fail(why, why_cap, "grid resize failed", pos);
            }
            break;
        }
        if (!ok || !grid_invariants(&grid, why, why_cap, pos)) {
            ok = false;
            break;
        }
        output.len = 0u;
        emitted = sag_render_frame(&render, &grid, &output);
        if (emitted != output.len ||
            !vt_stream_closed(output.data, output.len, &counts,
                              why, why_cap)) {
            ok = false;
            break;
        }
        if (counts.bsu != counts.esu || counts.bsu > 1u ||
            (emitted != 0u && counts.bsu != 1u) ||
            (emitted == 0u && counts.bsu != 0u)) {
            ok = fail(why, why_cap, "unbalanced mode 2026 frame", pos);
            break;
        }
        sag_grid_flip(&grid);
        if (!grid_invariants(&grid, why, why_cap, pos)) {
            ok = false;
            break;
        }
        ops++;
    }

    bytebuf_free(&output);
    sag_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_grid", NULL, check_grid);
}
