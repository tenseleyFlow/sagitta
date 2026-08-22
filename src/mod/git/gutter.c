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
    u32 i,j,p=0,q=0; u32 *d;
    (void)a;
    if (!out || (!a0 && an) || (!b0 && bn)) return false;
    out->len = 0; if (an==0 && bn==0) return true;
    if (an > SIZE_MAX/(bn+1U)) return false;
    d = yew_xcalloc((size_t)(an+1U)*(bn+1U), sizeof(*d));
    for(i=an;i>0;i--) for(j=bn;j>0;j--)
        d[(size_t)(i-1U)*(bn+1U)+j-1U] = a0[i-1U]==b0[j-1U] ? d[(size_t)i*(bn+1U)+j] + 1U :
            (d[(size_t)i*(bn+1U)+j-1U] > d[(size_t)(i-1U)*(bn+1U)+j] ? d[(size_t)i*(bn+1U)+j-1U] : d[(size_t)(i-1U)*(bn+1U)+j]);
    while(p<an || q<bn) {
        u32 ap=p,bq=q;
        while(p<an && q<bn && a0[p]==b0[q]) {p++;q++;}
        while(p<an || q<bn) {
            if(p<an && q<bn && a0[p]==b0[q]) break;
            if(p<an && (q==bn || d[(size_t)(p+1U)*(bn+1U)+q] >= d[(size_t)p*(bn+1U)+q+1U])) p++; else if(q<bn) q++;
        }
        if(ap!=p || bq!=q) { if (budget && out->len>=budget) {free(d); return false;} GitHunkVec_push(out,(GitHunk){LINENO(ap),LINENO(p-ap),LINENO(bq),LINENO(q-bq),(ap==p)?YEW_HUNK_ADD:((bq==q)?YEW_HUNK_DEL:YEW_HUNK_MOD)}); }
    }
    free(d);
    return true;
}

bool yew_git_hunk_patch(Bytebuf *o, const char *path, const u8 *base,
                        size_t bl, const u8 *buf, size_t nl, const GitHunk *h)
{
    size_t at, i, line;
    if (!o || !path || strchr(path, '\n') || (!base && bl) || (!buf && nl) || !h) return false;
    bytebuf_init(o);
    bytebuf_printf(o, "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n@@ -%u,%u +%u,%u @@\n", path,path,path,path,h->base_lo.v+1U,h->base_n.v,h->buf_lo.v+1U,h->buf_n.v);
    /* Serialize the changed ranges.  The integration layer may prepend
     * additional context hunks; this primitive always preserves raw bytes. */
    at=0; line=0;
    while (at<bl && line<h->base_lo.v) { size_t e=at; while(e<bl&&base[e]!='\n')e++; at=e<bl?e+1:e; line++; }
    for (i=0;i<h->base_n.v && at<bl;i++) { size_t e=at; while(e<bl&&base[e]!='\n')e++; bytebuf_push_u8(o,'-'); bytebuf_append(o,base+at,e-at); if(e<bl)bytebuf_push_u8(o,'\n'); at=e<bl?e+1:e; }
    at=0; line=0; while(at<nl&&line<h->buf_lo.v){size_t e=at;while(e<nl&&buf[e]!='\n')e++;at=e<nl?e+1:e;line++;}
    for(i=0;i<h->buf_n.v&&at<nl;i++){size_t e=at;while(e<nl&&buf[e]!='\n')e++;bytebuf_push_u8(o,'+');bytebuf_append(o,buf+at,e-at);if(e<nl)bytebuf_push_u8(o,'\n');at=e<nl?e+1:e;}
    if ((h->base_lo.v+h->base_n.v)==0U && bl==0U) bytebuf_append(o,(const u8*)"",0);
    /* Caller supplies exact line slices in a later integration layer. */
    return true;
}
