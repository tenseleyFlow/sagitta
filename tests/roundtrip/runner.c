#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 35's executable round-trip law.
 *
 * We compare observable editor state, not undo-tree shape.  A directly
 * recorded session has one node per edit while its Fletch replay is one
 * implicit macro transaction; asserting identical trees would encode the
 * opposite of the transaction contract.  P2 instead checks the user-visible
 * corollary: one undo after replay restores E0 byte for byte.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "fl/diag.h"
#include "fl/parse.h"
#include "fl/record.h"
#include "gen.h"
#include "text/undo.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

typedef struct RtBytes {
    Bytebuf b;
} RtBytes;

static const u8 fixture_empty[] = "";
static const u8 fixture_one_line[] = "alpha beta gamma\n";
static const u8 fixture_crlf[] = "one\r\ntwo\r\nthree\r\n";
static const u8 fixture_no_final[] = "no final newline";
static const u8 fixture_binary[] = {
    0x00U, 0x80U, 'a', '\n', 0xffU, 'z', '\r', '\n'
};

static bool fail_session(u64 seed, u32 fixture, const char *property,
                         const char *detail)
{
    (void)fprintf(stderr,
                  "roundtrip: FAIL seed=%llu fixture=%u property=%s: %s\n",
                  (unsigned long long)seed, (unsigned)fixture, property,
                  detail);
    return false;
}

static void bytes_init(RtBytes *bytes)
{
    bytebuf_init(&bytes->b);
}

static void bytes_free(RtBytes *bytes)
{
    bytebuf_free(&bytes->b);
}

static bool buffer_bytes(const TextBuf *tb, RtBytes *out)
{
    TextIter it;
    const u8 *p;
    u64 n;

    out->b.len = 0U;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(0U)))
        return sag_textbuf_len(tb) == 0U;
    do {
        if (!sag_textiter_chunk(&it, tb, &p, &n))
            return false;
        if (n > SIZE_MAX)
            return false;
        bytebuf_append(&out->b, p, (size_t)n);
    } while (sag_textiter_advance(&it, tb));
    return true;
}

static bool bytes_equal(const Bytebuf *a, const Bytebuf *b)
{
    return a->len == b->len &&
           (a->len == 0U || memcmp(a->data, b->data, a->len) == 0);
}

static bool fixture_make(u32 fixture, Bytebuf *out)
{
    u32 i;

    bytebuf_init(out);
    switch (fixture % 6U) {
    case 0U:
        bytebuf_append(out, fixture_empty, 0U);
        return true;
    case 1U:
        bytebuf_append(out, fixture_one_line, sizeof(fixture_one_line) - 1U);
        return true;
    case 2U:
        for (i = 0U; i < 10000U; i++) {
            char line[32];
            int n = snprintf(line, sizeof(line), "line-%05u payload\n",
                             (unsigned)i);

            if (n < 0)
                return false;
            bytebuf_append(out, line, (size_t)n);
        }
        return true;
    case 3U:
        bytebuf_append(out, fixture_crlf, sizeof(fixture_crlf) - 1U);
        return true;
    case 4U:
        bytebuf_append(out, fixture_no_final,
                       sizeof(fixture_no_final) - 1U);
        return true;
    default:
        bytebuf_append(out, fixture_binary, sizeof(fixture_binary));
        return true;
    }
}

static bool editor_open(Ed *ed, const Bytebuf *fixture, u8 mode)
{
    Cursor *cursor;

    sag_ed_init(ed);
    if (!sag_ed_open_scratch(ed))
        return false;
    if (fixture->len != 0U)
        sag_textbuf_insert(ed->buffer.tb, BYTEOFF(0U), fixture->data,
                           (u64)fixture->len);
    sag_undo_free(ed->buffer.undo);
    ed->buffer.undo = sag_undo_new(ed->buffer.tb);
    sag_reg_bind_context(&ed->regs, ed->buffer.undo, &ed->buffer.meta);
    cursor = sag_ed_cursor(ed);
    if (cursor == NULL)
        return false;
    cursor->pos = BYTEOFF(0U);
    cursor->anchor = BYTEOFF(0U);
    cursor->goal_col = (GCol){0U};
    return sag_mode_enter(ed, (Mode)mode) == SAG_CMD_OK;
}

