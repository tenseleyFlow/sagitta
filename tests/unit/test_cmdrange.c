#include "harness.h"

#include <string.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "ui/cmdparse.h"

typedef struct RangeFixture {
    Ed ed;
    TextBuf *tb;
    Arena arena;
} RangeFixture;

static CmdStatus range_nop(CmdCtx *cx)
{
    (void)cx;
    return SAG_CMD_OK;
}

static void range_register(const char *name, const char *abbrev, u8 policy)
{
    CmdEntry entry = {
        {name, range_nop, SAG_ARITY_NONE, 0U, "Range test command", NULL},
        "", policy, abbrev};

    (void)sag_cmd_register_entry(&entry);
}

static void range_fixture_init(RangeFixture *f, const char *text)
{
    Cursor cursor = {BYTEOFF(4U), {1U}, BYTEOFF(4U)};

    memset(f, 0, sizeof(*f));
    f->tb = sag_textbuf_from_bytes((const u8 *)text, strlen(text));
    f->ed.buffer.tb = f->tb;
    f->ed.single_win.buf = &f->ed.buffer;
    sag_cset_init(&f->ed.single_win.cs, cursor);
    f->ed.win = &f->ed.single_win;
    arena_init(&f->arena);
    sag_cmd_shutdown();
    sag_cmd_init();
    range_register("ed.ui.delete", "td", SAG_RP_LINE);
    range_register("ed.ui.open", "to", SAG_RP_OPT);
    range_register("ed.ui.close", "tq", SAG_RP_FORBID);
    range_register("ed.ui.grow", "tb", SAG_RP_BUFFER);
    range_register("ed.ui.shrink", "tsort", SAG_RP_REQUIRED);
}

static void range_fixture_free(RangeFixture *f)
{
    sag_cmd_shutdown();
    arena_free_all(&f->arena);
    sag_cset_free(&f->ed.single_win.cs);
    sag_textbuf_free(f->tb);
}

static CmdRange parse_range_ok(RangeFixture *f, const char *line)
{
    CmdParse parsed;

    SAG_ASSERT(sag_cmd_parse(&f->ed, line, strlen(line), &f->arena,
                             &parsed));
    return parsed.range;
}

static void range_error(RangeFixture *f, const char *line,
                        const char *message)
{
    CmdParse parsed;

    SAG_ASSERT(!sag_cmd_parse(&f->ed, line, strlen(line), &f->arena,
                              &parsed));
    SAG_ASSERT_EQ_STR(parsed.err.msg, message);
    SAG_ASSERT(parsed.err.tok_lo < parsed.err.tok_hi);
    SAG_ASSERT(parsed.err.tok_hi <= strlen(line));
}

/* Sprint 21 §7: `'a` and `/pat/` as addresses, now that they resolve. */
void test_cmdrange_mark_and_pattern_addresses_resolve(void)
{
    RangeFixture f;
    CmdRange range;

    range_fixture_init(&f, "aa\nbb\ncc\ndd");
    f.ed.buffer.marks = sag_marks_new();
    sag_search_opts_init(&f.ed.search_opts);

    /* A mark on line 2 (byte 6 is the start of "cc"). */
    SAG_ASSERT(sag_ed_mark_set(&f.ed, &f.ed.buffer, (u8)'a', BYTEOFF(6U)));
    range = parse_range_ok(&f, ":'atd");
    SAG_ASSERT_EQ_U64(range.lo.v, 2U);
    SAG_ASSERT_EQ_U64(range.hi.v, 2U);
    SAG_ASSERT(range.given);

    /* The cursor is on line 1, so a forward pattern address finds the
     * next match after it. */
    range = parse_range_ok(&f, ":/dd/td");
    SAG_ASSERT_EQ_U64(range.lo.v, 3U);

    /* A backward address searches the other way. */
    range = parse_range_ok(&f, ":?aa?td");
    SAG_ASSERT_EQ_U64(range.lo.v, 0U);

    /* A range built from two addresses. */
    range = parse_range_ok(&f, ":'a,/dd/td");
    SAG_ASSERT_EQ_U64(range.lo.v, 2U);
    SAG_ASSERT_EQ_U64(range.hi.v, 3U);

    /* A mark follows an edit above it, which is the whole reason the
     * table stores marks rather than line numbers. */
    SAG_ASSERT(sag_ed_mark_get(&f.ed, &f.ed.buffer, (u8)'a', NULL));
    sag_marks_adjust(f.ed.buffer.marks, SAG_JOURNAL_INS, BYTEOFF(0U), 3U);
    {
        ByteOff at;

        SAG_ASSERT(sag_ed_mark_get(&f.ed, &f.ed.buffer, (u8)'a', &at));
        SAG_ASSERT_EQ_U64(at.v, 9U);
    }
    sag_marks_free(f.ed.buffer.marks);
    f.ed.buffer.marks = NULL;
    range_fixture_free(&f);
}

