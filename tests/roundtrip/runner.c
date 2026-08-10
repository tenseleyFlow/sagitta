#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 35's executable round-trip law.
 *
 * We compare observable editor state, not undo-tree shape.  A directly
 * recorded session has one node per edit while its Fletch replay is one
 * implicit macro transaction; asserting identical trees would encode the
 * opposite of the transaction contract.  P2 instead checks the user-visible
 * corollary: one undo after replay restores E0 byte for byte.  P5 is a
 * compositional check over the generator's deliberately mode-closed sessions:
 * each S restores its starting L/W/B unit before returning.  It does not claim
 * that arbitrary mode-leaking command sequences compose as S;S.
 */

#include <errno.h>
#include <dirent.h>
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

static char last_property[16];
static char last_detail[192];

static bool fail_session(u64 seed, u32 fixture, const char *property,
                         const char *detail)
{
    (void)seed;
    (void)fixture;
    (void)snprintf(last_property, sizeof(last_property), "%s", property);
    (void)snprintf(last_detail, sizeof(last_detail), "%s", detail);
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

static size_t first_byte_difference(const Bytebuf *a, const Bytebuf *b)
{
    size_t i;
    size_t n = a->len < b->len ? a->len : b->len;

    for (i = 0U; i < n; i++)
        if (a->data[i] != b->data[i])
            return i;
    return a->len == b->len ? SIZE_MAX : n;
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
    cx.iarg = ev->iarg;
    cx.range = ev->range;
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

    /* P5: no hidden state across a second mode-closed invocation. */
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

static bool corrupt_takes_count_fold(const Bytebuf *source, Bytebuf *out)
{
    static const char needle[] = "buf_end buf_end i\"x\"";
    static const char pair[] = "buf_end buf_end";
    static const char folded[] = "2buf_end";
    size_t i;

    bytebuf_init(out);
    for (i = 0U; i + sizeof(needle) - 1U <= source->len; i++) {
        if (memcmp(source->data + i, needle, sizeof(needle) - 1U) == 0) {
            bytebuf_append(out, source->data, i);
            bytebuf_append(out, folded, sizeof(folded) - 1U);
            bytebuf_append(out, source->data + i + sizeof(pair) - 1U,
                           source->len - i - sizeof(pair) + 1U);
            return true;
        }
    }
    bytebuf_free(out);
    return false;
}

static bool p1_diverges(const RtSession *session, u32 prefix, bool corrupt,
                        size_t *byte_at, Bytebuf *emitted_out)
{
    Bytebuf fixture;
    Bytebuf source;
    Bytebuf bad_source;
    RtBytes direct_bytes;
    RtBytes replay_bytes;
    Ed direct;
    Ed replay;
    const RegVal *stored;
    const char *why = NULL;
    bool direct_open = false;
    bool replay_open = false;
    bool differs = false;
    RtSession view = *session;

    *byte_at = SIZE_MAX;
    bytebuf_init(emitted_out);
    bytebuf_init(&source);
    bytebuf_init(&bad_source);
    bytes_init(&direct_bytes);
    bytes_init(&replay_bytes);
    if (prefix > session->events.len || !fixture_make(session->fixture, &fixture))
        goto done;
    view.events.len = prefix;
    if (!editor_open(&direct, &fixture, session->start_mode))
        goto fixture_done;
    direct_open = true;
    if (!sag_record_start(&direct, (u8)'a') || !run_events(&direct, &view) ||
        sag_record_stop(&direct) != SAG_CMD_OK)
        goto fixture_done;
    stored = sag_reg_get(&direct.regs, (u8)'a');
    if (stored == NULL)
        goto fixture_done;
    bytebuf_append(&source, stored->bytes.data, stored->bytes.len);
    if (corrupt) {
        if (!corrupt_takes_count_fold(&source, &bad_source))
            goto fixture_done;
    } else {
        bytebuf_append(&bad_source, source.data, source.len);
    }
    bytebuf_append(emitted_out, bad_source.data, bad_source.len);
    if (!editor_open(&replay, &fixture, session->start_mode))
        goto fixture_done;
    replay_open = true;
    if (!install_macro(&replay, &bad_source) ||
        sag_macro_replay(&replay, (u8)'a', 1U) != SAG_CMD_OK)
        goto fixture_done;
    if (corrupt && !install_macro(&direct, &bad_source))
        goto fixture_done;
    differs = !editors_equal(&direct, &replay, &why);
    if (differs && buffer_bytes(direct.buffer.tb, &direct_bytes) &&
        buffer_bytes(replay.buffer.tb, &replay_bytes))
        *byte_at = first_byte_difference(&direct_bytes.b, &replay_bytes.b);

fixture_done:
    if (replay_open)
        sag_ed_free(&replay);
    if (direct_open)
        sag_ed_free(&direct);
    bytebuf_free(&fixture);
done:
    bytes_free(&replay_bytes);
    bytes_free(&direct_bytes);
    bytebuf_free(&bad_source);
    bytebuf_free(&source);
    return differs;
}

static bool write_failure_artifact(const RtSession *session, u32 prefix,
                                   const char *fault, char *path,
                                   size_t path_cap)
{
    const char *dir = getenv("SAG_RT_TMP");
    FILE *fp;

    if (dir == NULL || dir[0] == '\0')
        dir = "build/tmp";
    if (mkdir(dir, 0777) != 0 && errno != EEXIST)
        return false;
    (void)snprintf(path, path_cap, "%s/fail-%llu.rec", dir,
                   (unsigned long long)session->seed);
    fp = fopen(path, "wb");
    if (fp == NULL)
        return false;
    (void)fprintf(fp,
                  "seed=%llu fixture=%u length=%u prefix=%u fault=%s\n",
                  (unsigned long long)session->seed,
                  (unsigned)session->fixture,
                  (unsigned)session->generated_len, (unsigned)prefix,
                  fault == NULL ? "none" : fault);
    return fclose(fp) == 0;
}

static void report_failure(const RtSession *session, bool corrupt)
{
    Bytebuf source;
    size_t byte_at = SIZE_MAX;
    u32 prefix;
    u32 op_at;
    char path[512] = "<artifact write failed>";
    const CmdDesc *desc;

    for (prefix = 1U; prefix <= session->events.len; prefix++) {
        bytebuf_init(&source);
        if (p1_diverges(session, prefix, corrupt, &byte_at, &source))
            break;
        bytebuf_free(&source);
    }
    if (prefix > session->events.len) {
        prefix = (u32)session->events.len;
        bytebuf_init(&source);
        (void)p1_diverges(session, prefix, corrupt, &byte_at, &source);
    }
    (void)write_failure_artifact(session, prefix,
                                 corrupt ? "fold-takes-count" : "none",
                                 path, sizeof(path));
    op_at = corrupt ? 0U : prefix == 0U ? 0U : prefix - 1U;
    desc = prefix == 0U ? NULL : sag_cmd_desc(session->events.data[op_at].cmd);
    (void)fprintf(stderr,
                  "roundtrip: FAIL seed=%llu fixture=%u property=%s\n"
                  "  diverged at event %u of %u; shrunk from %u\n"
                  "  op: %s\n"
                  "  first differing byte: %s%zu\n"
                  "  detail: %s\n"
                  "  minimized session: %s\n"
                  "  emitted source:\n",
                  (unsigned long long)session->seed,
                  (unsigned)session->fixture,
                  corrupt ? "SELFTEST/P1" : last_property,
                  (unsigned)op_at,
                  (unsigned)prefix, (unsigned)session->events.len,
                  desc == NULL ? "<unknown>" : desc->name,
                  byte_at == SIZE_MAX ? "unavailable " : "",
                  byte_at == SIZE_MAX ? 0U : byte_at,
                  corrupt ? "TAKES_COUNT run was illegally folded" :
                            last_detail,
                  path);
    (void)fwrite(source.data, 1U, source.len, stderr);
    (void)fputc('\n', stderr);
    bytebuf_free(&source);
}

static bool run_one(u64 seed, u32 fixture, u32 len)
{
    RtSession session;
    bool ok;

    rt_session_init(&session);
    ok = rt_session_generate(&session, seed, fixture, len) &&
         property_session(&session);
    if (!ok && session.events.len != 0U)
        report_failure(&session, false);
    rt_session_free(&session);
    return ok;
}

static bool init_count_folding_session(RtSession *session, u32 length)
{
    static const u8 insert[] = {'x'};
    const char *name;
    RtEvent event = {0};
    u32 i;

    if (length < 3U)
        return false;
    rt_session_init(session);
    session->seed = UINT64_C(0x534147434f554e54);
    session->fixture = 2U;
    session->generated_len = length;
    session->start_mode = (u8)SAG_MODE_L;

    event.cmd = sag_cmd_lookup("ed.move.buf.end",
                               (u32)strlen("ed.move.buf.end"));
    event.count = 1U;
    if (event.cmd.v == 0U)
        goto fail;
    RtEventVec_push(&session->events, event);
    RtEventVec_push(&session->events, event);

    event = (RtEvent){0};
    event.cmd = sag_cmd_lookup("ed.edit.insert.text",
                               (u32)strlen("ed.edit.insert.text"));
    event.count = 1U;
    event.sarg = insert;
    event.sarg_len = sizeof(insert);
    if (event.cmd.v == 0U)
        goto fail;
    RtEventVec_push(&session->events, event);

    for (i = 3U; i < length; i++) {
        name = (i & 1U) == 0U ? "ed.move.unit.next" :
                                "ed.move.unit.prev";
        event = (RtEvent){0};
        event.cmd = sag_cmd_lookup(name, (u32)strlen(name));
        event.count = 1U;
        if (event.cmd.v == 0U)
            goto fail;
        RtEventVec_push(&session->events, event);
    }
    return true;

fail:
    rt_session_free(session);
    return false;
}

static bool run_count_folding_sentinel(void)
{
    RtSession session;
    bool ok;

    if (!init_count_folding_session(&session, 3U))
        return false;

    /* Two uncounted TAKES_COUNT operations must remain two operations.
     * Folding them into `2buf_end` changes where the following insert lands. */
    ok = property_session(&session);
    rt_session_free(&session);
    return ok;
}

static bool rec_name(const char *name)
{
    size_t n = strlen(name);

    return n > 4U && strcmp(name + n - 4U, ".rec") == 0;
}

static void sort_names(char **names, u32 n)
{
    u32 i;

    for (i = 1U; i < n; i++) {
        char *value = names[i];
        u32 j = i;

        while (j != 0U && strcmp(names[j - 1U], value) > 0) {
            names[j] = names[j - 1U];
            j--;
        }
        names[j] = value;
    }
}

static bool run_corpus(u32 *count_out)
{
    static const char dir_name[] = "tests/corpus/recorder";
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    u32 names_len = 0U;
    u32 names_cap = 0U;
    u32 i;
    bool ok = false;

    *count_out = 0U;
    dir = opendir(dir_name);
    if (dir == NULL) {
        (void)fprintf(stderr, "roundtrip: open corpus %s: %s\n", dir_name,
                      strerror(errno));
        return false;
    }
    while ((entry = readdir(dir)) != NULL) {
        size_t n;

        if (!rec_name(entry->d_name))
            continue;
        if (names_len == names_cap) {
            names_cap = names_cap == 0U ? 32U : names_cap * 2U;
            names = sag_xreallocarray(names, names_cap, sizeof(*names));
        }
        n = strlen(entry->d_name);
        names[names_len] = sag_xmalloc(n + 1U);
        (void)memcpy(names[names_len], entry->d_name, n + 1U);
        names_len++;
    }
    if (closedir(dir) != 0)
        goto done;
    sort_names(names, names_len);

    for (i = 0U; i < names_len; i++) {
        char path[512];
        FILE *fp;
        unsigned long long seed;
        unsigned fixture;
        unsigned len;
        unsigned prefix = 0U;
        int parsed;

        (void)snprintf(path, sizeof(path), "%s/%s", dir_name, names[i]);
        fp = fopen(path, "rb");
        if (fp == NULL) {
            (void)fprintf(stderr, "roundtrip: missing corpus %s: %s\n",
                          path, strerror(errno));
            goto done;
        }
        parsed = fscanf(fp, "seed=%llu fixture=%u length=%u prefix=%u",
                        &seed, &fixture, &len, &prefix);
        if (parsed < 3 || fclose(fp) != 0) {
            (void)fprintf(stderr, "roundtrip: invalid corpus %s\n", path);
            goto done;
        }
        if (prefix == 0U) {
            if (!run_one((u64)seed, (u32)fixture, (u32)len))
                goto done;
        } else {
            RtSession session;

            rt_session_init(&session);
            if (!rt_session_generate(&session, (u64)seed, (u32)fixture,
                                     (u32)len) ||
                prefix > session.events.len) {
                rt_session_free(&session);
                goto done;
            }
            session.events.len = prefix;
            if (!property_session(&session)) {
                rt_session_free(&session);
                goto done;
            }
            rt_session_free(&session);
        }
    }
    *count_out = names_len;
    ok = names_len >= 20U;
    if (!ok)
        (void)fprintf(stderr, "roundtrip: corpus has %u files; need >=20\n",
                      (unsigned)names_len);
done:
    for (i = 0U; i < names_len; i++)
        free(names[i]);
    free(names);
    return ok;
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

static u64 env_base_seed(void)
{
    const char *s = getenv("SAG_RT_BASE_SEED");
    char *end;
    unsigned long long n;

    if (s == NULL || s[0] == '\0')
        return 0U;
    errno = 0;
    n = strtoull(s, &end, 0);
    if (errno != 0 || *end != '\0') {
        (void)fprintf(stderr, "roundtrip: invalid SAG_RT_BASE_SEED=%s\n", s);
        exit(2);
    }
    return (u64)n;
}

static bool has_count_folding_prefix(const RtSession *session)
{
    const CmdDesc *first;
    const CmdDesc *second;
    const CmdDesc *third;

    if (session->events.len < 3U)
        return false;
    first = sag_cmd_desc(session->events.data[0].cmd);
    second = sag_cmd_desc(session->events.data[1].cmd);
    third = sag_cmd_desc(session->events.data[2].cmd);
    return first != NULL && second != NULL && third != NULL &&
           strcmp(first->name, "ed.move.buf.end") == 0 &&
           strcmp(second->name, "ed.move.buf.end") == 0 &&
           !session->events.data[0].count_given &&
           !session->events.data[1].count_given &&
           strcmp(third->name, "ed.edit.insert.text") == 0 &&
           !session->events.data[2].count_given &&
           session->events.data[2].sarg_len == 1U &&
           session->events.data[2].sarg[0] == (u8)'x';
}

static int run_selftest(void)
{
    const u64 seed = UINT64_C(20764);
    RtSession session;

    /* Use an ordinary generator seed so the minimized .rec can be copied
     * directly into the corpus and reconstructed by run_corpus().  Keep the
     * seed pinned: a generator change that invalidates the fixture must fail
     * this self-test visibly rather than silently selecting another case. */
    rt_session_init(&session);
    if (!rt_session_generate(&session, seed, 2U, 96U) ||
        !has_count_folding_prefix(&session) || session.events.len < 96U) {
        rt_session_free(&session);
        return 2;
    }
    session.events.len = 96U;
    /* Fold a TAKES_COUNT run in real emitted Fletch, then execute it through
     * the real VM.  The shortest divergent prefix is exactly three events. */
    report_failure(&session, true);
    rt_session_free(&session);
    return 1;
}

int main(int argc, char **argv)
{
    u32 seeds;
    u32 corpus_count;
    u32 i;
    u64 base_seed;
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
    if (!run_count_folding_sentinel()) {
        sag_cmd_shutdown();
        return 1;
    }
    seeds = env_seeds();
    base_seed = env_base_seed();
    for (i = 0U; i < seeds; i++) {
        if (!run_one(base_seed + i, i % 6U, 0U)) {
            sag_cmd_shutdown();
            return 1;
        }
    }
    if (!run_corpus(&corpus_count)) {
        sag_cmd_shutdown();
        return 1;
    }
    sag_cmd_shutdown();
    (void)printf("roundtrip: ok seeds=%u base=%llu fixtures=6 corpus=%u "
                 "count-sentinel=1 P1-P5\n", (unsigned)seeds,
                 (unsigned long long)base_seed, (unsigned)corpus_count);
    return 0;
}
