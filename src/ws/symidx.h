#ifndef YEW_WS_SYMIDX_H
#define YEW_WS_SYMIDX_H

/* Sprint 44: deterministic, no-LSP symbol completion. */

#include <stdbool.h>

#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/intern.h"
#include "util/strmap.h"
#include "util/vec.h"
#include "ws/finder.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct EditCtx EditCtx;
typedef struct Workspace Workspace;

enum {
    YEW_SYM_MIN_LEN = 3,
    YEW_SYM_MAX_LEN = 128,
    YEW_SYM_QUERY_MAX = 200,
    YEW_SYM_GHOST_MIN = 400,
    YEW_SYM_GHOST_MIN_STEM = 3,
    YEW_SYMIDX_BURST_US = 500,
    YEW_SYMIDX_DIRTY_MAX_LINES = 512,
    YEW_SYMIDX_FULL_US = 4000,
    YEW_SYMIDX_BYTES_MAX = 32 * 1024 * 1024
};

typedef enum SymKind {
    YEW_SYMK_WORD = 0,
    YEW_SYMK_FUNC,
    YEW_SYMK_TYPE,
    YEW_SYMK_MACRO,
    YEW_SYMK_KEYWORD,
    YEW_SYMK_NKIND
} SymKind;

enum { YEW_SYMF_DECL = 1U << 0 };

typedef struct SymEntry {
    u32 name;
    u32 buf_id;
    u32 file;
    u64 off;
    u32 line;
    u16 hits;
    u8 kind;
    u8 flags;
} SymEntry;

_Static_assert(sizeof(SymEntry) <= 32, "sym entry bloat");

VEC_DECL(Vec_SymEntry, SymEntry);
VEC_DECL(Vec_SymTick, u32);
VEC_DECL(Vec_SymSig, u64);

typedef struct SymOcc {
    u32 name;
    u32 file;
    u64 off;
    u32 line;
    u32 updated;
    u8 kind;
    u8 flags;
    u16 reserved;
} SymOcc;

VEC_DECL(Vec_SymOcc, SymOcc);

typedef struct SymIndex {
    Vec_SymEntry e;
    Strmap by_name;
    Arena arena;
    u32 tick;
    u32 scan_limit;
    u64 bytes;

    /* Parallel to e. Recency is deliberately an index-update clock, not
     * wall time, so ranking is reproducible across hosts and replays. */
    Vec_SymTick updated;
    Vec_SymSig sig;
    /* Open-buffer indices retain compact occurrences so a dirty-line
     * replacement can reproduce a full scan byte-for-byte. Workspace
     * indices are rebuild-only and leave this vector disabled. */
    Vec_SymOcc occ;
    Interner *intern;
    Workspace *owner;
    u32 buf_id;
    bool capped;
    bool map_dirty;
    bool track_occ;
    bool occ_only;
} SymIndex;

typedef enum SymProx {
    YEW_PROX_CURSOR = 0,
    YEW_PROX_BUFFER,
    YEW_PROX_DIR,
    YEW_PROX_WS,
    YEW_PROX_N
} SymProx;

typedef struct SymQuery {
    const char *stem;
    u32 slen;
    u32 buf_id;
    ByteOff pos;
    u32 max;
    bool keywords;
} SymQuery;

typedef struct SymHit {
    u32 name;
    i32 rank;
    u8 kind;
    u8 prox;
    FzMatch m;
    u32 file;
    u32 line;
    /* Internal total-order key. Consumers ignore it. */
    u64 off;
} SymHit;

VEC_DECL(Vec_SymHit, SymHit);

typedef struct SymDirty {
    LineNo pre_lo;
    LineNo pre_hi;
    LineNo post_lo;
    LineNo post_hi;
    u64 old_lines;
    bool pending;
    bool have_pre;
    bool prepared;
    bool full_rebuild;
    size_t occ_base;
    Vec_SymTick affected;
} SymDirty;

typedef struct SymBufIndex {
    u32 buf_id;
    u32 full_invalidations;
    SymIndex idx;
    SymDirty dirty;
} SymBufIndex;

VEC_DECL(Vec_SymBufIndex, SymBufIndex);

void yew_symidx_init(SymIndex *idx, Interner *intern);
void yew_symidx_clear(SymIndex *idx);
void yew_symidx_free(SymIndex *idx);
void yew_symidx_workspace_free(Workspace *ws);

/* Scan a byte range using the editor's word unit and syntax spans. The
 * caller removes obsolete records first when replacing an indexed range. */
u32 yew_symidx_scan(SymIndex *idx, Buffer *buf, Span range);
/* Workspace scans use buf_id 0 even when their bytes come from an open
 * buffer.  This is the save-time and background-walk tier boundary. */
u32 yew_symidx_scan_workspace(SymIndex *idx, Buffer *buf, Span range);
void yew_symidx_workspace_replace(Workspace *ws, Buffer *buf);
u64 yew_symidx_workspace_bytes(const Workspace *ws);
void yew_symidx_reindex_buffers(Workspace *ws);

i32 yew_sym_rank(i32 fuzzy, u32 age, SymProx prox, u8 kind, u16 hits);
u32 yew_symidx_query(Workspace *ws, const SymQuery *q, SymHit *out, u32 max);

SymIndex *yew_symidx_buffer(Workspace *ws, u32 buf_id, bool create);
void yew_symidx_drop_buffer(Workspace *ws, u32 buf_id);
void yew_symidx_invalidate_buffer(Ed *ed, Buffer *buf);
void yew_symidx_note_pre(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_symidx_note_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);
bool yew_symidx_pending(const Ed *ed);
void yew_symidx_pump(Ed *ed, i64 budget_us);

/* Non-goal for 1.0: no filesystem watcher or global/cross-workspace index.
 * Sprint 47 supplies documentation and LSP items through the same query
 * consumers; index records intentionally carry no documentation. */

#endif /* YEW_WS_SYMIDX_H */
