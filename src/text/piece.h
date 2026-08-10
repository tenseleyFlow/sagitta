#ifndef YEW_TEXT_PIECE_H
#define YEW_TEXT_PIECE_H

#include <stdbool.h>

#include "text/coords.h"
#include "util/base.h"
#include "util/vec.h"

/* A 3:1 WBT over the full u32 node domain can exceed 64 levels. */
#define YEW_PIECE_MAX_DEPTH 128

VEC_DECL(YewU64Vec, u64);

typedef enum {
    YEW_STORE_ORIG = 0,
    YEW_STORE_ADD = 1
} PieceSrc;

typedef struct {
    u8 *bytes;
    u64 len;
    u64 cap;
    YewU64Vec lfs;
} TextStore;

/* Shared by a live buffer and every snapshot made from it. */
typedef struct TextBacking {
    TextStore orig;                 /* immutable after construction */
    TextStore add;                  /* append-only until last release */
    u32 refs;                       /* single-threaded ownership count */
} TextBacking;

typedef struct {
    u8 src;
    Span span;
    u64 lf_first;
    u64 lf_count;
} Piece;

typedef struct PieceNode {
    struct PieceNode *left;
    struct PieceNode *right;
    Span span;
    u64 sub_bytes;
    u64 sub_lfs;
    u64 lf_count;
    u32 sub_count;
    u32 refs;
    u8 src;
    u64 lf_first;
} PieceNode;

_Static_assert(sizeof(PieceNode) <= 96, "node bloat");

typedef struct {
    PieceNode *root;
    TextBacking *backing;
    u64 len;
    u64 gen;
    bool active;
} TextSnap;

typedef struct {
    u64 off;
    u64 gcol;
} YewGraphemeCheckpoint;

typedef struct {
    u64 off;
    u64 cluster_start;
    u64 gcol;
    u8 prev_gcb;
    u8 flags;
    bool have_cluster;
    bool after_lf;
} YewGraphemeMotionCheckpoint;

typedef struct {
    Span range;
    Span affected;
    u64 inserted_len;
    u64 old_gen;
    u64 new_gen;
    TextSnap after;
} YewGraphemePendingEdit;

enum { YEW_GRAPHEME_PENDING_MAX = 2 };

typedef struct {
    YewGraphemePendingEdit edits[YEW_GRAPHEME_PENDING_MAX];
    u8 len;
} YewGraphemePendingJournal;

typedef struct {
    YewGraphemeCheckpoint *data;
    size_t len;
    size_t cap;
    YewGraphemeMotionCheckpoint *motion;
    size_t motion_len;
    size_t motion_cap;
    u64 gen;
    bool simple_ascii;             /* one printable-ASCII line */
    bool simple_ascii_direct;      /* formulas supersede stale arrays */
    bool initialized;              /* false for deferred non-ASCII files */
    /* Adjacent after-state snapshots are replayed before this queue fills. */
    YewGraphemePendingJournal pending;
} YewGraphemeIndex;

typedef struct TextBuf {
    TextBacking *backing;
    /* Read-only compatibility views; mutation is owned by piece.c. */
    TextStore orig;
    TextStore add;
    PieceNode *root;
    u64 gen;
    YewGraphemeIndex graphemes;
    u64 add_tail_at;               /* buffer end of latest add-store run */
    bool add_tail_known;           /* false after delete; probe once */
} TextBuf;

typedef struct {
    const PieceNode *stack[YEW_PIECE_MAX_DEPTH];
    const TextBuf *owner;           /* live iter only; NULL for snapshot */
    TextBacking *backing;           /* borrowed; a snapshot pins it */
    u32 depth;
    u64 gen;
    u64 skip;
    bool snapshot;
} TextIter;

typedef struct {
    const TextBuf *owner;
    u64 next_line;
    u64 gen;
} LineIter;

/* Constructors allocate a buffer; from_bytes copies its input. */
TextBuf *yew_textbuf_new(void);
TextBuf *yew_textbuf_from_bytes(const u8 *bytes, u64 len);
/* Transfers bytes to the immutable original store, including for len == 0. */
TextBuf *yew_textbuf_from_owned_bytes(u8 *bytes, u64 len);
/* Frees the live buffer; snapshots and their backing bytes remain valid. */
void yew_textbuf_free(TextBuf *tb);

/* Payload may alias either backing store; the implementation stages it. */
void yew_textbuf_insert(TextBuf *tb, ByteOff at, const u8 *bytes, u64 len);
/* Splices an existing immutable store span without appending its bytes. */
void yew_textbuf_insert_span(TextBuf *tb, ByteOff at, u8 src, Span span);
void yew_textbuf_delete(TextBuf *tb, Span range);

u64 yew_textbuf_len(const TextBuf *tb);
u64 yew_textbuf_line_count(const TextBuf *tb);
u32 yew_textbuf_piece_count(const TextBuf *tb);
ByteOff yew_textbuf_line_start(const TextBuf *tb, LineNo line);
LineNo yew_textbuf_line_of(const TextBuf *tb, ByteOff off);
Span yew_textbuf_line_span(const TextBuf *tb, LineNo line);

bool yew_textiter_begin(TextIter *it, const TextBuf *tb, ByteOff at);
/* Returned bytes are borrowed: until the next live edit or snapshot release. */
bool yew_textiter_chunk(TextIter *it, const TextBuf *tb,
                        const u8 **bytes, u64 *len);
bool yew_textiter_advance(TextIter *it, const TextBuf *tb);

bool yew_lineiter_begin(LineIter *it, const TextBuf *tb, LineNo at);
bool yew_lineiter_next(LineIter *it, const TextBuf *tb, Span *line);

/* A snapshot owns one tree reference and one backing-store reference. */
TextSnap yew_textbuf_snap(TextBuf *tb);
/* tb validates ownership when live; pass NULL after yew_textbuf_free(tb). */
void yew_textsnap_release(TextBuf *tb, TextSnap *snap);
/* Snapshot iterators borrow the snapshot; release it only after iteration. */
bool yew_textsnap_iter(TextIter *it, const TextSnap *snap, ByteOff at);

void yew_textbuf_check(const TextBuf *tb);

#endif