void test_cmdrange_addresses_policies_and_deferred_forms(void)
{
    static const struct {
        const char *line;
        u64 lo;
        u64 hi;
        u8 kind;
        bool given;
    } rows[] = {
        {":td", 1U, 1U, SAG_RANGE_LINES, false},
        {":1td", 0U, 0U, SAG_RANGE_LINES, true},
        {":.td", 1U, 1U, SAG_RANGE_LINES, true},
        {":$td", 3U, 3U, SAG_RANGE_LINES, true},
        {":+td", 2U, 2U, SAG_RANGE_LINES, true},
        {":-td", 0U, 0U, SAG_RANGE_LINES, true},
        {":.+2td", 3U, 3U, SAG_RANGE_LINES, true},
        {":$-2td", 1U, 1U, SAG_RANGE_LINES, true},
        {":1,3td", 0U, 2U, SAG_RANGE_LINES, true},
        {":%td", 0U, 3U, SAG_RANGE_BUFFER, true},
        {":to", 0U, 0U, SAG_RANGE_NONE, false},
        {":tb", 0U, 3U, SAG_RANGE_BUFFER, false},
        {":1,2tsort", 0U, 1U, SAG_RANGE_LINES, true},
    };
    RangeFixture f;
    size_t i;

    range_fixture_init(&f, "aa\nbb\ncc\ndd");
    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        CmdRange range = parse_range_ok(&f, rows[i].line);

        SAG_ASSERT_EQ_U64(range.lo.v, rows[i].lo);
        SAG_ASSERT_EQ_U64(range.hi.v, rows[i].hi);
        SAG_ASSERT_EQ_U64(range.kind, rows[i].kind);
        SAG_ASSERT_EQ_U64(range.given, rows[i].given);
    }
    range_error(&f, ":1tq", ":ui.close takes no range");
    range_error(&f, ":tsort", ":ui.shrink requires a range");
    range_error(&f, ":3,2td", "backwards range (3,2)");
    range_error(&f, ":900td",
                "line 900 past end of buffer (4 lines)");
    range_error(&f, ":0td", "line 0 past end of buffer (4 lines)");
    /*
     * Sprint 21 closed both deferrals.  An unset mark and an unmatched
     * pattern are errors that name what went wrong — addressing the
     * wrong line silently is worse than refusing.
     */
    range_error(&f, ":'atd", "mark not set");
    range_error(&f, ":/re/td", "pattern not found");
    range_error(&f, ":?re?td", "pattern not found");
    range_fixture_free(&f);
}

void test_cmdrange_selection_and_eol_spans(void)
{
    RangeFixture f;
    CmdRange range;
    Span bytes;

    range_fixture_init(&f, "aa\nbb\ncc\ndd");
    range = parse_range_ok(&f, ":1,2td");
    bytes = sag_range_span(f.tb, &range);
    SAG_ASSERT_EQ_U64(bytes.lo, 0U);
    SAG_ASSERT_EQ_U64(bytes.hi, 6U);

    range = parse_range_ok(&f, ":3,4td");
    bytes = sag_range_span(f.tb, &range);
    SAG_ASSERT_EQ_U64(bytes.lo, 6U);
    SAG_ASSERT_EQ_U64(bytes.hi, 11U);

    f.ed.single_win.cs.curs.data[0].anchor = BYTEOFF(0U);
    f.ed.single_win.cs.curs.data[0].pos = BYTEOFF(7U);
    range = parse_range_ok(&f, ":'<,'>td");
    SAG_ASSERT_EQ_U64(range.kind, SAG_RANGE_SELECTION);
    SAG_ASSERT_EQ_U64(range.lo.v, 0U);
    SAG_ASSERT_EQ_U64(range.hi.v, 2U);
    bytes = sag_range_span(f.tb, &range);
    SAG_ASSERT_EQ_U64(bytes.lo, 0U);
    SAG_ASSERT_EQ_U64(bytes.hi, 9U);
    range_fixture_free(&f);

    range_fixture_init(&f, "");
    range = parse_range_ok(&f, ":%td");
    bytes = sag_range_span(f.tb, &range);
    SAG_ASSERT_EQ_U64(bytes.lo, 0U);
    SAG_ASSERT_EQ_U64(bytes.hi, 0U);
    range_fixture_free(&f);
}
