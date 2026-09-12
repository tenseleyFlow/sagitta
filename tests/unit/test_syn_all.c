#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syn/defs.h"
#include "syn/engine.h"
#include "syn/langs_gen.h"
#include "text/piece.h"

typedef struct PackCase {
    const char *language;
    const char *path;
} PackCase;

static const PackCase all_packs[] = {
    {"c", "tests/syn/c/01-kitchen.c"},
    {"cmake", "tests/syn/cmake/01-kitchen.cmake"},
    {"cpp", "tests/syn/cpp/01-kitchen.cpp"},
    {"csharp", "tests/syn/csharp/01-kitchen.cs"},
    {"css", "tests/syn/css/01-standalone.css"},
    {"dart", "tests/syn/dart/01-kitchen.dart"},
    {"diff", "tests/syn/diff/01-unified.diff"},
    {"dockerfile", "tests/syn/dockerfile/01-kitchen.Dockerfile"},
    {"fish", "tests/syn/fish/01-kitchen.fish"},
    {"fletch", "tests/syn/fletch/01-spec-14.fl"},
    {"fortran", "tests/syn/fortran/01-kitchen.f90"},
    {"fortran-fixed", "tests/syn/fortran/10-kitchen.f"},
    {"go", "tests/syn/go/01-kitchen.go"},
    {"graphql", "tests/syn/graphql/01-kitchen.graphql"},
    {"haskell", "tests/syn/haskell/01-kitchen.hs"},
    {"hcl", "tests/syn/hcl/01-kitchen.tf"},
    {"html", "tests/syn/html/01-standalone.html"},
    {"ini", "tests/syn/ini/01-basics.ini"},
    {"java", "tests/syn/java/01-kitchen.java"},
    {"javascript", "tests/syn/javascript/01-kitchen.js"},
    {"json", "tests/syn/json/01-kitchen.json"},
    {"jsonc", "tests/syn/json/10-kitchen.jsonc"},
    {"julia", "tests/syn/julia/01-kitchen.jl"},
    {"kotlin", "tests/syn/kotlin/01-kitchen.kt"},
    {"lua", "tests/syn/lua/01-kitchen.lua"},
    {"make", "tests/syn/make/01-comments.mk"},
    {"markdown", "tests/syn/markdown/01-atx.md"},
    {"meson", "tests/syn/meson/01-kitchen.build"},
    {"nix", "tests/syn/nix/01-kitchen.nix"},
    {"objective-c", "tests/syn/objective_c/01-kitchen.m"},
    {"ocaml", "tests/syn/ocaml/01-kitchen.ml"},
    {"perl", "tests/syn/perl/01-kitchen.pl"},
    {"powershell", "tests/syn/powershell/01-kitchen.ps1"},
    {"protobuf", "tests/syn/protobuf/01-kitchen.proto"},
    {"python", "tests/syn/python/01-kitchen.py"},
    {"r", "tests/syn/r/01-kitchen.r"},
    {"ruby", "tests/syn/ruby/01-kitchen.rb"},
    {"rust", "tests/syn/rust/01-kitchen.rs"},
    {"sh", "tests/syn/sh/01-comments.sh"},
    {"sql", "tests/syn/sql/01-kitchen.sql"},
    {"swift", "tests/syn/swift/01-kitchen.swift"},
    {"toml", "tests/syn/toml/01-kitchen.toml"},
    {"typescript", "tests/syn/javascript/10-kitchen.ts"},
    {"wolf", "tests/syn/wolf/01-kitchen.lu"},
    {"xml", "tests/syn/xml/01-kitchen.xml"},
    {"yaml", "tests/syn/yaml/01-kitchen.yml"},
    {"zig", "tests/syn/zig/01-kitchen.zig"},
    {"zsh", "tests/syn/zsh/01-kitchen.zsh"},
};

static bool pack_def_can_embed(const SynDef *def)
{
    u32 rule;

    for (rule = 0U; rule < def->nrules; rule++)
        if (def->rules[rule].embed.lang_kind != SYN_EMBED_LANG_NONE)
            return true;
    return false;
}

