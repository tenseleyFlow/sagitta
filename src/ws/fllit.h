#ifndef YEW_WS_FLLIT_H
#define YEW_WS_FLLIT_H

/*
 * Sprint 25 §2/§4: Fletch pure data literals — the workspace-state
 * format, FROZEN as v1.
 *
 * Six value kinds and nothing else: map, list, string, i64, bool, nil.
 *
 * THERE ARE NO FLOATS.  The only real number in the model is a pane
 * ratio, carried as `ratio_permille` in [1, 999].  Two reasons, both
 * fatal: a floating-point conversion honours LC_NUMERIC, so a user in a
 * de_DE locale emits `0,5` and every determinism golden breaks on their
 * machine; and float text does not round-trip exactly, so
 * save->restore->save would not be a fixpoint.
 *
 * DoD 6 greps src/ws/ for the float conversion and parsing specifiers
 * and expects nothing, so this paragraph deliberately names none of
 * them — a tripwire that a comment can trip is one a reviewer learns to
 * wave through.
 *
 * SPRINT 36 REPLACES THIS CODE WITH THE FLETCH VM DATA PATH.  The
 * FORMAT is the contract; the implementation is not.  Everything the
 * emitter pins below — indent, key order, trailing commas, the escape
 * table — is therefore part of the specification, not a style choice,
 * and tests/unit/fixtures/wsstate/v1 is the corpus s36 must reproduce
 * byte for byte.
 */

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

/*
 * FL_LIT_*, not FL_*, since Sprint 34.
 *
 * Sprint 30's FlType spells six of its tags FL_NIL, FL_BOOL, FL_INT,
 * FL_STR, FL_LIST and FL_MAP -- the same six kinds, the same six
 * names.  Nothing included both headers until Sprint 34's editor
 * bindings needed ed.h and fl/value.h in one translation unit, and the
 * collision was then a hard error in a file that had touched neither.
 * Prefixing here rather than there: this format is frozen but its
 * implementation is not (see above), and FlType's spellings are what
 * `type_of` reports to a user.
 */
typedef enum {
    FL_LIT_NIL = 0,
    FL_LIT_BOOL,
    FL_LIT_INT,
    FL_LIT_STR,
    FL_LIT_LIST,
    FL_LIT_MAP
} WsFlLitKind;

typedef struct FlLit FlLit;

/*
 * Maps are ARRAYS of key/value pairs, kept in insertion order.
 *
 * Not a hash map, on purpose and twice over: at schema sizes a linear
 * scan beats hashing, and a hash map's iteration order would leak into
 * re-emission and break determinism (invariant 5).
 */
struct FlLit {
    WsFlLitKind kind;
    /* FL_LIT_BOOL / FL_LIT_INT */
    i64 i;
    /* FL_LIT_STR: bytes, NOT text.  Never validated as UTF-8 — paths are
     * bytes, and rejecting an invalid sequence loses exactly the files
     * invariant 2 exists to protect. */
    const char *s;
    u64 slen;
    /* FL_LIT_LIST / FL_LIT_MAP */
    FlLit **items;
    const char **keys; /* FL_LIT_MAP only; parallel to items */
    u64 *keylens;
    u32 len;
    u32 cap;
};

typedef struct FlParseErr {
    u64 off;
    u32 line, col;
    const char *msg;
} FlParseErr;

enum {
    /* Whole-document caps.  A breach is CORRUPTION (§7), never a
     * truncation the reader has to notice later. */
    YEW_FL_MAX_BYTES = 8U * 1024U * 1024U,
    YEW_FL_MAX_NODES = 1000000U,
    YEW_FL_MAX_DEPTH = 32U,
    YEW_FL_MAX_STRING = 4096U
};

/*
 * Total over the §2 subset.  NULL on failure with `err` filled in.
 *
 * Syntax and the structural caps only: ranges, enum spellings and
 * required-field checks belong to the schema layer, where a failure can
 * be attributed to a field NAME in the log.  A parser that also
 * validated semantics would have to be rewritten wholesale in s36.
 */
FlLit *yew_fl_parse(Arena *a, const u8 *src, u64 len, FlParseErr *err);

const FlLit *yew_fl_get(const FlLit *map, const char *key);
u32 yew_fl_len(const FlLit *list);
const FlLit *yew_fl_at(const FlLit *list, u32 i);
/* Type-safe readers: a wrong-typed value takes its default.  A state
 * file is a cache, and one bad field must not cost the user a layout. */
i64 yew_fl_int_or(const FlLit *v, i64 dflt);
bool yew_fl_bool_or(const FlLit *v, bool dflt);
const char *yew_fl_str_or(const FlLit *v, const char *dflt, u64 *n);

/* ---------------------------------------------------------------- */
/* Emitter                                                          */
/* ---------------------------------------------------------------- */

/*
 * Depth is tracked HERE, never by callers.  An unbalanced open/close
 * is a YEW_BUG at close time rather than a document that parses into
 * the wrong shape.
 */
typedef struct FlEmit {
    Bytebuf *out;
    u32 depth;
    /* Guards the balance check without walking anything. */
    u32 opened;
} FlEmit;

void yew_fl_emit_init(FlEmit *e, Bytebuf *out);
void yew_fl_emit_done(const FlEmit *e); /* YEW_BUG if unbalanced */

void yew_fl_map_open(FlEmit *e, const char *key); /* key NULL at root */
void yew_fl_map_close(FlEmit *e);
void yew_fl_list_open(FlEmit *e, const char *key);
void yew_fl_list_close(FlEmit *e);
void yew_fl_str(FlEmit *e, const char *key, const char *s, u64 n);
void yew_fl_int(FlEmit *e, const char *key, i64 v);
void yew_fl_bool(FlEmit *e, const char *key, bool v);
void yew_fl_nil(FlEmit *e, const char *key);

/* Re-emits a parsed tree in canonical form.  This is what makes
 * emit(parse(d)) a fixpoint, and what s36's differential test compares
 * across two independent parsers. */
void yew_fl_emit_lit(FlEmit *e, const char *key, const FlLit *v);

#endif