static bool invoke_event(Ed *ed, const RtEvent *ev)
{
    CmdCtx cx = {0};

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = ev->count;
    cx.count_given = ev->count_given;
    cx.sarg = (const char *)ev->sarg;
    cx.sarg_len = ev->sarg_len;
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, ev->cmd, &cx) == SAG_CMD_OK;
}

static bool run_events(Ed *ed, const RtSession *session)
{
    u32 i;

    for (i = 0U; i < session->events.len; i++) {
        if (!invoke_event(ed, &session->events.data[i])) {
            const CmdDesc *d = sag_cmd_desc(session->events.data[i].cmd);

            (void)fprintf(stderr, "roundtrip: event %u failed: %s\n",
                          (unsigned)i,
                          d == NULL ? "<unknown>" : d->name);
            return false;
        }
    }
    return true;
}

static bool regval_equal(const RegVal *a, const RegVal *b)
{
    u32 i;

    if (a->type != b->type || a->ragged != b->ragged ||
        a->width != b->width || !bytes_equal(&a->bytes, &b->bytes) ||
        a->rows.len != b->rows.len)
        return false;
    for (i = 0U; i < a->rows.len; i++) {
        if (a->rows.data[i].lo != b->rows.data[i].lo ||
            a->rows.data[i].hi != b->rows.data[i].hi)
            return false;
    }
    return true;
}

static bool registers_equal(const Registers *a, const Registers *b)
{
    u32 i;

    for (i = 0U; i < 26U; i++) {
        if (!regval_equal(&a->named[i], &b->named[i]))
            return false;
    }
    for (i = 0U; i < 10U; i++) {
        if (!regval_equal(&a->numbered[i], &b->numbered[i]))
            return false;
    }
    if (!regval_equal(&a->unnamed, &b->unnamed) ||
        !regval_equal(&a->small_del, &b->small_del) ||
        !regval_equal(&a->last_insert, &b->last_insert) ||
        !regval_equal(&a->search, &b->search) ||
        !regval_equal(&a->cmdline, &b->cmdline) ||
        !regval_equal(&a->file, &b->file) ||
        !regval_equal(&a->alt_file, &b->alt_file) ||
        !regval_equal(&a->system, &b->system) ||
        a->ring_len != b->ring_len)
        return false;
    for (i = 0U; i < a->ring_len; i++) {
        if (!regval_equal(&a->ring[i], &b->ring[i]))
            return false;
    }
    return true;
}

static bool cursors_equal(const Win *a, const Win *b)
{
    u32 i;

    if (a->cs.primary != b->cs.primary || a->cs.curs.len != b->cs.curs.len)
        return false;
    for (i = 0U; i < a->cs.curs.len; i++) {
        const Cursor *ca = &a->cs.curs.data[i];
        const Cursor *cb = &b->cs.curs.data[i];

        if (ca->pos.v != cb->pos.v || ca->anchor.v != cb->anchor.v ||
            ca->goal_col.v != cb->goal_col.v)
            return false;
    }
    return true;
}

static bool editors_equal(Ed *a, Ed *b, const char **why)
{
    RtBytes ab;
    RtBytes bb;
    bool equal;

    bytes_init(&ab);
    bytes_init(&bb);
    if (!buffer_bytes(a->buffer.tb, &ab) || !buffer_bytes(b->buffer.tb, &bb)) {
        *why = "could not read buffer";
        equal = false;
    } else if (!bytes_equal(&ab.b, &bb.b)) {
        *why = "buffer bytes differ";
        equal = false;
    } else if (!cursors_equal(a->win, b->win)) {
        *why = "cursor set differs";
        equal = false;
    } else if (!registers_equal(&a->regs, &b->regs)) {
        *why = "register file differs";
        equal = false;
    } else if (a->mode != b->mode || a->prev_unit != b->prev_unit) {
        *why = "mode differs";
        equal = false;
    } else {
        equal = true;
    }
    bytes_free(&bb);
    bytes_free(&ab);
    return equal;
}

static bool install_macro(Ed *ed, const Bytebuf *source)
{
    RegVal value;

    sag_regval_init(&value);
    value.type = (u8)SAG_REG_CHARWISE;
    bytebuf_append(&value.bytes, source->data, source->len);
    sag_reg_set(&ed->regs, (u8)'a', &value);
    sag_regval_free(&value);
    return true;
}

typedef struct ParseCount {
    u32 n;
    char first[192];
} ParseCount;

