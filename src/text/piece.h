#ifndef SAG_TEXT_PIECE_H
#define SAG_TEXT_PIECE_H

#include <stdbool.h>

#include "text/coords.h"
#include "util/base.h"
#include "util/vec.h"

/* A 3:1 WBT over the full u32 node domain can exceed 64 levels. */
#define SAG_PIECE_MAX_DEPTH 128

VEC_DECL(SagU64Vec, u64);

typedef enum {
    SAG_STORE_ORIG = 0,
    SAG_STORE_ADD = 1
} PieceSrc;

typedef struct {
    u8 *bytes;
    u64 len;
    u64 cap;
    SagU64Vec lfs;
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
} SagGraphemeCheckpoint;

typedef struct {
    u64 off;
    u64 cluster_start;
    u64 gcol;
    u8 prev_gcb;
    u8 flags;
    bool have_cluster;
    bool after_lf;
} SagGraphemeMotionCheckpoint;

typedef struct {
    Span range;
    Span affected;
    u64 inserted_len;
    u64 old_gen;
    u64 new_gen;
    TextSnap after;
} SagGraphemePendingEdit;

enum { SAG_GRAPHEME_PENDING_MAX = 2 };

typedef struct {
    SagGraphemePendingEdit edits[SAG_GRAPHEME_PENDING_MAX];
    u8 len;
    bool rebuild_required;
} SagGraphemePendingJournal;

typedef struct {
    SagGraphemeCheckpoint *data;
    size_t len;
    size_t cap;
    SagGraphemeMotionCheckpoint *motion;
    size_t motion_len;
    size_t motion_cap;
    u64 gen;
    /* Adjacent after-state snapshots make a short edit burst replayable. */
    SagGraphemePendingJournal pending;
} SagGraphemeIndex;

typedef struct TextBuf {
    TextBacking *backing;
    /* Read-only compatibility views; mutation is owned by piece.c. */
    TextStore orig;
    TextStore add;
    PieceNode *root;
    u64 gen;
    SagGraphemeIndex graphemes;
    u64 add_tail_at;               /* buffer end of latest add-store run */
    bool add_tail_known;           /* false after delete; probe once */
} TextBuf;

typedef struct {
    const PieceNode *stack[SAG_PIECE_MAX_DEPTH];
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
TextBuf *sag_textbuf_new(void);
TextBuf *sag_textbuf_from_bytes(const u8 *bytes, u64 len);
/* Transfers bytes to the immutable original store, including for len == 0. */
TextBuf *sag_textbuf_from_owned_bytes(u8 *bytes, u64 len);
/* Frees the live buffer; snapshots and their backing bytes remain valid. */
void sag_textbuf_free(TextBuf *tb);

/* Payload may alias either backing store; the implementation stages it. */
void sag_textbuf_insert(TextBuf *tb, ByteOff at, const u8 *bytes, u64 len);
void sag_textbuf_delete(TextBuf *tb, Span range);

u64 sag_textbuf_len(const TextBuf *tb);
u64 sag_textbuf_line_count(const TextBuf *tb);
u32 sag_textbuf_piece_count(const TextBuf *tb);
ByteOff sag_textbuf_line_start(const TextBuf *tb, LineNo line);
LineNo sag_textbuf_line_of(const TextBuf *tb, ByteOff off);
Span sag_textbuf_line_span(const TextBuf *tb, LineNo line);

bool sag_textiter_begin(TextIter *it, const TextBuf *tb, ByteOff at);
/* Returned bytes are borrowed: until the next live edit or snapshot release. */
bool sag_textiter_chunk(TextIter *it, const TextBuf *tb,
                        const u8 **bytes, u64 *len);
bool sag_textiter_advance(TextIter *it, const TextBuf *tb);

bool sag_lineiter_begin(LineIter *it, const TextBuf *tb, LineNo at);
bool sag_lineiter_next(LineIter *it, const TextBuf *tb, Span *line);

/* A snapshot owns one tree reference and one backing-store reference. */
TextSnap sag_textbuf_snap(TextBuf *tb);
/* tb validates ownership when live; pass NULL after sag_textbuf_free(tb). */
void sag_textsnap_release(TextBuf *tb, TextSnap *snap);
/* Snapshot iterators borrow the snapshot; release it only after iteration. */
bool sag_textsnap_iter(TextIter *it, const TextSnap *snap, ByteOff at);

void sag_textbuf_check(const TextBuf *tb);

#endif
