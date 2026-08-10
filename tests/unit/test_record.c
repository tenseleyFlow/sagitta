#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/parse.h"
#include "fl/record.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct RecordEmitFix {
    Ed ed;
    Rec rec;
    Bytebuf out;
} RecordEmitFix;

static void ref_open(RecordEmitFix *f, Mode start)
{
    sag_ed_init(&f->ed);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    sag_record_init(&f->rec);
    f->rec.reg = (u8)'a';
    f->rec.mode_at_start = (u8)start;
    bytebuf_init(&f->out);
}

static void ref_close(RecordEmitFix *f)
{
    bytebuf_free(&f->out);
    sag_record_free(&f->rec);
    sag_ed_free(&f->ed);
}

static CmdId cmd(const char *name)
{
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));

    SAG_ASSERT(id.v != 0U);
    return id;
}

static void add_event(RecordEmitFix *f, const char *name, u32 count,
                      bool count_given, i64 iarg,
                      const void *sarg, u32 sarg_len, Mode mode,
                      CmdSource source)
{
    RecEvent event = {0};

    event.cmd = cmd(name);
    event.count = count;
    event.count_given = count_given;
    event.iarg = iarg;
    event.mode = (u8)mode;
    event.src = (u8)source;
    event.sarg_at = (u32)f->rec.blob.len;
    event.sarg_len = sarg_len;
    if (sarg_len != 0U)
        bytebuf_append(&f->rec.blob, sarg, sarg_len);
    RecEventVec_push(&f->rec.ev, event);
}

static const char *emit(RecordEmitFix *f)
{
    sag_record_emit(&f->rec, &f->ed, &f->out);
    bytebuf_push_u8(&f->out, 0U);
    return (const char *)f->out.data;
}

static void assert_parses(const Bytebuf *source)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlProgram program;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_diag_add_file(&dc, "record.fl", (const char *)source->data,
                           source->len);
    program = fl_parse(&arena, &dc, &in, (const char *)source->data,
                       source->len != 0U && source->data[source->len - 1U] == 0U
                           ? source->len - 1U : source->len,
                       0U);
    SAG_ASSERT(!program.had_error);
    SAG_ASSERT(!program.incomplete);
    interner_free(&in);
    arena_free_all(&arena);
}

void test_record_emit_maps_the_frozen_motion_vocabulary(void)
{
    static const struct {
        const char *name;
        const char *sarg;
        const char *want;
    } rows[] = {
        {"ed.mode.enter", "L", " l "},
        {"ed.mode.enter", "W", " w "},
        {"ed.mode.enter", "B", " b "},
        {"ed.mode.enter", "C", " c "},
        {"ed.move.unit.next", NULL, " > "},
        {"ed.move.unit.prev", NULL, " < "},
        {"ed.move.unit.up", NULL, " ^ "},
        {"ed.move.unit.down", NULL, " v "},
        {"ed.move.unit.next_alt", NULL, " a> "},
        {"ed.move.unit.prev_alt", NULL, " a< "},
        {"ed.move.unit.up_alt", NULL, " a^ "},
        {"ed.move.unit.down_alt", NULL, " av "},
        {"ed.edit.delete.unit", NULL, " del "},
        {"ed.mode.escape", NULL, " esc "}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        RecordEmitFix f;
        const char *got;

        ref_open(&f, SAG_MODE_E);
        add_event(&f, rows[i].name, 1U, false, 0,
                  rows[i].sarg, rows[i].sarg == NULL ? 0U : 1U,
                  SAG_MODE_E, SAG_SRC_KEY);
        got = emit(&f);
        SAG_ASSERT(strstr(got, rows[i].want) != NULL);
        SAG_ASSERT(strstr(got, "# sagitta-macro: 1\n") == got);
        SAG_ASSERT(strstr(got, "# recorded-with: sagitta ") != NULL);
        SAG_ASSERT(strstr(got, "# keymap: default\n") != NULL);
        SAG_ASSERT(strstr(got, "rec_a") != NULL);
        assert_parses(&f.out);
        ref_close(&f);
    }

    {
        RecordEmitFix f;
        const char *got;

        ref_open(&f, SAG_MODE_L);
        add_event(&f, "ed.mode.enter", 1U, false, 0, "H", 1U,
                  SAG_MODE_L, SAG_SRC_KEY);
        add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
                  SAG_MODE_H, SAG_SRC_KEY);
        add_event(&f, "ed.mode.escape", 1U, false, 0, NULL, 0U,
                  SAG_MODE_H, SAG_SRC_KEY);
        got = emit(&f);
        SAG_ASSERT(strstr(got, "H( > )") != NULL);
        assert_parses(&f.out);
        ref_close(&f);
    }

    {
        RecordEmitFix f;
        const char *got;

        ref_open(&f, SAG_MODE_H);
        add_event(&f, "ed.mode.escape", 1U, false, 0, NULL, 0U,
                  SAG_MODE_H, SAG_SRC_KEY);
        got = emit(&f);
        SAG_ASSERT(strstr(got, " esc ") != NULL);
        assert_parses(&f.out);
        ref_close(&f);
    }
}