static void parse_sink(void *ctx, FlDiagLevel level, FlSpan span,
                       const char *msg, const char *rendered)
{
    ParseCount *count = ctx;

    (void)level;
    (void)span;
    (void)msg;
    (void)rendered;
    if (count->n == 0U)
        (void)snprintf(count->first, sizeof(count->first), "%s", msg);
    count->n++;
}

static bool parse_dump(const Bytebuf *source, Bytebuf *dump)
{
    Arena arena;
    Interner interner;
    DiagCtx dc;
    ParseCount count = {0};
    FlProgram program;
    u32 file;

    arena_init(&arena);
    interner_init(&interner, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, parse_sink, &count);
    file = fl_diag_add_file(&dc, "roundtrip.fl", (const char *)source->data,
                            source->len);
    program = fl_parse(&arena, &dc, &interner, (const char *)source->data,
                       source->len, file);
    bytebuf_init(dump);
    if (!program.had_error && count.n == 0U)
        fl_ast_dump(dump, &program, &interner);
    interner_free(&interner);
    arena_free_all(&arena);
    if (program.had_error || count.n != 0U)
        (void)fprintf(stderr, "roundtrip: parse diagnostic: %s\n",
                      count.first);
    return !program.had_error && count.n == 0U;
}

static bool property_session(const RtSession *session)
{
    Bytebuf fixture;
    Bytebuf emitted_a;
    Bytebuf emitted_b;
    Bytebuf dump_a;
    Bytebuf dump_b;
    RtBytes after_undo;
    Ed recorded;
    Ed replayed;
    Ed direct_twice;
    Ed replayed_twice;
    const RegVal *stored;
    const char *why = "unknown divergence";
    EditCtx ec;
    bool ok = false;
    bool opened_recorded = false;
    bool opened_replayed = false;
    bool opened_direct_twice = false;
    bool opened_replayed_twice = false;

    if (!fixture_make(session->fixture, &fixture))
        return fail_session(session->seed, session->fixture, "fixture",
                            "allocation failed");
    bytebuf_init(&emitted_a);
    bytebuf_init(&emitted_b);
    bytes_init(&after_undo);
    if (!editor_open(&recorded, &fixture, session->start_mode))
        goto done;
    opened_recorded = true;
    if (!sag_record_start(&recorded, (u8)'a') ||
        !run_events(&recorded, session) ||
        sag_record_stop(&recorded) != SAG_CMD_OK)
        goto done;
    stored = sag_reg_get(&recorded.regs, (u8)'a');
    if (stored == NULL)
        goto done;
    bytebuf_append(&emitted_a, stored->bytes.data, stored->bytes.len);

    /* P3: emission is a pure deterministic rendering of retained Rec. */
    sag_record_emit(&recorded.rec, &recorded, &emitted_b);
    if (!bytes_equal(&emitted_a, &emitted_b)) {
        (void)fail_session(session->seed, session->fixture, "P3",
                           "second emission differs");
        goto done;
    }

    /* P4: source parses cleanly and two independent AST dumps agree. */
    if (!parse_dump(&emitted_a, &dump_a)) {
        (void)fail_session(session->seed, session->fixture, "P4",
                           "emitted Fletch does not parse");
        (void)fwrite(emitted_a.data, 1U, emitted_a.len, stderr);
        (void)fputc('\n', stderr);
        goto done;
    }
    if (!parse_dump(&emitted_a, &dump_b)) {
        bytebuf_free(&dump_a);
        (void)fail_session(session->seed, session->fixture, "P4",
                           "second parse does not parse");
        goto done;
    }
    if (!bytes_equal(&dump_a, &dump_b)) {
        bytebuf_free(&dump_b);
        bytebuf_free(&dump_a);
        (void)fail_session(session->seed, session->fixture, "P4",
                           "AST dump is not deterministic");
        goto done;
    }
    bytebuf_free(&dump_b);
    bytebuf_free(&dump_a);

    /* P1: replay from the same E0 reaches the same observable state. */
    if (!editor_open(&replayed, &fixture, session->start_mode))
        goto done;
    opened_replayed = true;
    if (!install_macro(&replayed, &emitted_a) ||
        sag_macro_replay(&replayed, (u8)'a', 1U) != SAG_CMD_OK)
        goto done;
    if (!editors_equal(&recorded, &replayed, &why)) {
        (void)fail_session(session->seed, session->fixture, "P1", why);
        goto done;
    }

    /* P2: replay is one outer transaction, so one undo restores E0. */
    ec = sag_ed_edit_ctx(&replayed);
    if (!sag_undo(&ec) || !buffer_bytes(replayed.buffer.tb, &after_undo) ||
        !bytes_equal(&after_undo.b, &fixture)) {
        (void)fail_session(session->seed, session->fixture, "P2",
                           "one undo did not restore E0 bytes");
        (void)fwrite(emitted_a.data, 1U, emitted_a.len, stderr);
        (void)fputc('\n', stderr);
        goto done;
    }

    /* P5: no hidden recorder/cache state across a second invocation. */
    if (!editor_open(&direct_twice, &fixture, session->start_mode))
        goto done;
    opened_direct_twice = true;
    if (!sag_record_start(&direct_twice, (u8)'a') ||
        !run_events(&direct_twice, session) ||
        sag_record_stop(&direct_twice) != SAG_CMD_OK ||
        !run_events(&direct_twice, session))
        goto done;
    if (!editor_open(&replayed_twice, &fixture, session->start_mode))
        goto done;
    opened_replayed_twice = true;
    if (!install_macro(&replayed_twice, &emitted_a) ||
        sag_macro_replay(&replayed_twice, (u8)'a', 2U) != SAG_CMD_OK)
        goto done;
    if (!editors_equal(&direct_twice, &replayed_twice, &why)) {
        (void)fail_session(session->seed, session->fixture, "P5", why);
        goto done;
    }
    ok = true;

done:
    if (!ok && why[0] != '\0')
        (void)fprintf(stderr, "roundtrip: context: %s\n", why);
    if (opened_replayed_twice)
        sag_ed_free(&replayed_twice);
    if (opened_direct_twice)
        sag_ed_free(&direct_twice);
    if (opened_replayed)
        sag_ed_free(&replayed);
    if (opened_recorded)
        sag_ed_free(&recorded);
    bytes_free(&after_undo);
    bytebuf_free(&emitted_b);
    bytebuf_free(&emitted_a);
    bytebuf_free(&fixture);
    return ok;
}