static void pack_validate_states(SynEngine *engine)
{
    const SynDef *root = yew_syn_engine_def(engine);
    SynStateTab *tab = yew_syn_engine_states(engine);
    const SynFrame zero = {0U, 0U, 0U};
    bool can_embed = pack_def_can_embed(root);
    u32 state_count = yew_syn_state_count(tab);
    u32 id;

    for (id = YEW_SYN_STATE_ROOT; id < state_count; id++) {
        const SynState *state = yew_syn_state_get(tab, id);
        u8 frame;
        u8 keep;
        u8 slot;

        YEW_ASSERT_NOT_NULL(state);
        YEW_ASSERT(state->depth >= 1U &&
                   state->depth <= YEW_SYN_DEPTH_MAX);
        YEW_ASSERT(state->ndef >= 1U && state->ndef <= YEW_SYN_DEF_MAX);
        for (frame = 0U; frame < state->depth; frame++) {
            YEW_ASSERT(state->f[frame].def < state->ndef);
            YEW_ASSERT_NOT_NULL(yew_syn_engine_def_at(
                engine, state->f[frame].def));
            if (!can_embed)
                YEW_ASSERT_EQ_U64(state->f[frame].def, 0U);
        }
        for (; frame < YEW_SYN_DEPTH_MAX; frame++)
            YEW_ASSERT_EQ_MEM(&state->f[frame], &zero, sizeof(zero));
        keep = state->ndef;
        if (((state->flags & YEW_SYN_F_EMBED_PEND) != 0U ||
             (state->f[state->depth - 1U].fl & YEW_SYN_FR_DEFER) != 0U) &&
            keep < YEW_SYN_DEF_MAX)
            keep++;
        for (slot = keep; slot < YEW_SYN_DEF_MAX; slot++)
            YEW_ASSERT_EQ_U64(state->aux[slot], 0U);
        if (!can_embed)
            YEW_ASSERT_EQ_U64(state->ndef, 1U);
    }
}

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
    bool can_embed = pack_def_can_embed(yew_syn_engine_def(engine));

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
        if (!can_embed) {
            YEW_ASSERT_EQ_U64(entry->ndef, 1U);
            YEW_ASSERT_EQ_U64(exit->ndef, 1U);
            for (u8 depth = 0U; depth < entry->depth; depth++)
                YEW_ASSERT_EQ_U64(entry->f[depth].def, 0U);
            for (u8 depth = 0U; depth < exit->depth; depth++)
                YEW_ASSERT_EQ_U64(exit->f[depth].def, 0U);
        }
    }
}

static void pack_run(const PackCase *pack, u64 seed, u32 edits)
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

    for (edit = 0U; edit < edits; edit++) {
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
    pack_validate_states(engine);
    yew_syn_detach(&fresh);
    yew_syn_detach(&incremental);
    yew_syn_engine_free(engine);
    yew_textbuf_free(tb);
    free(data);
}

