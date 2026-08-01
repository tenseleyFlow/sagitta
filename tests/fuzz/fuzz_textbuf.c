#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "edit/multicursor.h"
#include "oracle.h"
#include "shrink.h"
#include "text/edit.h"
#include "unicode/coords.h"

enum {
    SAG_TEXT_FUZZ_DEFAULT_ITERS = 200000,
    SAG_TEXT_FUZZ_MAX_LIVE = 64 * 1024,
    SAG_TEXT_FUZZ_MAX_LINES = 8192,
    SAG_TEXT_FUZZ_SNAPSHOTS = 4,
    SAG_TEXT_FUZZ_MARKS = 8,
    SAG_TEXT_FUZZ_CHECK_ARGS = 8
};

typedef enum {
    MIX_TYPING,
    MIX_PASTE,
    MIX_UNDO,
    MIX_LINES
} Mix;

typedef enum {
    CHECK_LEN = 1,
    CHECK_LINES,
    CHECK_PIECES,
    CHECK_LINE_QUERY,
    CHECK_MATERIALIZE,
    CHECK_ITERATOR,
    CHECK_ALL_LINES,
    CHECK_CURSOR,
    CHECK_MARK,
    CHECK_SNAPSHOT,
    CHECK_UNDO_STATE,
    CHECK_OPERATION
} CheckId;

typedef struct {
    bool present;
    Bytebuf bytes;
    Cursor cursor;
    u64 marks[SAG_TEXT_FUZZ_MARKS];
} NodeState;

typedef struct {
    TextBuf *tb;
    UndoTree *undo;
    EditCtx edit;
    CursorSet cursors;
    MarkSet *marks;
    MarkId mark_ids[SAG_TEXT_FUZZ_MARKS];
    Oracle oracle;
    TextSnap snapshots[SAG_TEXT_FUZZ_SNAPSHOTS];
    Bytebuf snapshot_bytes[SAG_TEXT_FUZZ_SNAPSHOTS];
    bool snapshot_live[SAG_TEXT_FUZZ_SNAPSHOTS];
    NodeState *states;
    size_t state_cap;
    u64 clock_ms;
    i64 wall_sec;
    u64 mutation_count;
    bool full_checks;
} Replay;

typedef struct {
    u64 rng;
    Mix mix;
    Trace trace;
    size_t iterations;
    size_t class_counts[TRACE_CONTENT_CRLF + 1U];
    size_t sequence_splits;
    u64 trace_hash;
} Generator;

typedef struct {
    const Trace *trace;
    bool full_checks;
} ProbeCtx;

