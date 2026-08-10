#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/dispatch.h"
#include "edit/ed.h"
#include "edit/keymap.h"

static void keyseq_format(const KeyId *seq, u32 n, Bytebuf *out)
{
    bytebuf_init(out);
    yew_key_format_seq(seq, n, out);
    bytebuf_push_u8(out, 0U);
}

void test_keyseq_atom_roundtrip(void)
{
    static const char *const names[] = {
        "left", "right", "up", "down", "esc", "cr", "tab", "bs",
        "del", "space", "home", "end", "pgup", "pgdn", "ins", "f1",
        "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10",
        "f11", "f12", "lt",
    };
    static const char *const modifiers[] = {
        "", "C-", "A-", "S-", "M-", "C-A-", "C-S-", "C-M-",
        "A-S-", "A-M-", "S-M-", "C-A-S-", "C-A-M-", "C-S-M-",
        "A-S-M-", "C-A-S-M-",
    };
    size_t i;
    size_t j;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        for (j = 0U; j < YEW_ARRAY_LEN(modifiers); j++) {
            char atom[48];
            KeyId parsed[1];
            KeyId reparsed[1];
            Bytebuf formatted;
            int n = snprintf(atom, sizeof(atom), "%s<%s>", modifiers[j],
                             names[i]);

            YEW_ASSERT(n > 0 && (size_t)n < sizeof(atom));
            YEW_ASSERT_EQ_U64(yew_key_parse_seq(atom, parsed, 1U), 1U);
            keyseq_format(parsed, 1U, &formatted);
            YEW_ASSERT_EQ_STR((const char *)formatted.data, atom);
            YEW_ASSERT_EQ_U64(yew_key_parse_seq(
                                  (const char *)formatted.data, reparsed, 1U),
                              1U);
            YEW_ASSERT_EQ_U64(reparsed[0].v, parsed[0].v);
            bytebuf_free(&formatted);
        }
    }
}

void test_keyseq_printable_canonicalization_and_errors(void)
{
    static const struct {
        const char *input;
        const char *canonical;
    } valid[] = {
        {"a", "a"}, {"!", "!"}, {"4", "4"}, {"<lt>", "<lt>"},
        {"C-r", "C-r"}, {"C-A-<up>", "C-A-<up>"},
        {"C-A", "C-a"}, {"S-a", "A"}, {"S-!", "!"},
        {"g g", "g g"}, {"q !", "q !"}, {"é", "é"},
    };
    static const char *const invalid[] = {
        "", "<>", "C-", "<nope>", "A-C-a", "a  ", "<left", "left>",
        "C--a", "<f13>", "C-A-S-M-", " ",
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(valid); i++) {
        KeyId seq[YEW_CHORD_MAX];
        Bytebuf formatted;
        u32 n = yew_key_parse_seq(valid[i].input, seq, YEW_CHORD_MAX);

        YEW_ASSERT(n != 0U);
        keyseq_format(seq, n, &formatted);
        YEW_ASSERT_EQ_STR((const char *)formatted.data, valid[i].canonical);
        bytebuf_free(&formatted);
    }
    for (i = 0U; i < YEW_ARRAY_LEN(invalid); i++) {
        KeyId seq[YEW_CHORD_MAX];

        YEW_ASSERT_EQ_U64(yew_key_parse_seq(invalid[i], seq,
                                            YEW_CHORD_MAX), 0U);
    }
    {
        KeyId guarded[YEW_CHORD_MAX + 1U];
        const u64 sentinel = UINT64_C(0xa55aa55aa55aa55a);

        guarded[YEW_CHORD_MAX].v = sentinel;
        YEW_ASSERT_EQ_U64(yew_key_parse_seq("a b c d e f g h i", guarded,
                                            YEW_CHORD_MAX), 0U);
        YEW_ASSERT_EQ_U64(guarded[YEW_CHORD_MAX].v, sentinel);
    }
}

typedef struct {
    u32 rows;
} DefaultVisit;

static bool default_roundtrip(const KeyId *seq, u32 n,
                              const Binding *binding, void *opaque)
{
    DefaultVisit *visit = opaque;
    KeyId parsed[YEW_CHORD_MAX];
    Bytebuf formatted;
    u32 parsed_n;
    u32 i;

    YEW_ASSERT_NOT_NULL(binding);
    keyseq_format(seq, n, &formatted);
    parsed_n = yew_key_parse_seq((const char *)formatted.data, parsed,
                                 YEW_CHORD_MAX);
    YEW_ASSERT_EQ_U64(parsed_n, n);
    for (i = 0U; i < n; i++)
        YEW_ASSERT_EQ_U64(parsed[i].v, seq[i].v);
    bytebuf_free(&formatted);
    visit->rows++;
    return true;
}

void test_keyseq_default_tables_roundtrip(void)
{
    DefaultVisit visit = {0};
    Ed ed = {0};
    u32 i;

    yew_dispatch_init(&ed);
    for (i = 0U; i < YEW_MODE__N; i++)
        YEW_ASSERT(yew_keymap_visit(&ed.mode_keys[i], default_roundtrip,
                                    &visit));
    YEW_ASSERT(yew_keymap_visit(&ed.user_keys, default_roundtrip, &visit));
    YEW_ASSERT(visit.rows >= 17U);
    yew_dispatch_free(&ed);
}
