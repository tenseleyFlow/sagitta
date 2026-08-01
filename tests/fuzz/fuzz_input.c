#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term/input.h"
#include "unicode/utf8.h"

typedef struct {
    Key *event;
    size_t len;
    size_t cap;
    u8 *paste;
    size_t paste_len;
    size_t paste_cap;
    size_t *paste_chunk;
    size_t paste_chunks;
    size_t paste_chunk_cap;
    u32 dropped;
} Trace;

static bool fail(char *why, size_t cap, const char *message, size_t off)
{
    (void)snprintf(why, cap, "%s at byte %zu", message, off);
    return false;
}

static void *grow(void *ptr, size_t count, size_t width)
{
    void *next;

    if (count != 0U && width > SIZE_MAX / count)
        return NULL;
    next = realloc(ptr, count * width);
    return next;
}

static void trace_free(Trace *trace)
{
    free(trace->event);
    free(trace->paste);
    free(trace->paste_chunk);
}

static bool trace_event(Trace *trace, const Key *key, const In *in,
                        char *why, size_t why_cap, size_t off)
{
    if (key->kind == SAG_EV_PASTE_DATA) {
        size_t chunk_len;
        const u8 *chunk = sag_input_paste_chunk(in, &chunk_len);

        if (chunk_len > SAG_IN_PASTE_CHUNK)
            return fail(why, why_cap, "paste chunk exceeded 4096", off);
        if (trace->paste_len + chunk_len > trace->paste_cap) {
            size_t cap = trace->paste_cap == 0U ? 64U : trace->paste_cap;

            while (cap < trace->paste_len + chunk_len) {
                if (cap > SIZE_MAX / 2U)
                    return fail(why, why_cap, "paste trace overflow", off);
                cap *= 2U;
            }
            trace->paste = grow(trace->paste, cap, 1U);
            if (trace->paste == NULL)
                return fail(why, why_cap, "paste trace allocation", off);
            trace->paste_cap = cap;
        }
        if (chunk_len != 0U)
            (void)memcpy(trace->paste + trace->paste_len, chunk, chunk_len);
        trace->paste_len += chunk_len;
        if (trace->paste_chunks == trace->paste_chunk_cap) {
            size_t cap = trace->paste_chunk_cap == 0U
                             ? 4U : trace->paste_chunk_cap * 2U;

            trace->paste_chunk = grow(trace->paste_chunk, cap,
                                      sizeof(*trace->paste_chunk));
            if (trace->paste_chunk == NULL)
                return fail(why, why_cap, "paste boundary allocation", off);
            trace->paste_chunk_cap = cap;
        }
        trace->paste_chunk[trace->paste_chunks++] = chunk_len;
    }
    if (trace->len == trace->cap) {
        size_t cap = trace->cap == 0U ? 16U : trace->cap * 2U;

        trace->event = grow(trace->event, cap, sizeof(*trace->event));
        if (trace->event == NULL)
            return fail(why, why_cap, "event trace allocation", off);
        trace->cap = cap;
    }
    trace->event[trace->len++] = *key;
    return true;
}

static bool code_is_defined(u32 code)
{
    return (code <= 0x10FFFFU &&
            !(code >= 0xD800U && code <= 0xDFFFU)) ||
           (code >= SAG_KEY_ESCAPE && code <= SAG_KEY_RIGHT_META);
}

static bool event_is_valid(const Key *key, bool was_paste,
                           char *why, size_t why_cap, size_t off)
{
    /* Property 3: associated text is bounded and valid UTF-8. */
    if (key->ntext > 15U ||
        sag_utf8_validate(key->text, key->ntext) != key->ntext)
        return fail(why, why_cap, "invalid associated text", off);
    /* Property 4: key code is Unicode or one of the append-only names. */
    if (!code_is_defined(key->code))
        return fail(why, why_cap, "undefined key code", off);
    /* Property 5: event kind and mouse fields are initialized/bounded. */
    if (key->kind > SAG_EV_FOCUS || key->button > SAG_MB_FORWARD)
        return fail(why, why_cap, "undefined event kind or mouse button", off);
    if (key->kind != SAG_EV_MOUSE &&
        (key->col != 0U || key->row != 0U || key->button != SAG_MB_NONE))
        return fail(why, why_cap, "mouse fields leaked into another event", off);
    if (key->kind == SAG_EV_MOUSE &&
        (key->code != 0U || key->ntext != 0U ||
         key->ev < SAG_KEY_PRESS || key->ev > SAG_KEY_RELEASE))
        return fail(why, why_cap, "malformed normalized mouse event", off);
    /* Property 7: paste state can never emit a key event. */
    if (was_paste && key->kind == SAG_EV_KEY)
        return fail(why, why_cap, "key emitted from paste state", off);
    return true;
}

