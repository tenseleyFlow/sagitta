#ifndef SAG_TEST_FUZZ_SHRINK_H
#define SAG_TEST_FUZZ_SHRINK_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"
#include "util/buf.h"

typedef enum {
    TRACE_INS,
    TRACE_DEL,
    TRACE_LINE_START,
    TRACE_LINE_OF,
    TRACE_LINE_SPAN,
    TRACE_ITER,
    TRACE_SNAP,
    TRACE_RELEASE,
    TRACE_UNDO,
    TRACE_REDO,
    TRACE_UNDO_BOUNDARY,
    TRACE_UNDO_TO,
    TRACE_SAVE,
    TRACE_CHECK
} TraceOpKind;

typedef enum {
    TRACE_CONTENT_ASCII,
    TRACE_CONTENT_UTF8,
    TRACE_CONTENT_GRAPHEME,
    TRACE_CONTENT_INVALID,
    TRACE_CONTENT_BINARY,
    TRACE_CONTENT_NEWLINES,
    TRACE_CONTENT_HUGE_LINE,
    TRACE_CONTENT_CRLF
} TraceContentClass;

typedef struct {
    TraceOpKind kind;
    u64 a;
    u64 b;
    u64 ordinal;
    TraceContentClass content_class;
    Bytebuf payload;
} TraceOp;

typedef struct Trace {
    TraceOp *ops;
    size_t len;
    size_t cap;
    u64 seed;
    char mix[32];
    char base[256];
} Trace;

typedef enum {
    TRACE_FAILURE_CHECK,
    TRACE_FAILURE_ASSERTION
} TraceFailureKind;

typedef struct {
    TraceFailureKind kind;
    u64 first_op;
    u32 check_id;
    char assertion[128];
} TraceFailure;

typedef bool (*FailureProbe)(const Trace *trace, TraceFailure *failure,
                             void *context);

typedef struct FailurePred {
    FailureProbe probe;
    void *context;
    TraceFailure target;
    u32 max_replays; /* zero selects the binding 5,000-replay budget */
    u64 max_ns;      /* zero selects the binding ten-second budget */
    u32 *replays_out;
} FailurePred;

void trace_init(Trace *trace);
void trace_free(Trace *trace);
bool trace_push(Trace *trace, TraceOpKind kind, u64 a, u64 b,
                const u8 *payload, size_t payload_len);
bool trace_parse(Trace *trace, const u8 *text, size_t len,
                 char *why, size_t why_cap);
void trace_write(const Trace *trace, Bytebuf *out);
void trace_write_c_snippet(const Trace *trace, Bytebuf *out);
bool trace_failure_equal(const TraceFailure *a, const TraceFailure *b);

/* Returns true only when the minimized trace still reproduces target. */
bool shrink(Trace *trace, FailurePred pred);

#endif