void test_syn_all_48_definitions_depth_cap_and_firstbyte_sets(void)
{
    size_t language;

    _Static_assert(YEW_ARRAY_LEN(all_packs) == 48U,
                   "audit matrix must cover all 48 syntax definitions");
    YEW_ASSERT_EQ_U64(yew_syn_builtin_langs_len, YEW_ARRAY_LEN(all_packs));
    for (language = 0U; language < yew_syn_builtin_langs_len; language++) {
        size_t candidate;
        u32 matches = 0U;

        for (candidate = 0U; candidate < YEW_ARRAY_LEN(all_packs);
             candidate++)
            if (strcmp(yew_syn_builtin_langs[language].name,
                       all_packs[candidate].language) == 0)
                matches++;
        YEW_ASSERT_EQ_U64(matches, 1U);
    }
    for (language = 0U; language < YEW_ARRAY_LEN(all_packs); language++) {
        const SynDef *def = yew_syn_def_for(
            yew_syn_lang_named(all_packs[language].language));
        SynEngine *engine;
        SynStateTab *tab;
        const SynState *root;
        SynState state;
        u32 entry;
        u32 push;

        YEW_ASSERT_NOT_NULL(def);
        YEW_ASSERT_EQ_STR(def->name, all_packs[language].language);
        YEW_ASSERT(yew_syn_def_firstbyte_check(def, NULL, NULL));
        engine = yew_syn_engine_new((SynDef *)def);
        YEW_ASSERT_NOT_NULL(engine);
        tab = yew_syn_engine_states(engine);
        root = yew_syn_state_get(tab, YEW_SYN_STATE_ROOT);
        YEW_ASSERT_NOT_NULL(root);
        state = *root;
        entry = yew_syn_state_intern(tab, &state);
        YEW_ASSERT_EQ_U64(entry, YEW_SYN_STATE_ROOT);
        for (push = 0U; push < 40U; push++)
            yew_syn_state_push(&state, (u16)(push + 1U));
        for (push = 0U; push < 40U; push++)
            yew_syn_state_pop(&state, 1U);
        YEW_ASSERT_EQ_U64(yew_syn_state_intern(tab, &state), entry);
        YEW_ASSERT_EQ_MEM(&state, root, sizeof(state));
        pack_validate_states(engine);
        yew_syn_engine_free(engine);
    }
}

void test_syn_all_48_modes_four_seeds_100k_edits_audit(void)
{
    static const u64 seeds[] = {
        UINT64_C(0x58a1100000000001),
        UINT64_C(0x58a1100000000002),
        UINT64_C(0x58a1100000000003),
        UINT64_C(0x58a1100000000004),
    };
    const char *enabled = getenv("YEW_SYN_AUDIT_ALL48");
    size_t language;
    size_t seed;

    if (enabled == NULL || strcmp(enabled, "1") != 0) {
        YEW_ASSERT(true);
        return;
    }
    for (language = 0U; language < YEW_ARRAY_LEN(all_packs); language++) {
        (void)fprintf(stderr, "syn audit: %zu/48 %s\n", language + 1U,
                      all_packs[language].language);
        for (seed = 0U; seed < YEW_ARRAY_LEN(seeds); seed++)
            pack_run(&all_packs[language], seeds[seed], 100000U);
    }
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
            pack_run(&packs[language], seeds[seed], 100000U);
}

void test_syn_all_new_pack_two_seeds_2000_edits(void)
{
    static const PackCase packs[] = {
        {"wolf", "tests/syn/wolf/01-kitchen.lu"},
        {"cpp", "tests/syn/cpp/01-kitchen.cpp"},
        {"objective-c", "tests/syn/objective_c/01-kitchen.m"},
        {"java", "tests/syn/java/01-kitchen.java"},
        {"kotlin", "tests/syn/kotlin/01-kitchen.kt"},
        {"csharp", "tests/syn/csharp/01-kitchen.cs"},
        {"swift", "tests/syn/swift/01-kitchen.swift"},
        {"zig", "tests/syn/zig/01-kitchen.zig"},
        {"lua", "tests/syn/lua/01-kitchen.lua"},
        {"ruby", "tests/syn/ruby/01-kitchen.rb"},
        {"perl", "tests/syn/perl/01-kitchen.pl"},
        {"r", "tests/syn/r/01-kitchen.r"},
        {"julia", "tests/syn/julia/01-kitchen.jl"},
        {"dart", "tests/syn/dart/01-kitchen.dart"},
        {"powershell", "tests/syn/powershell/01-kitchen.ps1"},
        {"zsh", "tests/syn/zsh/01-kitchen.zsh"},
        {"fish", "tests/syn/fish/01-kitchen.fish"},
        {"sql", "tests/syn/sql/01-kitchen.sql"},
        {"nix", "tests/syn/nix/01-kitchen.nix"},
        {"haskell", "tests/syn/haskell/01-kitchen.hs"},
        {"ocaml", "tests/syn/ocaml/01-kitchen.ml"},
        {"xml", "tests/syn/xml/01-kitchen.xml"},
        {"graphql", "tests/syn/graphql/01-kitchen.graphql"},
        {"protobuf", "tests/syn/protobuf/01-kitchen.proto"},
        {"hcl", "tests/syn/hcl/01-kitchen.tf"},
        {"dockerfile", "tests/syn/dockerfile/01-kitchen.Dockerfile"},
        {"cmake", "tests/syn/cmake/01-kitchen.cmake"},
        {"meson", "tests/syn/meson/01-kitchen.build"},
        {"diff", "tests/syn/diff/01-unified.diff"},
    };
    static const u64 seeds[] = {
        UINT64_C(0x4250000000000001),
        UINT64_C(0x4250000000000002),
    };
    size_t language;
    size_t seed;

    for (language = 0U; language < YEW_ARRAY_LEN(packs); language++)
        for (seed = 0U; seed < YEW_ARRAY_LEN(seeds); seed++)
            pack_run(&packs[language], seeds[seed], 2000U);
}

