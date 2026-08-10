#define _POSIX_C_SOURCE 200809L

#include "shrink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log.h"

enum {
    DEFAULT_REPLAYS = 5000U
};

static const u64 DEFAULT_NS = UINT64_C(10000000000);

static void op_init(TraceOp *op, TraceOpKind kind, u64 a, u64 b,
                    u64 ordinal, const u8 *payload, size_t payload_len)
{
    op->kind = kind;
    op->a = a;
    op->b = b;
    op->ordinal = ordinal;
    op->content_class = TRACE_CONTENT_ASCII;
    bytebuf_init(&op->payload);
    bytebuf_append(&op->payload, payload, payload_len);
}

static void op_free(TraceOp *op)
{
    bytebuf_free(&op->payload);
}

static TraceOp op_copy(const TraceOp *source)
{
    TraceOp copy;

    op_init(&copy, source->kind, source->a, source->b, source->ordinal,
            source->payload.data, source->payload.len);
    copy.content_class = source->content_class;
    return copy;
}

void trace_init(Trace *trace)
{
    memset(trace, 0, sizeof(*trace));
    (void)snprintf(trace->mix, sizeof(trace->mix), "%s", "default");
    (void)snprintf(trace->base, sizeof(trace->base), "%s", "empty");
}

void trace_free(Trace *trace)
{
    size_t i;

    if (trace == NULL)
        return;
    for (i = 0U; i < trace->len; i++)
        op_free(&trace->ops[i]);
    free(trace->ops);
    trace_init(trace);
}

static void trace_reserve(Trace *trace, size_t need)
{
    size_t cap;

    if (trace->cap >= need)
        return;
    cap = trace->cap ? trace->cap : 64U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    trace->ops = yew_xreallocarray(trace->ops, cap, sizeof(*trace->ops));
    trace->cap = cap;
}

bool trace_push(Trace *trace, TraceOpKind kind, u64 a, u64 b,
                const u8 *payload, size_t payload_len)
{
    if (trace == NULL || trace->len == SIZE_MAX ||
        (payload_len != 0U && payload == NULL) ||
        (u64)trace->len != trace->len)
        return false;
    trace_reserve(trace, trace->len + 1U);
    op_init(&trace->ops[trace->len], kind, a, b, (u64)trace->len,
            payload, payload_len);
    trace->len++;
    return true;
}

static void set_why(char *why, size_t cap, size_t line, const char *message)
{
    if (why != NULL && cap != 0U)
        (void)snprintf(why, cap, "line %zu: %s", line, message);
}

static bool parse_u64(const char *text, u64 *value)
{
    char *end;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-')
        return false;
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || *end != '\0')
        return false;
    *value = (u64)parsed;
    return true;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static bool parse_hex(const char *hex, u64 declared, Bytebuf *payload)
{
    size_t len = strlen(hex);
    size_t i;

    if (declared > (u64)(SIZE_MAX / 2U) ||
        len != (size_t)declared * 2U)
        return false;
    for (i = 0U; i < len; i += 2U) {
        int hi = hex_digit(hex[i]);
        int lo = hex_digit(hex[i + 1U]);
        if (hi < 0 || lo < 0)
            return false;
        bytebuf_push_u8(payload, (u8)((hi << 4) | lo));
    }
    return true;
}

static bool one_arg_kind(const char *name, TraceOpKind *kind)
{
    static const struct {
        const char *name;
        TraceOpKind kind;
    } table[] = {
        {"line_start", TRACE_LINE_START}, {"line_of", TRACE_LINE_OF},
        {"line_span", TRACE_LINE_SPAN},   {"iter", TRACE_ITER},
        {"snap", TRACE_SNAP},             {"release", TRACE_RELEASE},
        {"undo_to", TRACE_UNDO_TO}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(table); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *kind = table[i].kind;
            return true;
        }
    }
    return false;
}

static bool no_arg_kind(const char *name, TraceOpKind *kind)
{
    static const struct {
        const char *name;
        TraceOpKind kind;
    } table[] = {
        {"undo", TRACE_UNDO}, {"redo", TRACE_REDO},
        {"undo_boundary", TRACE_UNDO_BOUNDARY}, {"save", TRACE_SAVE},
        {"check", TRACE_CHECK}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(table); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *kind = table[i].kind;
            return true;
        }
    }
    return false;
}