static u64 rng_next(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 mix64(u64 value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static size_t choose(u64 *state, size_t limit)
{
    return limit == 0U ? 0U : (size_t)(rng_next(state) % (u64)limit);
}

static u64 mono_clock(void *context)
{
    return ((Replay *)context)->clock_ms;
}

static i64 wall_clock(void *context)
{
    return ((Replay *)context)->wall_sec;
}

static void materialize_text(const TextBuf *tb, Bytebuf *out)
{
    TextIter iter;

    out->len = 0U;
    if (!sag_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return;
    do {
        const u8 *bytes;
        u64 len;

        if (!sag_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            SAG_BUG("fuzz_textbuf: invalid live iterator");
        bytebuf_append(out, bytes, (size_t)len);
    } while (sag_textiter_advance(&iter, tb));
}

static void materialize_snapshot(const TextBuf *tb, const TextSnap *snap,
                                 Bytebuf *out)
{
    TextIter iter;

    out->len = 0U;
    if (!sag_textsnap_iter(&iter, snap, BYTEOFF(0U)))
        return;
    do {
        const u8 *bytes;
        u64 len;

        if (!sag_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            SAG_BUG("fuzz_textbuf: invalid snapshot iterator");
        bytebuf_append(out, bytes, (size_t)len);
    } while (sag_textiter_advance(&iter, tb));
}

static bool bytes_equal(const Bytebuf *a, const Bytebuf *b)
{
    return a->len == b->len &&
           (a->len == 0U || memcmp(a->data, b->data, a->len) == 0);
}

static void state_reserve(Replay *run, u32 id)
{
    size_t cap;

    if ((size_t)id < run->state_cap)
        return;
    cap = run->state_cap == 0U ? 16U : run->state_cap;
    while (cap <= (size_t)id) {
        if (cap > SIZE_MAX / 2U)
            SAG_BUG("fuzz_textbuf: state table overflow");
        cap *= 2U;
    }
    run->states = sag_xreallocarray(run->states, cap, sizeof(*run->states));
    (void)memset(run->states + run->state_cap, 0,
                 (cap - run->state_cap) * sizeof(*run->states));
    run->state_cap = cap;
}

static void state_capture(Replay *run, u32 id)
{
    NodeState *state;
    Bytebuf bytes;
    size_t i;

    state_reserve(run, id);
    state = &run->states[id];
    bytebuf_init(&bytes);
    oracle_materialize(&run->oracle, &bytes);
    if (state->present)
        bytebuf_free(&state->bytes);
    state->bytes = bytes;
    state->cursor = run->cursors.curs.data[run->cursors.primary];
    for (i = 0U; i < SAG_TEXT_FUZZ_MARKS; i++)
        state->marks[i] = sag_mark_pos(run->marks, run->mark_ids[i]).v;
    state->present = true;
}

static void state_prune_dead(Replay *run)
{
    size_t id;

    for (id = 1U; id < run->state_cap; id++) {
        NodeState *state = &run->states[id];
        const UndoNode *node;

        if (!state->present)
            continue;
        if (id > run->undo->nodes.len) {
            bytebuf_free(&state->bytes);
            state->present = false;
            continue;
        }
        node = &run->undo->nodes.data[id - 1U];
        if (node->id != (u32)id || (node->flags & SAG_TXN_DEAD) != 0U) {
            bytebuf_free(&state->bytes);
            state->present = false;
        }
    }
}

static bool state_restore_oracle(Replay *run, u32 id)
{
    NodeState *state;
    Bytebuf actual;
    size_t i;
    bool ok;

    if ((size_t)id >= run->state_cap || !run->states[id].present)
        return false;
    state = &run->states[id];
    bytebuf_init(&actual);
    materialize_text(run->tb, &actual);
    ok = bytes_equal(&actual, &state->bytes);
    bytebuf_free(&actual);
    if (!ok)
        return false;
    oracle_free(&run->oracle);
    oracle_init(&run->oracle, state->bytes.data, (u64)state->bytes.len);
    if (run->cursors.curs.len != 1U ||
        memcmp(&run->cursors.curs.data[0], &state->cursor,
               sizeof(state->cursor)) != 0)
        return false;
    for (i = 0U; i < SAG_TEXT_FUZZ_MARKS; i++) {
        if (sag_mark_pos(run->marks, run->mark_ids[i]).v != state->marks[i])
            return false;
    }
    return true;
}

static void replay_dispose(Replay *run)
{
    size_t i;

    for (i = 0U; i < SAG_TEXT_FUZZ_SNAPSHOTS; i++) {
        if (run->snapshot_live[i])
            sag_textsnap_release(run->tb, &run->snapshots[i]);
        bytebuf_free(&run->snapshot_bytes[i]);
    }
    for (i = 0U; i < run->state_cap; i++) {
        if (run->states[i].present)
            bytebuf_free(&run->states[i].bytes);
    }
    free(run->states);
    oracle_free(&run->oracle);
    sag_cset_free(&run->cursors);
    sag_marks_free(run->marks);
    sag_undo_free(run->undo);
    sag_textbuf_free(run->tb);
    (void)memset(run, 0, sizeof(*run));
}

static bool read_file_bytes(const char *path, Bytebuf *bytes)
{
    FILE *file = fopen(path, "rb");
    u8 chunk[8192];

    if (file == NULL)
        return false;
    for (;;) {
        size_t got = fread(chunk, 1U, sizeof(chunk), file);

        bytebuf_append(bytes, chunk, got);
        if (got != sizeof(chunk)) {
            bool ok = feof(file) != 0 && ferror(file) == 0;

            (void)fclose(file);
            return ok;
        }
    }
}

static bool replay_base(const char *base, Bytebuf *bytes)
{
    if (strcmp(base, "empty") == 0)
        return true;
    if (strncmp(base, "fixture:", 8U) == 0)
        return read_file_bytes(base + 8U, bytes);
    if (strncmp(base, "gen:", 4U) == 0) {
        char *end;
        unsigned long long parsed_len;
        unsigned long long parsed_seed;
        const char *seed_text;
        u64 rng;
        size_t i;

        errno = 0;
        parsed_len = strtoull(base + 4U, &end, 0);
        if (errno != 0 || end == base + 4U || *end != ':' ||
            parsed_len > SAG_TEXT_FUZZ_MAX_LIVE)
            return false;
        seed_text = end + 1U;
        errno = 0;
        parsed_seed = strtoull(seed_text, &end, 0);
        if (errno != 0 || end == seed_text || *end != '\0')
            return false;
        rng = parsed_seed == 0U ? UINT64_C(0x9e3779b97f4a7c15)
                               : (u64)parsed_seed;
        for (i = 0U; i < (size_t)parsed_len; i++)
            bytebuf_push_u8(bytes, (u8)rng_next(&rng));
        return true;
    }
    return false;
}

static bool replay_init(Replay *run, const Trace *trace)
{
    Bytebuf base;
    Cursor cursor = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    size_t i;

    (void)memset(run, 0, sizeof(*run));
    bytebuf_init(&base);
    if (!replay_base(trace->base, &base)) {
        bytebuf_free(&base);
        return false;
    }
    run->tb = sag_textbuf_from_bytes(base.data, (u64)base.len);
    if (run->tb == NULL) {
        bytebuf_free(&base);
        return false;
    }
    oracle_init(&run->oracle, base.data, (u64)base.len);
    bytebuf_free(&base);
    run->marks = sag_marks_new();
    run->undo = sag_undo_new(run->tb);
    if (run->marks == NULL || run->undo == NULL)
        goto fail;
    sag_cset_init(&run->cursors, cursor);
    for (i = 0U; i < SAG_TEXT_FUZZ_MARKS; i++) {
        run->mark_ids[i] = sag_mark_add(
            run->marks, BYTEOFF(0U),
            (i & 1U) == 0U ? SAG_BIAS_LEFT : SAG_BIAS_RIGHT);
    }
    run->edit.tb = run->tb;
    run->edit.marks = run->marks;
    run->edit.cset = &run->cursors;
    run->edit.undo = run->undo;
    run->clock_ms = 1U;
    run->wall_sec = INT64_C(1700000000);
    sag_undo_set_clock(run->undo, mono_clock, wall_clock, run);
    sag_undo_set_limits(run->undo, UINT64_C(1024) * 1024U, 16U,
                        SAG_UNDO_PERSIST_BYTES_MAX);
    for (i = 0U; i < SAG_TEXT_FUZZ_SNAPSHOTS; i++)
        bytebuf_init(&run->snapshot_bytes[i]);
    state_capture(run, sag_undo_current(run->undo));
    return true;

fail:
    if (run->undo != NULL)
        sag_undo_free(run->undo);
    if (run->marks != NULL)
        sag_marks_free(run->marks);
    sag_textbuf_free(run->tb);
    oracle_free(&run->oracle);
    (void)memset(run, 0, sizeof(*run));
    return false;
}

static bool fail_at(TraceFailure *failure, const TraceOp *op, CheckId id)
{
    if (failure != NULL) {
        failure->kind = TRACE_FAILURE_CHECK;
        failure->first_op = op->ordinal;
        failure->check_id = (u32)id;
        failure->assertion[0] = '\0';
    }
    return false;
}

static bool compare_materialized(const Replay *run)
{
    Bytebuf expected;
    Bytebuf actual;
    bool equal;

    bytebuf_init(&expected);
    bytebuf_init(&actual);
    oracle_materialize(&run->oracle, &expected);
    materialize_text(run->tb, &actual);
    equal = bytes_equal(&expected, &actual);
    bytebuf_free(&actual);
    bytebuf_free(&expected);
    return equal;
}

static bool compare_iterator_suffix(const Replay *run, u64 at)
{
    Bytebuf expected;
    Bytebuf actual;
    TextIter iter;
    bool equal;

    bytebuf_init(&expected);
    bytebuf_init(&actual);
    oracle_materialize(&run->oracle, &expected);
    if (at > expected.len)
        at = expected.len;
    if (sag_textiter_begin(&iter, run->tb, BYTEOFF(at))) {
        do {
            const u8 *bytes;
            u64 len;

            if (!sag_textiter_chunk(&iter, run->tb, &bytes, &len) ||
                len == 0U) {
                bytebuf_free(&actual);
                bytebuf_free(&expected);
                return false;
            }
            bytebuf_append(&actual, bytes, (size_t)len);
        } while (sag_textiter_advance(&iter, run->tb));
    }
    equal = actual.len == expected.len - (size_t)at &&
            (actual.len == 0U ||
             memcmp(actual.data, expected.data + (size_t)at,
                    actual.len) == 0);
    bytebuf_free(&actual);
    bytebuf_free(&expected);
    return equal;
}

static bool compare_all_lines(const Replay *run)
{
    u64 at = 0U;
    size_t line;

    if (sag_textbuf_line_count(run->tb) != run->oracle.lines.len)
        return false;
    for (line = 0U; line < run->oracle.lines.len; line++) {
        u64 next = at + (u64)run->oracle.lines.data[line].len;
        Span span = sag_textbuf_line_span(run->tb, LINENO((u64)line));

        if (sag_textbuf_line_start(run->tb, LINENO((u64)line)).v != at ||
            span.lo != at || span.hi != next)
            return false;
        at = next;
    }
    return at == oracle_len(&run->oracle);
}

static bool compare_line_queries(const Replay *run, u64 salt)
{
    u64 len = oracle_len(&run->oracle);
    u64 lines = oracle_line_count(&run->oracle);
    size_t i;

    for (i = 0U; i < SAG_TEXT_FUZZ_CHECK_ARGS; i++) {
        u64 random = mix64(salt + (u64)i * UINT64_C(0x9e3779b97f4a7c15));
        u64 off = len == UINT64_MAX ? random : random % (len + 1U);
        u64 line = random % lines;
        u64 want_start = oracle_line_start(&run->oracle, line);
        u64 want_of = oracle_line_of(&run->oracle, off);
        u64 want_hi = line + 1U < lines
                          ? oracle_line_start(&run->oracle, line + 1U)
                          : len;
        Span actual_span = sag_textbuf_line_span(run->tb, LINENO(line));

        if (sag_textbuf_line_start(run->tb, LINENO(line)).v != want_start ||
            sag_textbuf_line_of(run->tb, BYTEOFF(off)).v != want_of ||
            actual_span.lo != want_start || actual_span.hi != want_hi)
            return false;
    }
    return true;
}

static bool compare_cursor_marks(const Replay *run, CheckId *which)
{
    u64 len = sag_textbuf_len(run->tb);
    size_t i;

    sag_cset_check(&run->cursors);
    for (i = 0U; i < run->cursors.curs.len; i++) {
        const Cursor *cursor = &run->cursors.curs.data[i];

        if (cursor->pos.v > len || cursor->anchor.v > len ||
            !sag_is_grapheme_boundary(run->tb, cursor->pos)) {
            *which = CHECK_CURSOR;
            return false;
        }
    }
    for (i = 0U; i < SAG_TEXT_FUZZ_MARKS; i++) {
        if (sag_mark_pos(run->marks, run->mark_ids[i]).v > len) {
            *which = CHECK_MARK;
            return false;
        }
    }
    return true;
}

static bool compare_snapshots(const Replay *run)
{
    size_t i;

    for (i = 0U; i < SAG_TEXT_FUZZ_SNAPSHOTS; i++) {
        Bytebuf actual;
        bool equal;

        if (!run->snapshot_live[i])
            continue;
        bytebuf_init(&actual);
        materialize_snapshot(run->tb, &run->snapshots[i], &actual);
        equal = bytes_equal(&actual, &run->snapshot_bytes[i]);
        bytebuf_free(&actual);
        if (!equal)
            return false;
    }
    return true;
}

static bool check_state(Replay *run, const TraceOp *op, size_t index,
                        bool final, TraceFailure *failure)
{
    CheckId which;
    u64 len = oracle_len(&run->oracle);
    u64 piece_bound;
    bool deep = run->full_checks || final || ((index + 1U) % 32U) == 0U;

    if (sag_textbuf_len(run->tb) != len)
        return fail_at(failure, op, CHECK_LEN);
    if (sag_textbuf_line_count(run->tb) != oracle_line_count(&run->oracle))
        return fail_at(failure, op, CHECK_LINES);
    sag_textbuf_check(run->tb);
    piece_bound = 2U * (u64)(index + 1U) + 1U;
    if ((u64)sag_textbuf_piece_count(run->tb) > piece_bound)
        return fail_at(failure, op, CHECK_PIECES);
    if (!compare_line_queries(run, op->ordinal ^ (u64)index))
        return fail_at(failure, op, CHECK_LINE_QUERY);
    if (!compare_cursor_marks(run, &which))
        return fail_at(failure, op, which);
    if (deep && !compare_snapshots(run))
        return fail_at(failure, op, CHECK_SNAPSHOT);
    if (deep && !compare_materialized(run))
        return fail_at(failure, op, CHECK_MATERIALIZE);
    if (deep && !compare_iterator_suffix(run, mix64(op->ordinal) %
                                                (len + 1U)))
        return fail_at(failure, op, CHECK_ITERATOR);
    if ((run->full_checks || final || len < 64U * 1024U ||
         ((index + 1U) % 512U) == 0U) &&
        !compare_all_lines(run))
        return fail_at(failure, op, CHECK_ALL_LINES);
    return true;
}

static u32 choose_state_id(const Replay *run, u64 choice)
{
    size_t count = 0U;
    size_t target;
    size_t i;

    for (i = 1U; i < run->state_cap; i++) {
        if (run->states[i].present)
            count++;
    }
    if (count == 0U)
        return sag_undo_current(run->undo);
    target = (size_t)(choice % (u64)count);
    for (i = 1U; i < run->state_cap; i++) {
        if (!run->states[i].present)
            continue;
        if (target-- == 0U)
            return (u32)i;
    }
    return sag_undo_current(run->undo);
}

static bool save_reload_compare(const Replay *run)
{
    char path[] = "/tmp/sagitta-fuzz-textbuf-XXXXXX";
    FileMeta saved_meta;
    FileMeta loaded_meta;
    TextBuf *loaded = NULL;
    Bytebuf expected;
    Bytebuf actual;
    int fd;
    bool ok = false;

    fd = mkstemp(path);
    if (fd < 0)
        return false;
    if (close(fd) != 0 || unlink(path) != 0)
        goto done;
    sag_filemeta_init(&saved_meta);
    sag_filemeta_init(&loaded_meta);
    if (sag_file_save(run->tb, &saved_meta, path) != SAG_SAVE_OK ||
        sag_file_load(path, &loaded, &loaded_meta) != SAG_LOAD_OK)
        goto dispose_meta;
    bytebuf_init(&expected);
    bytebuf_init(&actual);
    oracle_materialize(&run->oracle, &expected);
    materialize_text(loaded, &actual);
    ok = bytes_equal(&expected, &actual);
    bytebuf_free(&actual);
    bytebuf_free(&expected);

dispose_meta:
    if (loaded != NULL)
        sag_textbuf_free(loaded);
    sag_filemeta_dispose(&loaded_meta);
    sag_filemeta_dispose(&saved_meta);
done:
    (void)unlink(path);
    return ok;
}

static void normalize_generated_cursor(Replay *run)
{
    sag_cset_normalize(run->tb, &run->cursors);
}

static bool apply_trace_op(Replay *run, const TraceOp *op,
                           TraceFailure *failure)
{
    u64 len = oracle_len(&run->oracle);
    u64 lo;
    u64 hi;
    u32 current;

    run->clock_ms += 500U;
    run->wall_sec++;
    switch (op->kind) {
    case TRACE_INS:
        lo = op->a > len ? len : op->a;
        sag_undo_begin(&run->edit, SAG_TXN_TYPE);
        sag_edit_insert(&run->edit, BYTEOFF(lo), op->payload.data,
                        (u64)op->payload.len);
        normalize_generated_cursor(run);
        sag_undo_end(&run->edit);
        oracle_insert(&run->oracle, lo, op->payload.data,
                      (u64)op->payload.len);
        run->mutation_count++;
        state_capture(run, sag_undo_current(run->undo));
        state_prune_dead(run);
        return true;
    case TRACE_DEL:
        lo = op->a > len ? len : op->a;
        hi = op->b > len ? len : op->b;
        if (hi < lo) {
            u64 swap = lo;

            lo = hi;
            hi = swap;
        }
        sag_undo_begin(&run->edit, SAG_TXN_ERASE);
        sag_edit_delete(&run->edit, (Span){lo, hi});
        normalize_generated_cursor(run);
        sag_undo_end(&run->edit);
        oracle_delete(&run->oracle, lo, hi);
        run->mutation_count++;
        state_capture(run, sag_undo_current(run->undo));
        state_prune_dead(run);
        return true;
    case TRACE_LINE_START:
        lo = op->a % oracle_line_count(&run->oracle);
        return sag_textbuf_line_start(run->tb, LINENO(lo)).v ==
               oracle_line_start(&run->oracle, lo);
    case TRACE_LINE_OF:
        lo = op->a > len ? len : op->a;
        return sag_textbuf_line_of(run->tb, BYTEOFF(lo)).v ==
               oracle_line_of(&run->oracle, lo);
    case TRACE_LINE_SPAN: {
        Span actual;
        u64 lines = oracle_line_count(&run->oracle);
        u64 line = op->a % lines;
        u64 start = oracle_line_start(&run->oracle, line);
        u64 end = line + 1U < lines
                      ? oracle_line_start(&run->oracle, line + 1U)
                      : len;

        actual = sag_textbuf_line_span(run->tb, LINENO(line));
        return actual.lo == start && actual.hi == end;
    }
    case TRACE_ITER:
        return compare_iterator_suffix(run, op->a);
    case TRACE_SNAP: {
        size_t slot = (size_t)(op->a % SAG_TEXT_FUZZ_SNAPSHOTS);

        if (run->snapshot_live[slot])
            return true;
        run->snapshots[slot] = sag_textbuf_snap(run->tb);
        run->snapshot_bytes[slot].len = 0U;
        oracle_materialize(&run->oracle, &run->snapshot_bytes[slot]);
        run->snapshot_live[slot] = true;
        return true;
    }
    case TRACE_RELEASE: {
        size_t slot = (size_t)(op->a % SAG_TEXT_FUZZ_SNAPSHOTS);

        if (!run->snapshot_live[slot])
            return true;
        sag_textsnap_release(run->tb, &run->snapshots[slot]);
        run->snapshot_live[slot] = false;
        run->snapshot_bytes[slot].len = 0U;
        return true;
    }
    case TRACE_UNDO:
        if (sag_undo(&run->edit)) {
            current = sag_undo_current(run->undo);
            return state_restore_oracle(run, current);
        }
        return true;
    case TRACE_REDO:
        if (sag_redo(&run->edit)) {
            current = sag_undo_current(run->undo);
            return state_restore_oracle(run, current);
        }
        return true;
    case TRACE_UNDO_BOUNDARY:
        sag_undo_boundary(run->undo);
        return true;
    case TRACE_UNDO_TO:
        current = op->a < run->state_cap && run->states[op->a].present
                      ? (u32)op->a
                      : choose_state_id(run, op->a);
        if (current == sag_undo_current(run->undo))
            return true;
        return sag_undo_to(&run->edit, current) &&
               state_restore_oracle(run, current);
    case TRACE_SAVE:
        return save_reload_compare(run);
    case TRACE_CHECK:
        return true;
    }
    return fail_at(failure, op, CHECK_OPERATION);
}

static bool replay_trace(const Trace *trace, bool full_checks,
                         TraceFailure *failure, u64 *hash_out)
{
    Replay run;
    size_t i;
    bool ok = false;
    Bytebuf final;
    u64 hash = UINT64_C(1469598103934665603);

    if (!replay_init(&run, trace))
        return false;
    run.full_checks = full_checks;
    for (i = 0U; i < trace->len; i++) {
        const TraceOp *op = &trace->ops[i];

        if (!apply_trace_op(&run, op, failure)) {
            (void)fail_at(failure, op, CHECK_OPERATION);
            goto done;
        }
        if (!check_state(&run, op, i, i + 1U == trace->len, failure))
            goto done;
    }
    bytebuf_init(&final);
    oracle_materialize(&run.oracle, &final);
    for (i = 0U; i < final.len; i++) {
        hash ^= final.data[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= sag_undo_current(run.undo);
    hash *= UINT64_C(1099511628211);
    bytebuf_free(&final);
    if (hash_out != NULL)
        *hash_out = hash;
    ok = true;

done:
    replay_dispose(&run);
    return ok;
}

static bool failure_probe(const Trace *trace, TraceFailure *failure,
                          void *context)
{
    ProbeCtx *probe = context;

    (void)probe;
    return !replay_trace(trace, true, failure, NULL);
}

static u64 node_start_at(const PieceNode *node, u32 ordinal, u64 base)
{
    while (node != NULL) {
        u32 left_count = node->left != NULL ? node->left->sub_count : 0U;
        u64 left_bytes = node->left != NULL ? node->left->sub_bytes : 0U;

        if (ordinal < left_count) {
            node = node->left;
        } else if (ordinal == left_count) {
            return base + left_bytes;
        } else {
            ordinal -= left_count + 1U;
            base += left_bytes + node->span.hi - node->span.lo;
            node = node->right;
        }
    }
    SAG_BUG("fuzz_textbuf: seam ordinal out of range");
}

/* Test-only seam accessor: Sprint 11 adds no production src surface. */
static u64 textbuf_seam_at(const TextBuf *tb, u32 seam)
{
    u32 pieces = sag_textbuf_piece_count(tb);

    if (pieces < 2U || seam >= pieces - 1U)
        SAG_BUG("fuzz_textbuf: seam index out of range");
    return node_start_at(tb->root, seam + 1U, 0U);
}

static u64 choose_position(Generator *gen, const Replay *run)
{
    u64 len = sag_textbuf_len(run->tb);
    size_t bucket = choose(&gen->rng, 100U);
    u32 pieces = sag_textbuf_piece_count(run->tb);

    if (bucket < 40U)
        return choose(&gen->rng, (size_t)len + 1U);
    if (bucket < 65U && pieces > 1U)
        return textbuf_seam_at(run->tb,
                               (u32)choose(&gen->rng, pieces - 1U));
    if (bucket < 80U) {
        static const u8 extremes[] = {0U, 1U, 2U, 3U};
        u8 which = extremes[choose(&gen->rng, SAG_ARRAY_LEN(extremes))];

        if (which == 0U)
            return 0U;
        if (which == 1U)
            return len;
        if (which == 2U)
            return len == 0U ? 0U : len - 1U;
        return len < 1U ? len : 1U;
    }
    if (bucket < 90U) {
        u64 lines = sag_textbuf_line_count(run->tb);
        u64 line = (u64)choose(&gen->rng, (size_t)lines);

        if (choose(&gen->rng, 2U) == 0U)
            return sag_textbuf_line_start(run->tb, LINENO(line)).v;
        return sag_textbuf_line_span(run->tb, LINENO(line)).hi;
    }
    if (gen->trace.len != 0U) {
        const TraceOp *previous = &gen->trace.ops[gen->trace.len - 1U];
        i64 delta = (i64)choose(&gen->rng, 7U) - 3;
        u64 base = previous->kind == TRACE_INS
                       ? previous->a + (u64)previous->payload.len
                       : previous->a;

        if (delta < 0 && (u64)(-delta) > base)
            return 0U;
        if (delta > 0 && base > len - ((u64)delta > len ? len : (u64)delta))
            return len;
        base = delta < 0 ? base - (u64)(-delta) : base + (u64)delta;
        return base > len ? len : base;
    }
    return 0U;
}

static void payload_repeat(Bytebuf *payload, const u8 *bytes, size_t len,
                           size_t want)
{
    size_t at;

    for (at = 0U; at < want; at++)
        bytebuf_push_u8(payload, bytes[at % len]);
}

static void generate_payload(Generator *gen, TraceContentClass class_id,
                             size_t requested, Bytebuf *payload)
{
    static const u8 utf8[] = {0xc2U, 0xa2U, 0xe2U, 0x82U, 0xacU,
                              0xf0U, 0x9fU, 0x98U, 0x80U};
    static const u8 grapheme[] = {
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xf0U, 0x9fU, 0x87U,
        0xbaU, 0xf0U, 0x9fU, 0x87U, 0xb8U};
    static const u8 invalid[] = {0x80U, 0xc0U, 0x80U, 0xe2U, 0x82U,
                                 0xfeU, 0xffU, 0xedU, 0xa0U, 0x80U};
    size_t i;

    payload->len = 0U;
    if (class_id == TRACE_CONTENT_HUGE_LINE)
        requested = 64U * 1024U + choose(&gen->rng, 1024U);
    if (requested == 0U)
        requested = 1U;
    switch (class_id) {
    case TRACE_CONTENT_ASCII:
        for (i = 0U; i < requested; i++) {
            u64 value = rng_next(&gen->rng);

            bytebuf_push_u8(payload, (value & 15U) == 0U
                                         ? (u8)'\t'
                                         : (u8)(' ' + value % 95U));
        }
        break;
    case TRACE_CONTENT_UTF8:
        payload_repeat(payload, utf8, sizeof(utf8), requested);
        break;
    case TRACE_CONTENT_GRAPHEME:
        payload_repeat(payload, grapheme, sizeof(grapheme), requested);
        break;
    case TRACE_CONTENT_INVALID:
        payload_repeat(payload, invalid, sizeof(invalid), requested);
        break;
    case TRACE_CONTENT_BINARY:
        for (i = 0U; i < requested; i++)
            bytebuf_push_u8(payload, i % 3U == 0U
                                         ? 0U
                                         : (u8)rng_next(&gen->rng));
        break;
    case TRACE_CONTENT_NEWLINES:
        for (i = 0U; i < requested; i++)
            bytebuf_push_u8(payload, i % (1U + choose(&gen->rng, 3U)) == 0U
                                         ? (u8)'\n'
                                         : (u8)'n');
        break;
    case TRACE_CONTENT_HUGE_LINE:
        for (i = 0U; i < requested; i++)
            bytebuf_push_u8(payload, (u8)('a' + (i % 26U)));
        break;
    case TRACE_CONTENT_CRLF:
        for (i = 0U; i < requested; i++) {
            static const u8 crlf[] = {'\r', '\n', '\r', 'x'};

            bytebuf_push_u8(payload, crlf[i % sizeof(crlf)]);
        }
        break;
    }
}

static TraceContentClass choose_content(Generator *gen, bool *split)
{
    size_t choice = choose(&gen->rng, 100U);

    *split = false;
    if (choice < 30U)
        return TRACE_CONTENT_ASCII;
    if (choice < 48U)
        return TRACE_CONTENT_UTF8;
    if (choice < 57U)
        return TRACE_CONTENT_GRAPHEME;
    if (choice < 67U)
        return TRACE_CONTENT_INVALID;
    if (choice < 72U) {
        *split = true;
        return TRACE_CONTENT_ASCII;
    }
    if (choice < 77U)
        return TRACE_CONTENT_BINARY;
    if (choice < 85U)
        return TRACE_CONTENT_NEWLINES;
    if (choice < 90U)
        return TRACE_CONTENT_HUGE_LINE;
    return TRACE_CONTENT_CRLF;
}

static bool continuation_offset(const Replay *run, u64 *out)
{
    Bytebuf bytes;
    size_t i;

    bytebuf_init(&bytes);
    materialize_text(run->tb, &bytes);
    for (i = 1U; i < bytes.len; i++) {
        if ((bytes.data[i] & 0xc0U) == 0x80U) {
            *out = (u64)i;
            bytebuf_free(&bytes);
            return true;
        }
    }
    bytebuf_free(&bytes);
    return false;
}

static bool push_op(Generator *gen, TraceOpKind kind, u64 a, u64 b,
                    const Bytebuf *payload, TraceContentClass class_id)
{
    const u8 *bytes = payload != NULL ? payload->data : NULL;
    size_t len = payload != NULL ? payload->len : 0U;

    if (!trace_push(&gen->trace, kind, a, b, bytes, len))
        return false;
    gen->trace.ops[gen->trace.len - 1U].content_class = class_id;
    return true;
}

static bool forced_op(Generator *gen, const Replay *run, size_t index)
{
    Bytebuf payload;
    TraceContentClass class_id;
    u64 at = sag_textbuf_len(run->tb);

    bytebuf_init(&payload);
    if (index == 0U) {
        static const u8 euro[] = {0xe2U, 0x82U, 0xacU};

        bytebuf_append(&payload, euro, sizeof(euro));
        class_id = TRACE_CONTENT_UTF8;
        at = 0U;
    } else if (index == 1U) {
        bytebuf_push_u8(&payload, (u8)'x');
        class_id = TRACE_CONTENT_ASCII;
        at = 1U;
        gen->sequence_splits++;
    } else if (index == 2U) {
        class_id = TRACE_CONTENT_INVALID;
        generate_payload(gen, class_id, 10U, &payload);
    } else if (index == 3U) {
        class_id = TRACE_CONTENT_BINARY;
        generate_payload(gen, class_id, 9U, &payload);
    } else if (index == 4U) {
        class_id = TRACE_CONTENT_CRLF;
        generate_payload(gen, class_id, 12U, &payload);
    } else {
        class_id = TRACE_CONTENT_HUGE_LINE;
        generate_payload(gen, class_id, 64U * 1024U, &payload);
    }
    gen->class_counts[class_id]++;
    if (!push_op(gen, TRACE_INS, at, (u64)payload.len, &payload, class_id)) {
        bytebuf_free(&payload);
        return false;
    }
    bytebuf_free(&payload);
    return true;
}

static size_t insert_size(Generator *gen, size_t kind)
{
    if (kind == 0U)
        return 1U + choose(&gen->rng, 8U);
    if (kind == 1U)
        return 9U + choose(&gen->rng, 4088U);
    if (choose(&gen->rng, 128U) == 0U)
        return 1024U * 1024U;
    return (size_t)4096U << choose(&gen->rng, 5U);
}

static size_t op_choice(const Generator *gen, u64 random)
{
    size_t value = (size_t)(random % 100U);

    if (gen->mix == MIX_TYPING)
        return value;
    if (gen->mix == MIX_PASTE)
        return (value + 20U) % 100U;
    if (gen->mix == MIX_UNDO)
        return (value + 50U) % 100U;
    return (value + 72U) % 100U;
}

static bool generate_insert(Generator *gen, const Replay *run, size_t size_id)
{
    Bytebuf payload;
    TraceContentClass class_id;
    bool split;
    u64 at;

    bytebuf_init(&payload);
    class_id = choose_content(gen, &split);
    generate_payload(gen, class_id, insert_size(gen, size_id), &payload);
    at = choose_position(gen, run);
    if (split && continuation_offset(run, &at))
        gen->sequence_splits++;
    else if (split)
        split = false;
    gen->class_counts[class_id]++;
    if (!push_op(gen, TRACE_INS, at, (u64)payload.len, &payload, class_id)) {
        bytebuf_free(&payload);
        return false;
    }
    bytebuf_free(&payload);
    return true;
}

static bool generate_delete(Generator *gen, const Replay *run,
                            size_t kind)
{
    u64 len = sag_textbuf_len(run->tb);
    u64 lo;
    u64 hi;

    if (len == 0U)
        return generate_insert(gen, run, 0U);
    if (kind == 2U) {
        lo = 0U;
        hi = len;
    } else {
        lo = choose_position(gen, run);
        if (lo == len && len != 0U)
            lo--;
        hi = lo + 1U + (u64)choose(&gen->rng,
                                   kind == 0U ? 8U : 64U * 1024U);
        if (hi > len)
            hi = len;
    }
    return push_op(gen, TRACE_DEL, lo, hi, NULL, TRACE_CONTENT_ASCII);
}

static bool generate_op(Generator *gen, const Replay *run, size_t index)
{
    size_t choice;
    u64 len = sag_textbuf_len(run->tb);

    if (index < 6U)
        return forced_op(gen, run, index);
    if (len > SAG_TEXT_FUZZ_MAX_LIVE ||
        sag_textbuf_line_count(run->tb) > SAG_TEXT_FUZZ_MAX_LINES)
        return push_op(gen, TRACE_DEL, 0U, len, NULL,
                       TRACE_CONTENT_ASCII);
    choice = op_choice(gen, rng_next(&gen->rng));
    if (choice < 26U)
        return generate_insert(gen, run, 0U);
    if (choice < 37U)
        return generate_insert(gen, run, 1U);
    if (choice < 39U)
        return generate_insert(gen, run, 2U);
    if (choice < 56U)
        return generate_delete(gen, run, 0U);
    if (choice < 65U)
        return generate_delete(gen, run, 1U);
    if (choice < 66U)
        return generate_delete(gen, run, 2U);
    if (choice < 76U) {
        static const TraceOpKind queries[] = {
            TRACE_LINE_START, TRACE_LINE_OF, TRACE_LINE_SPAN};

        return push_op(gen, queries[choose(&gen->rng, SAG_ARRAY_LEN(queries))],
                       rng_next(&gen->rng), 0U, NULL, TRACE_CONTENT_ASCII);
    }
    if (choice < 81U)
        return push_op(gen, TRACE_ITER, rng_next(&gen->rng), 0U, NULL,
                       TRACE_CONTENT_ASCII);
    if (choice < 85U) {
        TraceOpKind kind = choose(&gen->rng, 2U) == 0U
                               ? TRACE_SNAP : TRACE_RELEASE;

        return push_op(gen, kind, choose(&gen->rng,
                                         SAG_TEXT_FUZZ_SNAPSHOTS),
                       0U, NULL, TRACE_CONTENT_ASCII);
    }
    if (choice < 90U)
        return push_op(gen, TRACE_UNDO, 0U, 0U, NULL, TRACE_CONTENT_ASCII);
    if (choice < 93U)
        return push_op(gen, TRACE_REDO, 0U, 0U, NULL, TRACE_CONTENT_ASCII);
    if (choice < 95U)
        return push_op(gen, TRACE_UNDO_BOUNDARY, 0U, 0U, NULL,
                       TRACE_CONTENT_ASCII);
    if (choice < 97U)
        return push_op(gen, TRACE_UNDO_TO,
                       choose_state_id(run, rng_next(&gen->rng)), 0U, NULL,
                       TRACE_CONTENT_ASCII);
    if (choice < 98U)
        return push_op(gen, TRACE_SAVE, 0U, 0U, NULL, TRACE_CONTENT_ASCII);
    return push_op(gen, TRACE_CHECK, 0U, 0U, NULL, TRACE_CONTENT_ASCII);
}

static u64 fnv_bytes(const u8 *bytes, size_t len)
{
    u64 hash = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool write_file(const char *path, const u8 *bytes, size_t len)
{
    FILE *file = fopen(path, "wb");
    bool ok;
    int close_result;

    if (file == NULL)
        return false;
    ok = (len == 0U || fwrite(bytes, 1U, len, file) == len) &&
         fflush(file) == 0;
    close_result = fclose(file);
    return ok && close_result == 0;
}

static const char *mix_name(Mix mix)
{
    static const char *const names[] = {"typing", "paste", "undo", "lines"};

    return names[(size_t)mix];
}

static bool parse_mix(const char *name, Mix *out)
{
    size_t i;

    for (i = 0U; i <= (size_t)MIX_LINES; i++) {
        if (strcmp(name, mix_name((Mix)i)) == 0) {
            *out = (Mix)i;
            return true;
        }
    }
    return false;
}

static bool parse_u64_arg(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || *text == '\0' || *text == '-')
        return false;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *out = (u64)value;
    return true;
}

static u64 monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        SAG_BUG("fuzz_textbuf: CLOCK_MONOTONIC failed");
    return (u64)now.tv_sec * UINT64_C(1000000000) + (u64)now.tv_nsec;
}

static bool class_coverage(const Generator *gen)
{
    return gen->class_counts[TRACE_CONTENT_INVALID] != 0U &&
           gen->class_counts[TRACE_CONTENT_BINARY] != 0U &&
           gen->class_counts[TRACE_CONTENT_CRLF] != 0U &&
           gen->class_counts[TRACE_CONTENT_HUGE_LINE] != 0U &&
           gen->sequence_splits != 0U;
}

static int report_failure(Trace *trace, TraceFailure observed)
{
    TraceFailure first;
    ProbeCtx context;
    FailurePred pred;
    Bytebuf text;
    Bytebuf snippet;
    char path[256];
    u32 replays = 0U;

    (void)observed;
    (void)memset(&first, 0, sizeof(first));
    if (replay_trace(trace, true, &first, NULL)) {
        (void)fprintf(stderr,
                      "fuzz_textbuf: tiered failure did not reproduce\n");
        return 1;
    }
    context.trace = trace;
    context.full_checks = true;
    (void)memset(&pred, 0, sizeof(pred));
    pred.probe = failure_probe;
    pred.context = &context;
    pred.target = first;
    pred.replays_out = &replays;
    if (!shrink(trace, pred)) {
        (void)fprintf(stderr,
                      "fuzz_textbuf: shrink lost check=%u op=%llu\n",
                      first.check_id, (unsigned long long)first.first_op);
        return 1;
    }
    bytebuf_init(&text);
    bytebuf_init(&snippet);
    trace_write(trace, &text);
    trace_write_c_snippet(trace, &snippet);
    if (mkdir("tests/fuzz/crashes", 0755) != 0 && errno != EEXIST)
        (void)fprintf(stderr,
                      "fuzz_textbuf: cannot create crash directory: %s\n",
                      strerror(errno));
    (void)snprintf(path, sizeof(path),
                   "tests/fuzz/crashes/%016llx-%llu.trace",
                   (unsigned long long)trace->seed,
                   (unsigned long long)first.first_op);
    if (!write_file(path, text.data, text.len))
        (void)fprintf(stderr, "fuzz_textbuf: cannot write %s\n", path);
    (void)fprintf(stderr,
                  "fuzz_textbuf: divergence seed=0x%016llx mix=%s "
                  "op=%llu check=%u minimized_ops=%zu replays=%u trace=%s\n",
                  (unsigned long long)trace->seed, trace->mix,
                  (unsigned long long)first.first_op, first.check_id,
                  trace->len, replays, path);
    if (snippet.len != 0U)
        (void)fwrite(snippet.data, 1U, snippet.len, stderr);
    bytebuf_free(&snippet);
    bytebuf_free(&text);
    return 1;
}

static int run_generated(u64 seed, Mix mix, size_t iterations,
                         u64 seconds, const char *trace_out)
{
    Generator gen;
    Replay run;
    TraceFailure failure;
    Bytebuf trace_bytes;
    Bytebuf final_bytes;
    u64 deadline = seconds == 0U
                       ? UINT64_MAX
                       : monotonic_ns() + seconds * UINT64_C(1000000000);
    u64 final_hash;
    size_t i;
    int result = 1;

    (void)memset(&gen, 0, sizeof(gen));
    trace_init(&gen.trace);
    gen.rng = seed == 0U ? UINT64_C(0x9e3779b97f4a7c15) : seed;
    gen.mix = mix;
    gen.iterations = iterations;
    gen.trace.seed = seed;
    (void)snprintf(gen.trace.mix, sizeof(gen.trace.mix), "%s",
                   mix_name(mix));
    if (!replay_init(&run, &gen.trace)) {
        trace_free(&gen.trace);
        return 2;
    }
    (void)memset(&failure, 0, sizeof(failure));
    for (i = 0U; i < iterations ||
                 (seconds != 0U && monotonic_ns() < deadline); i++) {
        TraceOp *op;

        if (!generate_op(&gen, &run, i)) {
            (void)fprintf(stderr, "fuzz_textbuf: trace allocation failed\n");
            goto done;
        }
        op = &gen.trace.ops[gen.trace.len - 1U];
        if (!apply_trace_op(&run, op, &failure)) {
            (void)fail_at(&failure, op, CHECK_OPERATION);
            replay_dispose(&run);
            result = report_failure(&gen.trace, failure);
            trace_free(&gen.trace);
            return result;
        }
        if (!check_state(&run, op, i, false, &failure)) {
            replay_dispose(&run);
            result = report_failure(&gen.trace, failure);
            trace_free(&gen.trace);
            return result;
        }
        if (seconds != 0U && i + 1U >= iterations &&
            monotonic_ns() >= deadline)
            break;
    }
    if (gen.trace.len == 0U ||
        !check_state(&run, &gen.trace.ops[gen.trace.len - 1U],
                     gen.trace.len - 1U, true, &failure)) {
        replay_dispose(&run);
        result = report_failure(&gen.trace, failure);
        trace_free(&gen.trace);
        return result;
    }
    if (!class_coverage(&gen)) {
        (void)fprintf(stderr, "fuzz_textbuf: required content class missed\n");
        goto done;
    }
    bytebuf_init(&trace_bytes);
    bytebuf_init(&final_bytes);
    trace_write(&gen.trace, &trace_bytes);
    oracle_materialize(&run.oracle, &final_bytes);
    gen.trace_hash = fnv_bytes(trace_bytes.data, trace_bytes.len);
    final_hash = fnv_bytes(final_bytes.data, final_bytes.len);
    final_hash ^= sag_undo_current(run.undo);
    final_hash *= UINT64_C(1099511628211);
    if (trace_out != NULL &&
        !write_file(trace_out, trace_bytes.data, trace_bytes.len)) {
        (void)fprintf(stderr, "fuzz_textbuf: cannot write trace %s\n",
                      trace_out);
        bytebuf_free(&final_bytes);
        bytebuf_free(&trace_bytes);
        goto done;
    }
    (void)printf(
        "fuzz_textbuf seed=%016llx mix=%s ops=%zu trace=%016llx "
        "final=%016llx bytes=%llu pieces=%u invalid=%zu nul=%zu crlf=%zu "
        "split=%zu huge=%zu\n",
        (unsigned long long)seed, mix_name(mix), gen.trace.len,
        (unsigned long long)gen.trace_hash, (unsigned long long)final_hash,
        (unsigned long long)sag_textbuf_len(run.tb),
        sag_textbuf_piece_count(run.tb),
        gen.class_counts[TRACE_CONTENT_INVALID],
        gen.class_counts[TRACE_CONTENT_BINARY],
        gen.class_counts[TRACE_CONTENT_CRLF], gen.sequence_splits,
        gen.class_counts[TRACE_CONTENT_HUGE_LINE]);
    bytebuf_free(&final_bytes);
    bytebuf_free(&trace_bytes);
    result = 0;

done:
    replay_dispose(&run);
    trace_free(&gen.trace);
    return result;
}

static int replay_file(const char *path)
{
    Bytebuf text;
    Trace trace;
    TraceFailure failure;
    char why[256];
    u64 hash;
    int result = 1;

    bytebuf_init(&text);
    trace_init(&trace);
    if (!read_file_bytes(path, &text)) {
        (void)fprintf(stderr, "fuzz_textbuf: cannot read %s\n", path);
        goto done;
    }
    if (!trace_parse(&trace, text.data, text.len, why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_textbuf: %s: %s\n", path, why);
        goto done;
    }
    (void)memset(&failure, 0, sizeof(failure));
    if (!replay_trace(&trace, true, &failure, &hash)) {
        (void)fprintf(stderr,
                      "fuzz_textbuf: replay divergence op=%llu check=%u\n",
                      (unsigned long long)failure.first_op,
                      failure.check_id);
        goto done;
    }
    (void)printf("fuzz_textbuf replay=%s seed=%016llx mix=%s ops=%zu "
                 "final=%016llx\n",
                 path, (unsigned long long)trace.seed, trace.mix, trace.len,
                 (unsigned long long)hash);
    result = 0;

done:
    trace_free(&trace);
    bytebuf_free(&text);
    return result;
}

static void usage(const char *program)
{
    (void)fprintf(stderr,
                  "usage: %s [--iters=N] [--seconds=N] [--seed=N] "
                  "[--mix=typing|paste|undo|lines] [--trace-out=PATH]\n"
                  "       %s --replay PATH\n",
                  program, program);
}

int main(int argc, char **argv)
{
    u64 seed = UINT64_C(1);
    u64 iterations_u64 = SAG_TEXT_FUZZ_DEFAULT_ITERS;
    u64 seconds = 0U;
    Mix mix = MIX_TYPING;
    const char *trace_out = NULL;
    const char *replay = NULL;
    const char *env;
    size_t i;

    env = getenv("SAG_FUZZ_SEED");
    if (env != NULL && !parse_u64_arg(env, &seed)) {
        usage(argv[0]);
        return 2;
    }
    env = getenv("SAG_FUZZ_MIX");
    if (env != NULL && !parse_mix(env, &mix)) {
        usage(argv[0]);
        return 2;
    }
    for (i = 1U; i < (size_t)argc; i++) {
        if (strncmp(argv[i], "--iters=", 8U) == 0 &&
            parse_u64_arg(argv[i] + 8U, &iterations_u64))
            continue;
        if (strncmp(argv[i], "--seconds=", 10U) == 0 &&
            parse_u64_arg(argv[i] + 10U, &seconds))
            continue;
        if (strncmp(argv[i], "--seed=", 7U) == 0 &&
            parse_u64_arg(argv[i] + 7U, &seed))
            continue;
        if (strncmp(argv[i], "--mix=", 6U) == 0 &&
            parse_mix(argv[i] + 6U, &mix))
            continue;
        if (strncmp(argv[i], "--trace-out=", 12U) == 0) {
            trace_out = argv[i] + 12U;
            continue;
        }
        if (strcmp(argv[i], "--replay") == 0 && i + 1U < (size_t)argc) {
            replay = argv[++i];
            continue;
        }
        usage(argv[0]);
        return 2;
    }
    if (replay != NULL)
        return replay_file(replay);
    if (iterations_u64 > SIZE_MAX || iterations_u64 < 6U ||
        seconds > UINT64_MAX / UINT64_C(1000000000)) {
        usage(argv[0]);
        return 2;
    }
    return run_generated(seed, mix, (size_t)iterations_u64, seconds,
                         trace_out);
}