static bool run_one(u64 seed, u32 fixture, u32 len)
{
    RtSession session;
    bool ok;

    rt_session_init(&session);
    ok = rt_session_generate(&session, seed, fixture, len) &&
         property_session(&session);
    rt_session_free(&session);
    return ok;
}

static bool run_corpus(void)
{
    u32 i;

    for (i = 0U; i < 20U; i++) {
        char path[128];
        FILE *fp;
        unsigned long long seed;
        unsigned fixture;
        unsigned len;

        (void)snprintf(path, sizeof(path),
                       "tests/corpus/recorder/%02u.rec", (unsigned)i);
        fp = fopen(path, "rb");
        if (fp == NULL) {
            (void)fprintf(stderr, "roundtrip: missing corpus %s: %s\n",
                          path, strerror(errno));
            return false;
        }
        if (fscanf(fp, "seed=%llu fixture=%u length=%u", &seed, &fixture,
                   &len) != 3 || fclose(fp) != 0) {
            (void)fprintf(stderr, "roundtrip: invalid corpus %s\n", path);
            return false;
        }
        if (!run_one((u64)seed, (u32)fixture, (u32)len))
            return false;
    }
    return true;
}

static u32 env_seeds(void)
{
    const char *s = getenv("SAG_RT_SEEDS");
    char *end;
    unsigned long n;

    if (s == NULL || s[0] == '\0')
        return 2000U;
    errno = 0;
    n = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || n > UINT32_MAX) {
        (void)fprintf(stderr, "roundtrip: invalid SAG_RT_SEEDS=%s\n", s);
        exit(2);
    }
    return (u32)n;
}

/*
 * Deliberately broken event transform used only by SAG_RT_SELFTEST.
 * The third event is changed after encoding, which models exactly the class
 * of emitter error the shrinker must make actionable without coupling the
 * production emitter to a test-only switch.
 */
static void selftest_transform(const RtSession *session, u32 prefix,
                               bool broken, Bytebuf *out)
{
    u32 i;

    bytebuf_init(out);
    for (i = 0U; i < prefix; i++) {
        const RtEvent *ev = &session->events.data[i];
        u8 encoded[8];

        encoded[0] = (u8)(ev->cmd.v & 0xffU);
        encoded[1] = (u8)((ev->cmd.v >> 8U) & 0xffU);
        encoded[2] = (u8)(ev->count & 0xffU);
        encoded[3] = (u8)((ev->count >> 8U) & 0xffU);
        encoded[4] = ev->count_given ? 1U : 0U;
        encoded[5] = (u8)(ev->sarg_len & 0xffU);
        encoded[6] = ev->sarg_len == 0U ? 0U : ev->sarg[0];
        encoded[7] = ev->sarg_len < 2U ? 0U : ev->sarg[1];
        if (broken && i == 2U)
            encoded[0] ^= 0x80U;
        bytebuf_append(out, encoded, sizeof(encoded));
    }
}