static bool parse_header(Trace *trace, const char *line)
{
    char seed[32];
    char mix[sizeof(trace->mix)];
    char base[sizeof(trace->base)];
    u64 value;

    if (sscanf(line, "# seed=%31s mix=%31s base=%255s", seed, mix, base) != 3)
        return false;
    if (!parse_u64(seed, &value))
        return false;
    trace->seed = value;
    (void)snprintf(trace->mix, sizeof(trace->mix), "%s", mix);
    (void)snprintf(trace->base, sizeof(trace->base), "%s", base);
    return true;
}

static bool parse_op(Trace *trace, char *line)
{
    char *save = NULL;
    char *name = strtok_r(line, " \t", &save);
    char *first = strtok_r(NULL, " \t", &save);
    char *second = strtok_r(NULL, " \t", &save);
    char *third = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    TraceOpKind kind;
    u64 a = 0U;
    u64 b = 0U;
    Bytebuf payload;
    bool ok = false;

    bytebuf_init(&payload);
    if (name == NULL)
        goto done;
    if (strcmp(name, "ins") == 0) {
        if (first == NULL || second == NULL || third == NULL || extra != NULL ||
            !parse_u64(first, &a) || !parse_u64(second, &b) ||
            !parse_hex(third, b, &payload))
            goto done;
        ok = trace_push(trace, TRACE_INS, a, b, payload.data, payload.len);
    } else if (strcmp(name, "del") == 0) {
        if (first == NULL || second == NULL || third != NULL ||
            !parse_u64(first, &a) || !parse_u64(second, &b))
            goto done;
        ok = trace_push(trace, TRACE_DEL, a, b, NULL, 0U);
    } else if (one_arg_kind(name, &kind)) {
        if (first == NULL || second != NULL || !parse_u64(first, &a))
            goto done;
        ok = trace_push(trace, kind, a, 0U, NULL, 0U);
    } else if (no_arg_kind(name, &kind)) {
        if (first != NULL)
            goto done;
        ok = trace_push(trace, kind, 0U, 0U, NULL, 0U);
    }
done:
    bytebuf_free(&payload);
    return ok;
}

bool trace_parse(Trace *trace, const u8 *text, size_t len,
                 char *why, size_t why_cap)
{
    char *copy;
    char *cursor;
    size_t line_no = 0U;
    bool saw_magic = false;

    if (trace == NULL || (len != 0U && text == NULL))
        return false;
    trace_free(trace);
    if (len == SIZE_MAX) {
        set_why(why, why_cap, 1U, "trace too large");
        return false;
    }
    copy = yew_xmalloc(len + 1U);
    memcpy(copy, text, len);
    copy[len] = '\0';
    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        size_t line_len;
        line_no++;
        if (nl != NULL) {
            *nl = '\0';
            cursor = nl + 1U;
        } else {
            cursor = NULL;
        }
        line_len = strlen(line);
        if (line_len != 0U && line[line_len - 1U] == '\r')
            line[--line_len] = '\0';
        if (line_len == 0U)
            continue;
        if (strcmp(line, "# yew textbuf trace v1") == 0) {
            saw_magic = true;
            continue;
        }
        if (strncmp(line, "# seed=", 7U) == 0) {
            if (!parse_header(trace, line)) {
                set_why(why, why_cap, line_no, "invalid metadata");
                free(copy);
                trace_free(trace);
                return false;
            }
            continue;
        }
        if (line[0] == '#')
            continue;
        if (!parse_op(trace, line)) {
            set_why(why, why_cap, line_no, "invalid operation");
            free(copy);
            trace_free(trace);
            return false;
        }
    }
    free(copy);
    if (!saw_magic) {
        set_why(why, why_cap, 1U, "missing trace v1 header");
        trace_free(trace);
        return false;
    }
    return true;
}

static const char *op_name(TraceOpKind kind)
{
    static const char *const names[] = {
        "ins", "del", "line_start", "line_of", "line_span", "iter",
        "snap", "release", "undo", "redo", "undo_boundary", "undo_to",
        "save", "check"
    };

    if ((size_t)kind >= YEW_ARRAY_LEN(names))
        YEW_BUG("trace_write: invalid operation kind");
    return names[(size_t)kind];
}

