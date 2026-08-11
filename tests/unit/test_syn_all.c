#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "text/piece.h"

typedef struct PackCase {
    const char *language;
    const char *path;
} PackCase;

static u64 pack_rand(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u8 *pack_read(const char *path, size_t *len)
{
    FILE *file = fopen(path, "rb");
    long end;
    u8 *data;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    end = ftell(file);
    YEW_ASSERT(end > 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    data = malloc((size_t)end);
    YEW_ASSERT_NOT_NULL(data);
    YEW_ASSERT_EQ_U64(fread(data, 1U, (size_t)end, file), (size_t)end);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    *len = (size_t)end;
    return data;
}

static void pack_settle(SynBuf *syn, const TextBuf *tb)
{
    SynSettleReport report;
    u32 calls = 0U;

    do {
        yew_syn_settle(syn, tb, LINENO(0U),
                       LINENO(yew_textbuf_line_count(tb)), INT64_MAX,
                       &report);
        YEW_ASSERT(++calls < 1000U);
    } while (!report.fixpoint);
}

static void pack_compare(SynBuf *incremental, SynBuf *fresh,
                         SynEngine *engine, const TextBuf *tb)
{
    u64 line_count = yew_textbuf_line_count(tb);
    u64 line;

    YEW_ASSERT_EQ_U64(incremental->entry.len, fresh->entry.len);
    YEW_ASSERT_EQ_MEM(incremental->entry.data, fresh->entry.data,
                      fresh->entry.len * sizeof(*fresh->entry.data));
    for (line = 0U; line < line_count; line++) {
        SynSpan a_spans[YEW_SYN_MAX_SPANS];
        SynSpan b_spans[YEW_SYN_MAX_SPANS];
        SynLineOut a = {a_spans, 0U, YEW_ARRAY_LEN(a_spans), 0U, 0U};
        SynLineOut b = {b_spans, 0U, YEW_ARRAY_LEN(b_spans), 0U, 0U};
        const SynState *entry;
        const SynState *exit;

        yew_syn_spans(incremental, tb, LINENO(line), &a);
        yew_syn_spans(fresh, tb, LINENO(line), &b);
        YEW_ASSERT_EQ_U64(a.stop, b.stop);
        YEW_ASSERT_EQ_U64(a.exit_state, b.exit_state);
        YEW_ASSERT_EQ_U64(a.n, b.n);
        YEW_ASSERT_EQ_MEM(a.spans, b.spans, a.n * sizeof(*a.spans));
        entry = yew_syn_state_get(yew_syn_engine_states(engine),
                                  incremental->entry.data[line]);
        exit = yew_syn_state_get(yew_syn_engine_states(engine),
                                 a.exit_state);
        YEW_ASSERT_NOT_NULL(entry);
        YEW_ASSERT_NOT_NULL(exit);
        YEW_ASSERT_EQ_U64(entry->ndef, 1U);
        YEW_ASSERT_EQ_U64(exit->ndef, 1U);
        for (u8 depth = 0U; depth < entry->depth; depth++)
            YEW_ASSERT_EQ_U64(entry->f[depth].def, 0U);
        for (u8 depth = 0U; depth < exit->depth; depth++)
            YEW_ASSERT_EQ_U64(exit->f[depth].def, 0U);
    }
}

static void pack_run(const PackCase *pack, u64 seed)
{
    static const u8 replacements[] =
        "abcdefghijklmnopqrstuvwxyz0123456789/*'\"`{}[]:=+-_# ";
    const SynDef *def = yew_syn_def_for(yew_syn_lang_named(pack->language));
    SynEngine *engine;
    SynBuf incremental;
    SynBuf fresh;
    TextBuf *tb;
    u8 *data;
    size_t len;
    u32 edit;

    YEW_ASSERT_NOT_NULL(def);
    data = pack_read(pack->path, &len);
    tb = yew_textbuf_from_bytes(data, len);
    YEW_ASSERT_NOT_NULL(tb);
    engine = yew_syn_engine_new((SynDef *)def);
    YEW_ASSERT_NOT_NULL(engine);
    yew_syn_buf_init(&incremental);
    yew_syn_buf_bind(&incremental, engine);
    yew_syn_attach(&incremental, 1U, tb);
    pack_settle(&incremental, tb);

    for (edit = 0U; edit < 100000U; edit++) {
        size_t at;
        u8 byte;
        LineNo line;

        do {
            at = (size_t)(pack_rand(&seed) % len);
        } while (data[at] == (u8)'\n');
        byte = replacements[pack_rand(&seed) %
                            (sizeof(replacements) - 1U)];
        line = yew_textbuf_line_of(tb, BYTEOFF(at));
        yew_textbuf_delete(tb, (Span){at, at + 1U});
        yew_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
        data[at] = byte;
        yew_syn_edit(&incremental, line, 0U, 0U);
        pack_settle(&incremental, tb);
        YEW_ASSERT_EQ_U64(incremental.entry.len,
                          yew_textbuf_line_count(tb));
        YEW_ASSERT_EQ_U64(incremental.entry.data[0], YEW_SYN_STATE_ROOT);
    }

    yew_syn_buf_init(&fresh);
    yew_syn_buf_bind(&fresh, engine);
    yew_syn_attach(&fresh, 1U, tb);
    pack_settle(&fresh, tb);
    pack_compare(&incremental, &fresh, engine, tb);
    yew_syn_detach(&fresh);
    yew_syn_detach(&incremental);
    yew_syn_engine_free(engine);
    yew_textbuf_free(tb);
    free(data);
}

void test_syn_all_eight_languages_four_seeds_100k_edits(void)
{
    static const PackCase packs[] = {
        {"python", "tests/syn/python/01-kitchen.py"},
        {"rust", "tests/syn/rust/01-kitchen.rs"},
        {"go", "tests/syn/go/01-kitchen.go"},
        {"javascript", "tests/syn/javascript/01-kitchen.js"},
        {"fortran", "tests/syn/fortran/01-kitchen.f90"},
        {"json", "tests/syn/json/01-kitchen.json"},
        {"yaml", "tests/syn/yaml/01-kitchen.yml"},
        {"toml", "tests/syn/toml/01-kitchen.toml"},
    };
    static const u64 seeds[] = {
        UINT64_C(0x123456789abcdef0),
        UINT64_C(0x0ddc0ffeebadf00d),
        UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0xfeedfacecafebeef),
    };
    u32 language;
    u32 seed;

    for (language = 0U; language < YEW_ARRAY_LEN(packs); language++)
        for (seed = 0U; seed < YEW_ARRAY_LEN(seeds); seed++)
            pack_run(&packs[language], seeds[seed]);
}