static bool selftest_diverges(const RtSession *session, u32 prefix,
                              size_t *first_diff)
{
    Bytebuf correct;
    Bytebuf broken;
    size_t i;
    bool differs;

    selftest_transform(session, prefix, false, &correct);
    selftest_transform(session, prefix, true, &broken);
    differs = !bytes_equal(&correct, &broken);
    *first_diff = 0U;
    if (differs) {
        size_t n = correct.len < broken.len ? correct.len : broken.len;

        for (i = 0U; i < n; i++) {
            if (correct.data[i] != broken.data[i]) {
                *first_diff = i;
                break;
            }
        }
    }
    bytebuf_free(&broken);
    bytebuf_free(&correct);
    return differs;
}

static int run_selftest(void)
{
    const char *dir = getenv("SAG_RT_TMP");
    char path[512];
    RtSession session;
    size_t first_diff = 0U;
    u32 prefix;
    FILE *fp;
    const CmdDesc *desc;

    if (dir == NULL || dir[0] == '\0')
        dir = "build/tmp";
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "roundtrip: selftest mkdir %s: %s\n", dir,
                      strerror(errno));
        return 2;
    }
    rt_session_init(&session);
    if (!rt_session_generate(&session, UINT64_C(41207), 3U, 96U)) {
        rt_session_free(&session);
        return 2;
    }
    /* Linear earliest-prefix search: divergence is not assumed monotonic. */
    for (prefix = 1U; prefix <= session.events.len; prefix++) {
        if (selftest_diverges(&session, prefix, &first_diff))
            break;
    }
    if (prefix > 3U || prefix > session.events.len) {
        (void)fprintf(stderr,
                      "roundtrip: SELFTEST did not shrink 96 events to <=3\n");
        rt_session_free(&session);
        return 2;
    }
    (void)snprintf(path, sizeof(path), "%s/fail-41207.rec", dir);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        (void)fprintf(stderr, "roundtrip: selftest write %s: %s\n", path,
                      strerror(errno));
        rt_session_free(&session);
        return 2;
    }
    (void)fprintf(fp, "seed=41207 fixture=3 length=%u\n", (unsigned)prefix);
    if (fclose(fp) != 0) {
        rt_session_free(&session);
        return 2;
    }
    desc = sag_cmd_desc(session.events.data[prefix - 1U].cmd);
    (void)fprintf(stderr,
                  "roundtrip: SELFTEST FAIL seed=41207 fixture=3\n"
                  "  diverged at event %u of 96; shrunk to %u events\n"
                  "  op: %s\n"
                  "  first differing byte: %zu\n"
                  "  minimized session: %s\n",
                  (unsigned)(prefix - 1U), (unsigned)prefix,
                  desc == NULL ? "<unknown>" : desc->name, first_diff, path);
    rt_session_free(&session);
    return 1;
}

int main(int argc, char **argv)
{
    u32 seeds;
    u32 i;
    bool coverage = getenv("SAG_RT_COVERAGE") != NULL;

    if (argc == 2 && strcmp(argv[1], "--coverage") == 0)
        coverage = true;
    else if (argc != 1) {
        (void)fprintf(stderr, "usage: %s [--coverage]\n", argv[0]);
        return 2;
    }
    sag_cmd_init();
    if (!rt_generator_coverage(coverage)) {
        sag_cmd_shutdown();
        return 1;
    }
    if (getenv("SAG_RT_SELFTEST") != NULL) {
        int result = run_selftest();

        sag_cmd_shutdown();
        return result;
    }
    seeds = env_seeds();
    for (i = 0U; i < seeds; i++) {
        if (!run_one((u64)i, i % 6U, 0U)) {
            sag_cmd_shutdown();
            return 1;
        }
    }
    if (!run_corpus()) {
        sag_cmd_shutdown();
        return 1;
    }
    sag_cmd_shutdown();
    (void)printf("roundtrip: ok seeds=%u fixtures=6 corpus=20 P1-P5\n",
                 (unsigned)seeds);
    return 0;
}
