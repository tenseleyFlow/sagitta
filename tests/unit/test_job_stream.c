/* Sprint 19 §3 + DoD 4: sag_job_safe_prefix must be chunking-independent.
 *
 * A pipe read may split a UTF-8 sequence AND a grapheme cluster, and
 * PIPE_BUF guarantees the reader nothing.  The property that matters is
 * that feeding a stream split at EVERY byte offset appends exactly the
 * same bytes as feeding it whole — s03's chunking-independence discipline
 * applied to pipes. */
#include "harness.h"

#include <string.h>

#include "edit/job.h"
#include "util/buf.h"

/* Every fixture is a byte string whose clusters must survive splitting. */
static const char *const stream_fixtures[] = {
    "",
    "a",
    "hello world\n",
    "line one\nline two\n",
    "\n\n\n",
    "caf\xC3\xA9",                         /* 2-byte sequence           */
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", /* CJK, 3-byte              */
    "\xF0\x9F\x98\x80",                     /* emoji, 4-byte            */
    /* ZWJ family: one cluster, eleven codepoints */
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
    "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6",
    "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8",     /* regional indicator pair  */
    "e\xCC\x81",                            /* e + combining acute      */
    "a\xCC\x80\xCC\x81\xCC\x82\xCC\x83",    /* stacked combining marks  */
    "\xEA\xB0\x81",                         /* Hangul syllable          */
    "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", /* Hangul jamo L+V+T        */
    "crlf\r\n",
    "tab\there\n",
    "\xFF",                                 /* lone invalid byte        */
    "ok\xFF" "bad\n",                          /* invalid mid-stream       */
    "\xC3",                                 /* truncated 2-byte lead    */
    "\xE6\x97",                             /* truncated 3-byte         */
    "\xF0\x9F\x98",                         /* truncated 4-byte         */
    "mix \xF0\x9F\x98\x80 \xE6\x97\xA5 e\xCC\x81 done\n",
    "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD",     /* emoji + skin tone        */
    "trailing space \n"
};

/* Drives the whole stream through safe_prefix with the given chunk size,
 * mirroring job_drain's hold-buffer discipline. */
static void feed_chunked(const u8 *src, size_t len, size_t chunk,
                         Bytebuf *out)
{
    Bytebuf hold;
    size_t at = 0U;

    bytebuf_init(&hold);
    while (at < len) {
        size_t take = len - at < chunk ? len - at : chunk;
        const u8 *view;
        u64 total;
        u64 safe;

        if (hold.len != 0U) {
            bytebuf_append(&hold, src + at, take);
            view = hold.data;
            total = (u64)hold.len;
        } else {
            view = src + at;
            total = (u64)take;
        }
        at += take;
        safe = sag_job_safe_prefix(view, total, false);
        SAG_ASSERT(safe <= total);
        bytebuf_append(out, view, (size_t)safe);
        if (safe < total) {
            u8 tail[SAG_JOB_HOLD_MAX + 32];
            u64 rest = total - safe;

            SAG_ASSERT(rest <= sizeof(tail));
            (void)memcpy(tail, view + safe, (size_t)rest);
            hold.len = 0U;
            bytebuf_append(&hold, tail, (size_t)rest);
        } else {
            hold.len = 0U;
        }
    }
    /* EOF flushes everything, invalid tail included (rule 4). */
    if (hold.len != 0U) {
        u64 safe = sag_job_safe_prefix(hold.data, (u64)hold.len, true);

        SAG_ASSERT_EQ_U64(safe, (u64)hold.len);
        bytebuf_append(out, hold.data, hold.len);
    }
    bytebuf_free(&hold);
}

void test_job_stream_split_at_every_offset(void)
{
    size_t f;

    for (f = 0U; f < SAG_ARRAY_LEN(stream_fixtures); f++) {
        const u8 *src = (const u8 *)stream_fixtures[f];
        size_t len = strlen(stream_fixtures[f]);
        size_t chunk;

        for (chunk = 1U; chunk <= len + 1U; chunk++) {
            Bytebuf got;

            bytebuf_init(&got);
            feed_chunked(src, len, chunk, &got);
            /* The whole point: chunk size must not change the output. */
            SAG_ASSERT_EQ_U64((u64)got.len, (u64)len);
            if (len != 0U)
                SAG_ASSERT_EQ_MEM(got.data, src, len);
            bytebuf_free(&got);
        }
    }
}

void test_job_stream_holds_incomplete_utf8(void)
{
    /* A 4-byte emoji arriving three bytes at a time holds all three. */
    const u8 partial[] = {0xF0U, 0x9FU, 0x98U};

    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(partial, 3U, false), 0U);
    /* Rules 1 and 2 compose, and the composition is deliberately
     * conservative: the incomplete lead is held, and so is the preceding
     * character.  We cannot yet know whether those pending bytes decode to
     * a combining mark (making "a" + mark ONE cluster, so emitting "a"
     * alone would split it) or to a standalone scalar like U+00E9.  Both
     * decodings start 0xC3, so the only safe answer before the rest
     * arrives is to hold both — which is why the split-at-every-offset
     * property above holds. */
    {
        const u8 two[] = {'a', 0xC3U};

        SAG_ASSERT_EQ_U64(sag_job_safe_prefix(two, 2U, false), 0U);
    }
    {
        const u8 three[] = {'x', 0xE6U, 0x97U};

        SAG_ASSERT_EQ_U64(sag_job_safe_prefix(three, 3U, false), 0U);
    }
    /* Holding is bounded, though: with a completed cluster followed by an
     * incomplete lead, everything before the last cluster still flows. */
    {
        const u8 mixed[] = {'a', 'b', 'c', 0xF0U};

        SAG_ASSERT_EQ_U64(sag_job_safe_prefix(mixed, 4U, false), 2U);
    }
}

void test_job_stream_newline_holds_nothing(void)
{
    /* Rule 2's fast path: a trailing newline terminates a cluster, so
     * ordinary line-oriented output streams with zero holdback. */
    const u8 line[] = "some output\n";
    u64 n = (u64)sizeof(line) - 1U;

    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(line, n, false), n);
}

void test_job_stream_eof_flushes_invalid_tail(void)
{
    const u8 bad[] = {'o', 'k', 0xFFU, 0xC3U};

    /* Not at EOF: the dangling 0xC3 lead is held. */
    SAG_ASSERT(sag_job_safe_prefix(bad, 4U, false) < 4U);
    /* At EOF everything goes, byte-exact — storage is verbatim. */
    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(bad, 4U, true), 4U);
}

void test_job_stream_hold_cap_flushes(void)
{
    /* A pathological run of combining marks never completes a cluster;
     * past SAG_JOB_HOLD_MAX we must flush rather than grow forever. */
    u8 marks[SAG_JOB_HOLD_MAX * 2U];
    size_t i;

    for (i = 0U; i + 1U < sizeof(marks); i += 2U) {
        marks[i] = 0xCCU; /* U+0300 combining grave */
        marks[i + 1U] = 0x80U;
    }
    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(marks, (u64)sizeof(marks), false),
                      (u64)sizeof(marks));
}

void test_job_stream_empty_and_null(void)
{
    const u8 any[] = {'a'};

    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(NULL, 4U, false), 0U);
    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(any, 0U, false), 0U);
    SAG_ASSERT_EQ_U64(sag_job_safe_prefix(any, 0U, true), 0U);
}