void trace_write(const Trace *trace, Bytebuf *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (trace == NULL || out == NULL)
        YEW_BUG("trace_write: invalid argument");
    bytebuf_printf(out, "# yew textbuf trace v1\n");
    bytebuf_printf(out, "# seed=0x%016llx mix=%s base=%s\n",
                   (unsigned long long)trace->seed, trace->mix, trace->base);
    for (i = 0U; i < trace->len; i++) {
        const TraceOp *op = &trace->ops[i];
        size_t j;
        if (op->kind == TRACE_INS) {
            bytebuf_printf(out, "ins %llu %zu ",
                           (unsigned long long)op->a, op->payload.len);
            for (j = 0U; j < op->payload.len; j++) {
                bytebuf_push_u8(out, (u8)hex[op->payload.data[j] >> 4]);
                bytebuf_push_u8(out, (u8)hex[op->payload.data[j] & 15U]);
            }
            bytebuf_push_u8(out, (u8)'\n');
        } else if (op->kind == TRACE_DEL) {
            bytebuf_printf(out, "del %llu %llu\n",
                           (unsigned long long)op->a,
                           (unsigned long long)op->b);
        } else if (op->kind == TRACE_LINE_START || op->kind == TRACE_LINE_OF ||
                   op->kind == TRACE_LINE_SPAN || op->kind == TRACE_ITER ||
                   op->kind == TRACE_SNAP || op->kind == TRACE_RELEASE ||
                   op->kind == TRACE_UNDO_TO) {
            bytebuf_printf(out, "%s %llu\n", op_name(op->kind),
                           (unsigned long long)op->a);
        } else {
            bytebuf_printf(out, "%s\n", op_name(op->kind));
        }
    }
}

void trace_write_c_snippet(const Trace *trace, Bytebuf *out)
{
    size_t i;

    if (trace == NULL || out == NULL)
        YEW_BUG("trace_write_c_snippet: invalid argument");
    bytebuf_printf(out, "TextBuf *tb = yew_textbuf_from_bytes(NULL, 0U);\n");
    for (i = 0U; i < trace->len; i++) {
        const TraceOp *op = &trace->ops[i];
        size_t j;

        if (op->kind == TRACE_INS) {
            bytebuf_printf(out, "yew_textbuf_insert(tb, BYTEOFF(%lluU), ",
                           (unsigned long long)op->a);
            if (op->payload.len == 0U) {
                bytebuf_printf(out, "NULL, 0U);\n");
                continue;
            }
            bytebuf_printf(out, "(const u8[]){");
            for (j = 0U; j < op->payload.len; j++)
                bytebuf_printf(out, "%s0x%02xU", j == 0U ? "" : ", ",
                               (unsigned int)op->payload.data[j]);
            bytebuf_printf(out, "}, %zuU);\n", op->payload.len);
        } else if (op->kind == TRACE_DEL) {
            bytebuf_printf(out,
                           "yew_textbuf_delete(tb, (Span){%lluU, %lluU});\n",
                           (unsigned long long)op->a,
                           (unsigned long long)op->b);
        } else {
            bytebuf_printf(out, "/* trace op: %s %llu */\n", op_name(op->kind),
                           (unsigned long long)op->a);
        }
    }
    bytebuf_printf(out, "yew_textbuf_check(tb);\nyew_textbuf_free(tb);\n");
}

bool trace_failure_equal(const TraceFailure *a, const TraceFailure *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == TRACE_FAILURE_CHECK)
        return a->first_op == b->first_op && a->check_id == b->check_id;
    return strcmp(a->assertion, b->assertion) == 0;
}

typedef struct {
    FailurePred pred;
    u32 replays;
    u32 max_replays;
    u64 deadline;
} ShrinkRun;

static u64 monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("shrinker: CLOCK_MONOTONIC failed");
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static bool budget_left(const ShrinkRun *run)
{
    return run->replays < run->max_replays && monotonic_ns() < run->deadline;
}

static bool reproduces(ShrinkRun *run, const Trace *trace)
{
    TraceFailure actual;

    if (!budget_left(run))
        return false;
    memset(&actual, 0, sizeof(actual));
    run->replays++;
    return run->pred.probe(trace, &actual, run->pred.context) &&
           trace_failure_equal(&actual, &run->pred.target);
}