void test_syn_all_state_heavy_four_seeds_25000_edits(void)
{
    static const PackCase packs[] = {
        {"wolf", "tests/syn/wolf/01-kitchen.lu"},
        {"cpp", "tests/syn/cpp/01-kitchen.cpp"},
        {"kotlin", "tests/syn/kotlin/01-kitchen.kt"},
        {"csharp", "tests/syn/csharp/01-kitchen.cs"},
        {"swift", "tests/syn/swift/01-kitchen.swift"},
        {"lua", "tests/syn/lua/01-kitchen.lua"},
        {"ruby", "tests/syn/ruby/01-kitchen.rb"},
        {"haskell", "tests/syn/haskell/01-kitchen.hs"},
        {"hcl", "tests/syn/hcl/01-kitchen.tf"},
    };
    static const u64 seeds[] = {
        UINT64_C(0x4250a11ce0000001),
        UINT64_C(0x4250a11ce0000002),
        UINT64_C(0x4250a11ce0000003),
        UINT64_C(0x4250a11ce0000004),
    };
    size_t language;
    size_t seed;

    for (language = 0U; language < YEW_ARRAY_LEN(packs); language++)
        for (seed = 0U; seed < YEW_ARRAY_LEN(seeds); seed++)
            pack_run(&packs[language], seeds[seed], 25000U);
}

void test_syn_all_new_pack_long_sanitizer_lane(void)
{
    static const PackCase heavy[] = {
        {"cpp", "tests/syn/cpp/01-kitchen.cpp"},
        {"kotlin", "tests/syn/kotlin/01-kitchen.kt"},
        {"ruby", "tests/syn/ruby/01-kitchen.rb"},
        {"perl", "tests/syn/perl/01-kitchen.pl"},
        {"powershell", "tests/syn/powershell/01-kitchen.ps1"},
        {"haskell", "tests/syn/haskell/01-kitchen.hs"},
        {"xml", "tests/syn/xml/01-kitchen.xml"},
        {"hcl", "tests/syn/hcl/01-kitchen.tf"},
    };
    const char *enabled = getenv("YEW_SYN_PACK_LONG");
    const char *rotation = getenv("YEW_SYN_PACK_ROTATE");
    unsigned long selected = 0UL;
    char *end = NULL;
    size_t seed;
    static const PackCase wolf = {
        "wolf", "tests/syn/wolf/01-kitchen.lu"
    };
    static const u64 seeds[] = {
        UINT64_C(0x4250100000000001),
        UINT64_C(0x4250100000000002),
        UINT64_C(0x4250100000000003),
        UINT64_C(0x4250100000000004),
    };

    if (enabled == NULL || strcmp(enabled, "1") != 0) {
        YEW_ASSERT(true);
        return;
    }
    if (rotation != NULL) {
        selected = strtoul(rotation, &end, 10);
        YEW_ASSERT(end != rotation && *end == '\0');
    }
    selected %= YEW_ARRAY_LEN(heavy);
    for (seed = 0U; seed < YEW_ARRAY_LEN(seeds); seed++) {
        pack_run(&wolf, seeds[seed], 100000U);
        pack_run(&heavy[selected], seeds[seed], 100000U);
    }
}
