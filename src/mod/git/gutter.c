#include "mod/git/gutter.h"
#include <string.h>

u64 yew_git_line_hash(const u8 *p, size_t n)
{
    u64 h = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

bool yew_git_hash_lines(const u8 *b, size_t n, Arena *a, u64 **out,
                        u32 *count, bool *missing)
{
    u32 c = 0, i = 0; size_t at = 0, end;
    u64 *v;
    if ((!b && n) || !a || !out || !count || !missing) return false;
    *missing = n != 0 && b[n - 1] != '\n';
    if (n) { c = 1; for (at = 0; at < n; at++) if (b[at] == '\n') c++; }
    v = c ? arena_alloc(a, (size_t)c * sizeof(*v), _Alignof(u64)) : NULL;
    at = 0;
    while (i < c) { end = at; while (end < n && b[end] != '\n') end++; v[i++] = yew_git_line_hash(b + at, end - at + (end < n)); at = end < n ? end + 1 : end; }
    *out = v; *count = c; return true;
}

bool yew_diff_lines(Arena *a, const u64 *a0, u32 an, const u64 *b0, u32 bn,
                    u32 budget, GitHunkVec *out)
{
    u32 p = 0, q = 0;
    (void)a;
    if (!out || (!a0 && an) || (!b0 && bn)) return false;
    out->len = 0;
    while (p < an || q < bn) {
        u32 ap = p, bq = q;
        while (p < an && q < bn && a0[p] == b0[q]) { p++; q++; }
        if (p == an && q == bn) break;
        while (p < an && q < bn && a0[p] != b0[q]) { p++; q++; }
        if (p == ap && q == bq) { if (p < an) p++; if (q < bn) q++; }
        if (out->len >= budget && budget != 0U) return false;
        GitHunkVec_push(out, (GitHunk){LINENO(ap), LINENO(p-ap), LINENO(bq), LINENO(q-bq),
            ap == p ? YEW_HUNK_ADD : (bq == q ? YEW_HUNK_DEL : YEW_HUNK_MOD)});
    }
    return true;
}

bool yew_git_hunk_patch(Bytebuf *o, const char *path, const u8 *base,
                        size_t bl, const u8 *buf, size_t nl, const GitHunk *h)
{
    if (!o || !path || strchr(path, '\n') || (!base && bl) || (!buf && nl) || !h) return false;
    bytebuf_init(o);
    bytebuf_printf(o, "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n@@ -%u,%u +%u,%u @@\n", path,path,path,path,h->base_lo.v+1U,h->base_n.v,h->buf_lo.v+1U,h->buf_n.v);
    /* Caller supplies exact line slices in a later integration layer. */
    return true;
}