static Trace trace_without(const Trace *source, size_t first, size_t count)
{
    Trace out;
    size_t i;

    trace_init(&out);
    out.seed = source->seed;
    (void)snprintf(out.mix, sizeof(out.mix), "%s", source->mix);
    (void)snprintf(out.base, sizeof(out.base), "%s", source->base);
    trace_reserve(&out, source->len - count);
    for (i = 0U; i < source->len; i++) {
        if (i < first || i >= first + count)
            out.ops[out.len++] = op_copy(&source->ops[i]);
    }
    return out;
}

static bool trace_start_len(const Trace *trace, u64 *len)
{
    const char *number;
    const char *colon;
    char copy[32];
    size_t count;

    if (strcmp(trace->base, "empty") == 0) {
        *len = 0U;
        return true;
    }
    if (strncmp(trace->base, "gen:", 4U) != 0)
        return false;
    number = trace->base + 4U;
    colon = strchr(number, ':');
    if (colon == NULL)
        return false;
    count = (size_t)(colon - number);
    if (count == 0U || count >= sizeof(copy))
        return false;
    memcpy(copy, number, count);
    copy[count] = '\0';
    return parse_u64(copy, len);
}

static bool trace_clamp_offsets(Trace *trace)
{
    u64 len;
    size_t i;
    bool changed = false;

    if (!trace_start_len(trace, &len))
        return false;
    for (i = 0U; i < trace->len; i++) {
        TraceOp *op = &trace->ops[i];
        if (op->kind == TRACE_INS) {
            if (op->a > len) {
                op->a = len;
                changed = true;
            }
            if ((u64)op->payload.len > UINT64_MAX - len)
                return changed;
            len += (u64)op->payload.len;
        } else if (op->kind == TRACE_DEL) {
            if (op->a > len) {
                op->a = len;
                changed = true;
            }
            if (op->b > len) {
                op->b = len;
                changed = true;
            }
            if (op->b < op->a) {
                op->b = op->a;
                changed = true;
            }
            len -= op->b - op->a;
        } else if (op->kind == TRACE_LINE_OF || op->kind == TRACE_ITER) {
            if (op->a > len) {
                op->a = len;
                changed = true;
            }
        } else if (op->kind == TRACE_UNDO || op->kind == TRACE_REDO ||
                   op->kind == TRACE_UNDO_TO) {
            /* Undo changes length according to history that this deliberately
               simple trace layer does not replay.  The subject replayer owns
               clamping after this point. */
            return changed;
        }
    }
    return changed;
}

static bool try_remove(Trace *trace, size_t first, size_t count, ShrinkRun *run)
{
    Trace candidate;
    Trace unclamped;
    bool clamped;

    if (count == 0U || count > trace->len - first)
        return false;
    candidate = trace_without(trace, first, count);
    unclamped = trace_without(trace, first, count);
    clamped = trace_clamp_offsets(&candidate);
    if (reproduces(run, &candidate)) {
        trace_free(trace);
        *trace = candidate;
        trace_free(&unclamped);
        return true;
    }
    trace_free(&candidate);
    if (clamped && reproduces(run, &unclamped)) {
        trace_free(trace);
        *trace = unclamped;
        return true;
    }
    trace_free(&unclamped);
    return false;
}

static bool ddmin(Trace *trace, ShrinkRun *run)
{
    bool changed = false;
    size_t divisions = 2U;

    while (trace->len > 1U && budget_left(run)) {
        size_t chunk = (trace->len + divisions - 1U) / divisions;
        size_t start;
        bool accepted = false;
        for (start = 0U; start < trace->len && budget_left(run);
             start += chunk) {
            size_t count = chunk;
            if (count > trace->len - start)
                count = trace->len - start;
            if (try_remove(trace, start, count, run)) {
                changed = true;
                accepted = true;
                divisions = 2U;
                break;
            }
        }
        if (!accepted) {
            if (divisions >= trace->len)
                break;
            divisions *= 2U;
            if (divisions > trace->len)
                divisions = trace->len;
        }
    }
    return changed;
}

static bool greedy_remove(Trace *trace, ShrinkRun *run)
{
    size_t i = 0U;
    bool changed = false;

    while (i < trace->len && budget_left(run)) {
        if (try_remove(trace, i, 1U, run))
            changed = true;
        else
            i++;
    }
    return changed;
}