void test_record_emit_folds_only_uncounted_repeatable_runs(void)
{
    RecordEmitFix f;
    const char *got;
    u32 i;

    ref_open(&f, SAG_MODE_L);
    for (i = 0U; i < 4U; i++)
        add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
                  SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.edit.delete.unit", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.edit.delete.unit", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.move.buf.end", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.move.buf.end", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    got = emit(&f);
    SAG_ASSERT(strstr(got, "4>") != NULL);
    SAG_ASSERT(strstr(got, "2del") != NULL);
    SAG_ASSERT(strstr(got, "2buf_end") == NULL);
    SAG_ASSERT(strstr(got, "buf_end buf_end") != NULL);
    assert_parses(&f.out);
    ref_close(&f);
}

void test_record_emit_preserves_explicit_counts_without_arithmetic(void)
{
    RecordEmitFix f;
    const char *got;

    ref_open(&f, SAG_MODE_L);
    add_event(&f, "ed.move.unit.next", 4U, true, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.move.unit.next", 3U, true, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    got = emit(&f);
    SAG_ASSERT(strstr(got, "4> 3>") != NULL);
    SAG_ASSERT(strstr(got, "7>") == NULL);
    assert_parses(&f.out);
    ref_close(&f);
}

void test_record_emit_concatenates_only_adjacent_insert_events(void)
{
    static const u8 one[] = {'a', '\0'};
    RecordEmitFix f;
    const char *got;

    ref_open(&f, SAG_MODE_I);
    add_event(&f, "ed.edit.insert.text", 1U, false, 0, one, sizeof(one),
              SAG_MODE_I, SAG_SRC_KEY);
    add_event(&f, "ed.edit.insert.text", 1U, false, 0, "b", 1U,
              SAG_MODE_I, SAG_SRC_KEY);
    add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
              SAG_MODE_I, SAG_SRC_KEY);
    add_event(&f, "ed.edit.insert.text", 1U, false, 0, "c", 1U,
              SAG_MODE_I, SAG_SRC_KEY);
    got = emit(&f);
    SAG_ASSERT(strstr(got, "i\"a\\0b\"") != NULL);
    SAG_ASSERT(strstr(got, "i\"c\"") != NULL);
    SAG_ASSERT(strstr(got, "a\\0bi") == NULL);
    assert_parses(&f.out);
    ref_close(&f);
}

void test_record_emit_escapes_binary_string_arguments(void)
{
    static const u8 payload[] = {'"', '\\', '\n', '\t', '\r', '\0', 0x80U};
    RecordEmitFix f;
    const char *got;

    ref_open(&f, SAG_MODE_I);
    add_event(&f, "ed.edit.insert.text", 1U, false, 0, payload,
              sizeof(payload), SAG_MODE_I, SAG_SRC_KEY);
    got = emit(&f);
    SAG_ASSERT(strstr(got, "\\\"") != NULL);
    SAG_ASSERT(strstr(got, "\\\\") != NULL);
    SAG_ASSERT(strstr(got, "\\n") != NULL);
    SAG_ASSERT(strstr(got, "\\t") != NULL);
    SAG_ASSERT(strstr(got, "\\r") != NULL);
    SAG_ASSERT(strstr(got, "\\0") != NULL);
    SAG_ASSERT(memchr(f.out.data, 0x80, f.out.len) != NULL);
    assert_parses(&f.out);
    ref_close(&f);
}