static bool drain(In *in, i64 now, size_t fed, Trace *trace,
                  size_t *iterations, size_t *events,
                  char *why, size_t why_cap)
{
    Key key;

    for (;;) {
        size_t before = in->buf.len - in->rd;
        bool was_paste = in->in_paste;
        u8 before_state = in->state;
        u32 before_dropped = in->dropped;
        bool produced = sag_input_next(in, now, &key);
        size_t after = in->buf.len - in->rd;
        bool terminal_transition =
            in->eof &&
            (in->state != before_state || in->in_paste != was_paste ||
             in->dropped != before_dropped);

        if (!produced)
            return true;
        (*iterations)++;
        /*
         * Property 1: every true return consumes input. The only exception
         * is a one-shot EOF transition that changes parser state.
         */
        if (after >= before && !terminal_transition)
            return fail(why, why_cap, "true return made no progress", fed);
        /* Property 2: N bytes cannot yield more than N events/steps. */
        if (*iterations > fed)
            return fail(why, why_cap, "more parser steps than input bytes", fed);
        if (key.kind != SAG_EV_NONE)
            (*events)++;
        if (*events > fed)
            return fail(why, why_cap, "more events than input bytes", fed);
        if (!event_is_valid(&key, was_paste, why, why_cap, fed))
            return false;
        if (!trace_event(trace, &key, in, why, why_cap, fed))
            return false;
        /* Property 6: allocations remain bounded by fed input plus max buf. */
        if (in->buf.cap + in->paste.cap > fed + SAG_IN_MAX_BUFFER)
            return fail(why, why_cap, "input allocation bound exceeded", fed);
    }
}

static bool parse_partition(const u8 *data, size_t len, size_t split,
                            bool dribble, Trace *trace,
                            char *why, size_t why_cap)
{
    TtyCaps caps = {0};
    In in;
    size_t iterations = 0U;
    size_t events = 0U;
    size_t fed = 0U;
    size_t pos;

    /* Exercise both ESC policies without consulting any clock. */
    caps.kitty_kbd = len != 0U && (data[0] & 1U) != 0U;
    sag_input_init(&in, &caps);
    if (dribble) {
        for (pos = 0U; pos < len; pos++) {
            sag_input_feed(&in, data + pos, 1U);
            fed++;
            if (!drain(&in, (i64)pos, fed, trace, &iterations, &events,
                       why, why_cap)) {
                sag_input_free(&in);
                return false;
            }
        }
    } else {
        sag_input_feed(&in, data, split);
        fed = split;
        if (!drain(&in, 0, fed, trace, &iterations, &events,
                   why, why_cap)) {
            sag_input_free(&in);
            return false;
        }
        sag_input_feed(&in, data + split, len - split);
        fed = len;
        if (!drain(&in, 1, fed, trace, &iterations, &events,
                   why, why_cap)) {
            sag_input_free(&in);
            return false;
        }
    }
    sag_input_eof(&in);
    if (!drain(&in, INT64_MAX / 2, fed, trace, &iterations, &events,
               why, why_cap)) {
        sag_input_free(&in);
        return false;
    }
    trace->dropped = in.dropped;
    sag_input_free(&in);
    return true;
}

static bool traces_equal(const Trace *a, const Trace *b)
{
    return a->len == b->len && a->paste_len == b->paste_len &&
           a->paste_chunks == b->paste_chunks &&
           a->dropped == b->dropped &&
           (a->len == 0U || memcmp(a->event, b->event,
                                   a->len * sizeof(*a->event)) == 0) &&
           (a->paste_len == 0U ||
            memcmp(a->paste, b->paste, a->paste_len) == 0) &&
           (a->paste_chunks == 0U ||
            memcmp(a->paste_chunk, b->paste_chunk,
                   a->paste_chunks * sizeof(*a->paste_chunk)) == 0);
}

static bool compare_partition(const u8 *data, size_t len,
                              const Trace *baseline, size_t split,
                              bool dribble, char *why, size_t why_cap)
{
    Trace actual = {0};
    bool ok = parse_partition(data, len, split, dribble, &actual,
                              why, why_cap);

    /* Property 8: feeding partition cannot change normalized semantics. */
    if (ok && !traces_equal(baseline, &actual))
        ok = fail(why, why_cap, "split changed normalized trace", split);
    trace_free(&actual);
    return ok;
}

static bool check_input(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    Trace baseline = {0};
    size_t split;
    bool ok;

    ok = parse_partition(data, len, len, false, &baseline, why, why_cap);
    if (!ok) {
        trace_free(&baseline);
        return false;
    }
    if (len <= 64U) {
        for (split = 0U; split <= len; split++) {
            if (!compare_partition(data, len, &baseline, split, false,
                                   why, why_cap)) {
                trace_free(&baseline);
                return false;
            }
        }
    } else {
        split = len == 0U ? 0U : (size_t)data[0] * len / 255U;
        if (!compare_partition(data, len, &baseline, split, false,
                               why, why_cap)) {
            trace_free(&baseline);
            return false;
        }
    }
    ok = compare_partition(data, len, &baseline, 0U, true, why, why_cap);
    trace_free(&baseline);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_input",
                         "tests/unit/fixtures/input", check_input);
}