static bool try_op(Trace *trace, size_t index, const TraceOp *replacement,
                   ShrinkRun *run)
{
    TraceOp old = trace->ops[index];

    trace->ops[index] = op_copy(replacement);
    if (reproduces(run, trace)) {
        op_free(&old);
        return true;
    }
    op_free(&trace->ops[index]);
    trace->ops[index] = old;
    return false;
}

static bool simplify_op(Trace *trace, size_t index, ShrinkRun *run)
{
    TraceOp *op = &trace->ops[index];
    TraceOp candidate;
    bool changed = false;
    size_t i;

    if (op->kind == TRACE_INS && op->payload.len != 0U) {
        candidate = op_copy(op);
        for (i = 0U; i < candidate.payload.len; i++)
            candidate.payload.data[i] = (u8)'a';
        candidate.content_class = TRACE_CONTENT_ASCII;
        if (memcmp(candidate.payload.data, op->payload.data,
                   op->payload.len) != 0 &&
            try_op(trace, index, &candidate, run))
            changed = true;
        op_free(&candidate);
        op = &trace->ops[index];
        if (op->payload.len > 1U) {
            candidate = op_copy(op);
            candidate.payload.len = 1U;
            candidate.b = 1U;
            if (try_op(trace, index, &candidate, run))
                changed = true;
            op_free(&candidate);
        }
    }
    op = &trace->ops[index];
    if (op->a != 0U) {
        u64 lo = 0U;
        u64 hi = op->a;
        while (lo < hi && budget_left(run)) {
            u64 mid = lo + (hi - lo) / 2U;
            candidate = op_copy(&trace->ops[index]);
            candidate.a = mid;
            if (try_op(trace, index, &candidate, run)) {
                changed = true;
                hi = mid;
            } else {
                lo = mid + 1U;
            }
            op_free(&candidate);
        }
    }
    op = &trace->ops[index];
    if (op->kind == TRACE_DEL && op->b > op->a + 1U) {
        candidate = op_copy(op);
        candidate.b = candidate.a + 1U;
        if (try_op(trace, index, &candidate, run))
            changed = true;
        op_free(&candidate);
    } else if (op->kind == TRACE_UNDO_TO) {
        candidate = op_copy(op);
        candidate.kind = TRACE_UNDO;
        candidate.a = 0U;
        if (try_op(trace, index, &candidate, run))
            changed = true;
        op_free(&candidate);
    }
    return changed;
}

static bool ops_equal(const TraceOp *a, const TraceOp *b)
{
    return a->kind == b->kind && a->a == b->a && a->b == b->b &&
           a->payload.len == b->payload.len &&
           (a->payload.len == 0U ||
            memcmp(a->payload.data, b->payload.data, a->payload.len) == 0);
}

static bool deduplicate(Trace *trace, ShrinkRun *run)
{
    size_t i = 1U;
    bool changed = false;

    while (i < trace->len && budget_left(run)) {
        if (ops_equal(&trace->ops[i - 1U], &trace->ops[i]) &&
            try_remove(trace, i, 1U, run))
            changed = true;
        else
            i++;
    }
    return changed;
}

bool shrink(Trace *trace, FailurePred pred)
{
    ShrinkRun run;
    TraceFailure final_failure;
    bool changed;
    size_t i;

    if (trace == NULL || pred.probe == NULL)
        return false;
    run.pred = pred;
    run.replays = 0U;
    run.max_replays = pred.max_replays ? pred.max_replays : DEFAULT_REPLAYS;
    run.deadline = monotonic_ns();
    if ((pred.max_ns ? pred.max_ns : DEFAULT_NS) > UINT64_MAX - run.deadline)
        run.deadline = UINT64_MAX;
    else
        run.deadline += pred.max_ns ? pred.max_ns : DEFAULT_NS;
    if (!reproduces(&run, trace))
        return false;
    do {
        changed = ddmin(trace, &run);
        changed = greedy_remove(trace, &run) || changed;
        for (i = 0U; i < trace->len && budget_left(&run); i++)
            changed = simplify_op(trace, i, &run) || changed;
        changed = deduplicate(trace, &run) || changed;
    } while (changed && budget_left(&run));
    if (pred.replays_out != NULL)
        *pred.replays_out = run.replays;
    memset(&final_failure, 0, sizeof(final_failure));
    return pred.probe(trace, &final_failure, pred.context) &&
           trace_failure_equal(&final_failure, &pred.target);
}