void test_record_emit_splits_argument_commands_in_event_order(void)
{
    RecordEmitFix f;
    const char *got;
    const char *before;
    const char *arg;
    const char *after;

    ref_open(&f, SAG_MODE_L);
    add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.file.save", 1U, false, 0, "a\"b.fl", 6U,
              SAG_MODE_L, SAG_SRC_KEY);
    add_event(&f, "ed.move.unit.prev", 1U, false, 0, NULL, 0U,
              SAG_MODE_L, SAG_SRC_KEY);
    got = emit(&f);
    before = strstr(got, " > ");
    arg = strstr(got, "ed.run(\"ed.file.save\"");
    after = arg == NULL ? NULL : strstr(arg, " < ");
    SAG_ASSERT_NOT_NULL(before);
    SAG_ASSERT_NOT_NULL(arg);
    SAG_ASSERT_NOT_NULL(after);
    SAG_ASSERT(before < arg);
    SAG_ASSERT(arg < after);
    SAG_ASSERT(strstr(arg, "a\\\"b.fl") != NULL);
    assert_parses(&f.out);
    ref_close(&f);
}

void test_record_emit_annotates_unit_relative_sessions(void)
{
    static const struct { Mode mode; const char *word; } rows[] = {
        {SAG_MODE_L, " l "}, {SAG_MODE_W, " w "}, {SAG_MODE_B, " b "}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        RecordEmitFix f;
        const char *got;
        const char *annotation;
        const char *motion;

        ref_open(&f, rows[i].mode);
        add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
                  rows[i].mode, SAG_SRC_KEY);
        got = emit(&f);
        annotation = strstr(got, rows[i].word);
        motion = strstr(got, " > ");
        SAG_ASSERT_NOT_NULL(annotation);
        SAG_ASSERT_NOT_NULL(motion);
        SAG_ASSERT(annotation < motion);
        ref_close(&f);
    }

    {
        RecordEmitFix f;
        const char *got;

        ref_open(&f, SAG_MODE_W);
        add_event(&f, "ed.edit.yank", 1U, false, 0, NULL, 0U,
                  SAG_MODE_W, SAG_SRC_KEY);
        got = emit(&f);
        SAG_ASSERT(strstr(got, " w yank") == NULL);
        ref_close(&f);
    }
}

void test_record_emit_is_byte_deterministic(void)
{
    RecordEmitFix f;
    Bytebuf second;

    ref_open(&f, SAG_MODE_W);
    add_event(&f, "ed.move.unit.next", 1U, false, 0, NULL, 0U,
              SAG_MODE_W, SAG_SRC_MOUSE);
    add_event(&f, "ed.edit.insert.text", 1U, false, 0, "same", 4U,
              SAG_MODE_I, SAG_SRC_FLETCH);
    sag_record_emit(&f.rec, &f.ed, &f.out);
    bytebuf_init(&second);
    sag_record_emit(&f.rec, &f.ed, &second);
    SAG_ASSERT_EQ_U64(f.out.len, second.len);
    SAG_ASSERT_EQ_MEM(f.out.data, second.data, f.out.len);
    bytebuf_free(&second);
    ref_close(&f);
}

void test_record_words_are_a_registry_bijection(void)
{
    u32 i;
    u32 checked = 0U;

    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);

        if ((desc->flags & SAG_CMD_RECORDABLE) == 0U)
            continue;
        SAG_ASSERT_NOT_NULL(desc->word);
        SAG_ASSERT(desc->word[0] != '\0');
        SAG_ASSERT_EQ_U64(sag_cmd_by_word(desc->word,
                                          (u32)strlen(desc->word)).v,
                          i + 1U);
        checked++;
    }
    SAG_ASSERT(checked >= 80U);
}
