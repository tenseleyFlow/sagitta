#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "term/input.h"

typedef struct {
    u32 code;
    u16 kind;
    u16 mods;
    u16 col;
    u16 row;
    u8 ev;
    u8 button;
    u8 ntext;
    u8 text[16];
} Expected;

typedef struct {
    Key *event;
    size_t len;
    size_t cap;
    Bytebuf paste;
    size_t *paste_chunk;
    size_t paste_chunks;
    size_t paste_chunk_cap;
    u32 dropped;
} Decoded;

static size_t fixture_vectors;

static void decoded_init(Decoded *d)
{
    (void)memset(d, 0, sizeof(*d));
    bytebuf_init(&d->paste);
}

static void decoded_free(Decoded *d)
{
    free(d->event);
    free(d->paste_chunk);
    bytebuf_free(&d->paste);
}

static void decoded_push(Decoded *d, const Key *key, const In *in)
{
    if (d->len == d->cap) {
        size_t cap = d->cap == 0U ? 16U : d->cap * 2U;

        d->event = sag_xreallocarray(d->event, cap, sizeof(*d->event));
        d->cap = cap;
    }
    d->event[d->len++] = *key;
    if (key->kind == SAG_EV_PASTE_DATA) {
        size_t len;
        const u8 *chunk = sag_input_paste_chunk(in, &len);

        SAG_ASSERT(len <= 4096U);
        if (d->paste_chunks == d->paste_chunk_cap) {
            size_t cap = d->paste_chunk_cap == 0U
                             ? 4U : d->paste_chunk_cap * 2U;

            d->paste_chunk = sag_xreallocarray(d->paste_chunk, cap,
                                               sizeof(*d->paste_chunk));
            d->paste_chunk_cap = cap;
        }
        d->paste_chunk[d->paste_chunks++] = len;
        bytebuf_append(&d->paste, chunk, len);
    }
}

static void drain(In *in, i64 now_ms, Decoded *d)
{
    Key key;
    size_t guard = 0U;

    while (sag_input_next(in, now_ms, &key)) {
        decoded_push(d, &key, in);
        guard++;
        SAG_ASSERT(guard <= in->buf.len + 2U);
    }
}

static void decode_parts(const u8 *bytes, size_t len, size_t split,
                         bool dribble, bool kitty, Decoded *d)
{
    TtyCaps caps = {0};
    In in;
    size_t pos = 0U;

    caps.kitty_kbd = kitty;
    sag_input_init(&in, &caps);
    if (dribble) {
        while (pos < len) {
            sag_input_feed(&in, bytes + pos, 1U);
            drain(&in, 1000 + (i64)pos, d);
            pos++;
        }
    } else {
        sag_input_feed(&in, bytes, split);
        drain(&in, 1000, d);
        sag_input_feed(&in, bytes + split, len - split);
        drain(&in, 1001, d);
    }
    sag_input_eof(&in);
    drain(&in, 2000, d);
    d->dropped = in.dropped;
    sag_input_free(&in);
}

static void assert_key(const Key *actual, const Expected *expected)
{
    SAG_ASSERT_EQ_U64(actual->code, expected->code);
    SAG_ASSERT_EQ_U64(actual->kind, expected->kind);
    SAG_ASSERT_EQ_U64(actual->mods, expected->mods);
    SAG_ASSERT_EQ_U64(actual->col, expected->col);
    SAG_ASSERT_EQ_U64(actual->row, expected->row);
    SAG_ASSERT_EQ_U64(actual->ev, expected->ev);
    SAG_ASSERT_EQ_U64(actual->button, expected->button);
    SAG_ASSERT_EQ_U64(actual->ntext, expected->ntext);
    SAG_ASSERT_EQ_MEM(actual->text, expected->text, expected->ntext);
}

static void assert_decoded_equal(const Decoded *a, const Decoded *b)
{
    size_t i;

    SAG_ASSERT_EQ_U64(a->len, b->len);
    SAG_ASSERT_EQ_U64(a->dropped, b->dropped);
    SAG_ASSERT_EQ_U64(a->paste.len, b->paste.len);
    SAG_ASSERT_EQ_U64(a->paste_chunks, b->paste_chunks);
    SAG_ASSERT_EQ_MEM(a->paste.data, b->paste.data, a->paste.len);
    SAG_ASSERT_EQ_MEM(a->paste_chunk, b->paste_chunk,
                      a->paste_chunks * sizeof(*a->paste_chunk));
    for (i = 0U; i < a->len; i++)
        SAG_ASSERT_EQ_MEM(&a->event[i], &b->event[i], sizeof(Key));
}

static void check_vector(const u8 *bytes, size_t len,
                         const Expected *expected, bool kitty)
{
    Decoded baseline;
    size_t split;

    decoded_init(&baseline);
    decode_parts(bytes, len, len, false, kitty, &baseline);
    SAG_ASSERT_EQ_U64(baseline.len, 1U);
    assert_key(&baseline.event[0], expected);
    for (split = 0U; split <= len; split++) {
        Decoded actual;

        decoded_init(&actual);
        decode_parts(bytes, len, split, false, kitty, &actual);
        assert_decoded_equal(&baseline, &actual);
        decoded_free(&actual);
    }
    {
        Decoded actual;

        decoded_init(&actual);
        decode_parts(bytes, len, 0U, true, kitty, &actual);
        assert_decoded_equal(&baseline, &actual);
        decoded_free(&actual);
    }
    decoded_free(&baseline);
    fixture_vectors++;
}

static Expected named(u32 code, u16 mods)
{
    Expected e = {0};

    e.code = code;
    e.kind = SAG_EV_KEY;
    e.mods = mods;
    e.ev = SAG_KEY_PRESS;
    return e;
}

static Expected text_key(u32 code, u16 mods, const u8 *text, size_t len)
{
    Expected e = named(code, mods);

    e.ntext = (u8)len;
    (void)memcpy(e.text, text, len);
    return e;
}

void test_input_legacy_bytes(void)
{
    unsigned int b;

    for (b = 0U; b < 128U; b++) {
        u8 byte = (u8)b;
        Expected e;

        if (b == 0x1BU) {
            e = named(SAG_KEY_ESCAPE, 0U);
        } else if (b == 0U) {
            e = named(' ', SAG_MOD_CTRL);
        } else if (b <= 7U) {
            e = named('a' + b - 1U, SAG_MOD_CTRL);
        } else if (b == 8U) {
            e = named(SAG_KEY_BACKSPACE, SAG_MOD_CTRL);
        } else if (b == 9U) {
            e = named(SAG_KEY_TAB, 0U);
        } else if (b >= 10U && b <= 12U) {
            e = named('a' + b - 1U, SAG_MOD_CTRL);
        } else if (b == 13U) {
            e = named(SAG_KEY_ENTER, 0U);
        } else if (b <= 26U) {
            e = named('a' + b - 1U, SAG_MOD_CTRL);
        } else if (b <= 31U) {
            e = named('\\' + b - 28U, SAG_MOD_CTRL);
        } else if (b == 127U) {
            e = named(SAG_KEY_BACKSPACE, 0U);
        } else {
            e = text_key(b, 0U, &byte, 1U);
        }
        check_vector(&byte, 1U, &e, false);
    }
    {
        static const struct {
            u8 bytes[4];
            size_t len;
            u32 code;
        } utf8[] = {
            {{0xC3U, 0xA9U, 0U, 0U}, 2U, 0xE9U},
            {{0xE2U, 0x82U, 0xACU, 0U}, 3U, 0x20ACU},
            {{0xF0U, 0x9FU, 0x98U, 0x80U}, 4U, 0x1F600U}
        };
        size_t i;

        for (i = 0U; i < SAG_ARRAY_LEN(utf8); i++) {
            Expected e = text_key(utf8[i].code, 0U, utf8[i].bytes,
                                  utf8[i].len);

            check_vector(utf8[i].bytes, utf8[i].len, &e, false);
        }
    }
    {
        static const u8 invalid_then_valid[] = {0xC2U, 'x'};
        Decoded d;

        decoded_init(&d);
        decode_parts(invalid_then_valid, sizeof(invalid_then_valid),
                     1U, false, false, &d);
        SAG_ASSERT_EQ_U64(d.len, 2U);
        SAG_ASSERT_EQ_U64(d.event[0].kind, SAG_EV_NONE);
        SAG_ASSERT_EQ_U64(d.event[1].code, 'x');
        SAG_ASSERT_EQ_U64(d.dropped, 1U);
        decoded_free(&d);
    }
    SAG_ASSERT(fixture_vectors >= 120U);
}

void test_input_csi_keys(void)
{
    static const struct { char final; u32 code; } finals[] = {
        {'A', SAG_KEY_UP}, {'B', SAG_KEY_DOWN}, {'C', SAG_KEY_RIGHT},
        {'D', SAG_KEY_LEFT}, {'E', SAG_KEY_BEGIN}, {'F', SAG_KEY_END},
        {'H', SAG_KEY_HOME}, {'P', SAG_KEY_F1}, {'Q', SAG_KEY_F2},
        {'R', SAG_KEY_F3}, {'S', SAG_KEY_F4}
    };
    static const u16 modifier_param[] = {
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 17U, 33U, 65U, 129U
    };
    static const struct { unsigned int number; u32 code; } tilde[] = {
        {1, SAG_KEY_HOME}, {2, SAG_KEY_INSERT}, {3, SAG_KEY_DELETE},
        {4, SAG_KEY_END}, {5, SAG_KEY_PAGE_UP}, {6, SAG_KEY_PAGE_DOWN},
        {7, SAG_KEY_HOME}, {8, SAG_KEY_END},
        {11, SAG_KEY_F1}, {12, SAG_KEY_F2}, {13, SAG_KEY_F3},
        {14, SAG_KEY_F4}, {15, SAG_KEY_F5}, {17, SAG_KEY_F6},
        {18, SAG_KEY_F7}, {19, SAG_KEY_F8}, {20, SAG_KEY_F9},
        {21, SAG_KEY_F10}, {23, SAG_KEY_F11}, {24, SAG_KEY_F12},
        {25, SAG_KEY_F13}, {26, SAG_KEY_F14}, {28, SAG_KEY_F15},
        {29, SAG_KEY_F16}, {31, SAG_KEY_F17}, {32, SAG_KEY_F18},
        {33, SAG_KEY_F19}, {34, SAG_KEY_F20}
    };
    size_t i;
    char seq[32];

    for (i = 0U; i < SAG_ARRAY_LEN(finals); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%c", finals[i].final);
        Expected e = named(finals[i].code, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, false);
    }
    for (i = 0U; i < SAG_ARRAY_LEN(modifier_param); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[1;%uC",
                           modifier_param[i]);
        Expected e = named(SAG_KEY_RIGHT, modifier_param[i] - 1U);

        check_vector((const u8 *)seq, (size_t)len, &e, false);
    }
    for (i = 0U; i < SAG_ARRAY_LEN(tilde); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%u~", tilde[i].number);
        Expected e = named(tilde[i].code, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, false);
    }
    {
        static const u8 seq_z[] = "\x1b[Z";
        Expected e = named(SAG_KEY_TAB, SAG_MOD_SHIFT);

        check_vector(seq_z, sizeof(seq_z) - 1U, &e, false);
    }
}

static void assert_dropped(const u8 *bytes, size_t len)
{
    Decoded baseline;
    size_t split;

    decoded_init(&baseline);
    decode_parts(bytes, len, len, false, false, &baseline);
    SAG_ASSERT_EQ_U64(baseline.len, 1U);
    SAG_ASSERT_EQ_U64(baseline.event[0].kind, SAG_EV_NONE);
    SAG_ASSERT_EQ_U64(baseline.dropped, 1U);
    for (split = 0U; split <= len; split++) {
        Decoded actual;

        decoded_init(&actual);
        decode_parts(bytes, len, split, false, false, &actual);
        assert_decoded_equal(&baseline, &actual);
        decoded_free(&actual);
    }
    {
        Decoded actual;

        decoded_init(&actual);
        decode_parts(bytes, len, 0U, true, false, &actual);
        assert_decoded_equal(&baseline, &actual);
        decoded_free(&actual);
    }
    decoded_free(&baseline);
}

void test_input_csi_drops(void)
{
    static const unsigned int gaps[] = {9, 10, 16, 22, 27, 30};
    static const u8 cpr[] = "\x1b[12;40R";
    static const u8 late_probe[] = "\x1b[?2026;1$y";
    static const u8 malformed_priv[] = "\x1b[1?A";
    static const u8 saturated[] = "\x1b[99999999999999C";
    static const u8 colon_arrow[] = "\x1b[1:999A";
    static const u8 colon_tilde[] = "\x1b[1:999~";
    char seq[24];
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(gaps); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%u~", gaps[i]);

        assert_dropped((const u8 *)seq, (size_t)len);
    }
    assert_dropped(cpr, sizeof(cpr) - 1U);
    assert_dropped(late_probe, sizeof(late_probe) - 1U);
    assert_dropped(malformed_priv, sizeof(malformed_priv) - 1U);
    assert_dropped(saturated, sizeof(saturated) - 1U);
    assert_dropped(colon_arrow, sizeof(colon_arrow) - 1U);
    assert_dropped(colon_tilde, sizeof(colon_tilde) - 1U);
}

void test_input_ss3_keys(void)
{
    static const struct { char final; u32 code; } table[] = {
        {'A', SAG_KEY_UP}, {'B', SAG_KEY_DOWN}, {'C', SAG_KEY_RIGHT},
        {'D', SAG_KEY_LEFT}, {'E', SAG_KEY_BEGIN}, {'F', SAG_KEY_END},
        {'H', SAG_KEY_HOME}, {'P', SAG_KEY_F1}, {'Q', SAG_KEY_F2},
        {'R', SAG_KEY_F3}, {'S', SAG_KEY_F4}, {'M', SAG_KEY_KP_ENTER},
        {'X', SAG_KEY_KP_EQUAL}, {'j', SAG_KEY_KP_MULTIPLY},
        {'k', SAG_KEY_KP_ADD}, {'l', SAG_KEY_KP_SEPARATOR},
        {'m', SAG_KEY_KP_SUBTRACT}, {'n', SAG_KEY_KP_DECIMAL},
        {'o', SAG_KEY_KP_DIVIDE}, {'p', SAG_KEY_KP_0},
        {'q', SAG_KEY_KP_1}, {'r', SAG_KEY_KP_2}, {'s', SAG_KEY_KP_3},
        {'t', SAG_KEY_KP_4}, {'u', SAG_KEY_KP_5}, {'v', SAG_KEY_KP_6},
        {'w', SAG_KEY_KP_7}, {'x', SAG_KEY_KP_8}, {'y', SAG_KEY_KP_9}
    };
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(table); i++) {
        u8 seq[] = {0x1BU, 'O', (u8)table[i].final};
        Expected e = named(table[i].code, 0U);

        check_vector(seq, sizeof(seq), &e, false);
    }
}

void test_input_kitty_keys(void)
{
    static const struct { unsigned int wire; u32 code; } functional[] = {
        {57358, SAG_KEY_CAPS_LOCK}, {57359, SAG_KEY_SCROLL_LOCK},
        {57360, SAG_KEY_NUM_LOCK}, {57361, SAG_KEY_PRINT_SCREEN},
        {57362, SAG_KEY_PAUSE}, {57363, SAG_KEY_MENU},
        {57376, SAG_KEY_F13}, {57398, SAG_KEY_F35},
        {57399, SAG_KEY_KP_0}, {57408, SAG_KEY_KP_9},
        {57409, SAG_KEY_KP_DECIMAL}, {57410, SAG_KEY_KP_DIVIDE},
        {57411, SAG_KEY_KP_MULTIPLY}, {57412, SAG_KEY_KP_SUBTRACT},
        {57413, SAG_KEY_KP_ADD}, {57414, SAG_KEY_KP_ENTER},
        {57415, SAG_KEY_KP_EQUAL}, {57416, SAG_KEY_KP_SEPARATOR},
        {57417, SAG_KEY_KP_LEFT}, {57418, SAG_KEY_KP_RIGHT},
        {57419, SAG_KEY_KP_UP}, {57420, SAG_KEY_KP_DOWN},
        {57421, SAG_KEY_KP_PAGE_UP}, {57422, SAG_KEY_KP_PAGE_DOWN},
        {57423, SAG_KEY_KP_HOME}, {57424, SAG_KEY_KP_END},
        {57425, SAG_KEY_KP_INSERT}, {57426, SAG_KEY_KP_DELETE},
        {57427, SAG_KEY_KP_BEGIN}
    };
    static const struct { const char *seq; Expected e; } examples[] = {
        {"\x1b[97u", {97, SAG_EV_KEY, 0, 0, 0, SAG_KEY_PRESS, 0, 1, {'a'}}},
        {"\x1b[97:65;2u", {97, SAG_EV_KEY, SAG_MOD_SHIFT, 0, 0,
                            SAG_KEY_PRESS, 0, 1, {'A'}}},
        {"\x1b[97;5u", {97, SAG_EV_KEY, SAG_MOD_CTRL, 0, 0,
                         SAG_KEY_PRESS, 0, 0, {0}}},
        {"\x1b[27u", {SAG_KEY_ESCAPE, SAG_EV_KEY, 0, 0, 0,
                       SAG_KEY_PRESS, 0, 0, {0}}},
        {"\x1b[13u", {SAG_KEY_ENTER, SAG_EV_KEY, 0, 0, 0,
                       SAG_KEY_PRESS, 0, 0, {0}}},
        {"\x1b[9u", {SAG_KEY_TAB, SAG_EV_KEY, 0, 0, 0,
                      SAG_KEY_PRESS, 0, 0, {0}}},
        {"\x1b[127u", {SAG_KEY_BACKSPACE, SAG_EV_KEY, 0, 0, 0,
                        SAG_KEY_PRESS, 0, 0, {0}}},
        {"\x1b[233;;233u", {233, SAG_EV_KEY, 0, 0, 0, SAG_KEY_PRESS,
                             0, 2, {0xC3, 0xA9}}},
        {"\x1b[97;1:3u", {97, SAG_EV_KEY, 0, 0, 0, SAG_KEY_RELEASE,
                           0, 1, {'a'}}},
        {"\x1b[65:65:97;2;65u", {97, SAG_EV_KEY, SAG_MOD_SHIFT, 0, 0,
                                  SAG_KEY_PRESS, 0, 1, {'A'}}}
    };
    size_t i;
    char seq[32];

    for (i = 0U; i < SAG_ARRAY_LEN(examples); i++)
        check_vector((const u8 *)examples[i].seq, strlen(examples[i].seq),
                     &examples[i].e, true);
    for (i = 0U; i < SAG_ARRAY_LEN(functional); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%uu", functional[i].wire);
        Expected e = named(functional[i].code, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, true);
    }
    for (i = 0U; i < 23U; i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%uu", 57376U + (unsigned)i);
        Expected e = named(SAG_KEY_F13 + (u32)i, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, true);
    }
    for (i = 0U; i < 10U; i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%uu", 57399U + (unsigned)i);
        Expected e = named(SAG_KEY_KP_0 + (u32)i, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, true);
    }
    for (i = 0U; i < 12U; i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%uu", 57441U + (unsigned)i);
        Expected e = named(SAG_KEY_LEFT_SHIFT + (u32)i, 0U);

        check_vector((const u8 *)seq, (size_t)len, &e, true);
    }
    assert_dropped((const u8 *)"\x1b[97:55296;2u",
                   sizeof("\x1b[97:55296;2u") - 1U);
    assert_dropped((const u8 *)"\x1b[57364:65:97;2u",
                   sizeof("\x1b[57364:65:97;2u") - 1U);
    assert_dropped((const u8 *)"\x1b[97:57364:97;2u",
                   sizeof("\x1b[97:57364:97;2u") - 1U);
    assert_dropped((const u8 *)"\x1b[97;;1114112u",
                   sizeof("\x1b[97;;1114112u") - 1U);
    {
        static const u8 truncated[] =
            "\x1b[97;;97:97:97:97:97:97:97:97:97:97:97:97:97:97:97:97u";
        Expected e = named('a', 0U);

        e.ntext = 15U;
        (void)memset(e.text, 'a', e.ntext);
        check_vector(truncated, sizeof(truncated) - 1U, &e, true);
    }
}

static void assert_paste(const u8 *payload, size_t payload_len,
                         size_t split, bool dribble)
{
    static const u8 begin[] = "\x1b[200~";
    static const u8 end[] = "\x1b[201~";
    Bytebuf stream;
    Decoded d;

    bytebuf_init(&stream);
    bytebuf_append(&stream, begin, sizeof(begin) - 1U);
    bytebuf_append(&stream, payload, payload_len);
    bytebuf_append(&stream, end, sizeof(end) - 1U);
    decoded_init(&d);
    decode_parts(stream.data, stream.len, split, dribble, false, &d);
    SAG_ASSERT(d.len >= 2U);
    SAG_ASSERT_EQ_U64(d.event[0].kind, SAG_EV_PASTE_BEGIN);
    SAG_ASSERT_EQ_U64(d.event[d.len - 1U].kind, SAG_EV_PASTE_END);
    SAG_ASSERT_EQ_U64(d.paste.len, payload_len);
    SAG_ASSERT_EQ_MEM(d.paste.data, payload, payload_len);
    decoded_free(&d);
    bytebuf_free(&stream);
}

void test_input_paste_framing(void)
{
    static const u8 plain[] = "paste text";
    static const u8 binary[] = {0, 1, 0x1B, '[', 'A', 0xFF, '\n'};
    size_t split;

    assert_paste(plain, sizeof(plain) - 1U, 5U, false);
    assert_paste(binary, sizeof(binary), 0U, true);
    for (split = 0U; split <= sizeof("\x1b[200~x\x1b[201~") - 1U; split++)
        assert_paste((const u8 *)"x", 1U, split, false);
    {
        enum { PAYLOAD_LEN = 5000 };
        static const u8 begin[] = "\x1b[200~";
        static const u8 end[] = "\x1b[201~";
        u8 *stream = sag_xmalloc(sizeof(begin) - 1U + PAYLOAD_LEN +
                                 sizeof(end) - 1U);
        size_t stream_len = sizeof(begin) - 1U + PAYLOAD_LEN +
                            sizeof(end) - 1U;
        Decoded baseline;
        Decoded dribbled;

        (void)memcpy(stream, begin, sizeof(begin) - 1U);
        (void)memset(stream + sizeof(begin) - 1U, 'p', PAYLOAD_LEN);
        (void)memcpy(stream + sizeof(begin) - 1U + PAYLOAD_LEN, end,
                     sizeof(end) - 1U);
        decoded_init(&baseline);
        decode_parts(stream, stream_len, stream_len, false, false, &baseline);
        decoded_init(&dribbled);
        decode_parts(stream, stream_len, 0U, true, false, &dribbled);
        assert_decoded_equal(&baseline, &dribbled);
        SAG_ASSERT_EQ_U64(baseline.paste_chunks, 2U);
        SAG_ASSERT_EQ_U64(baseline.paste_chunk[0], 4080U);
        SAG_ASSERT_EQ_U64(baseline.paste_chunk[1], PAYLOAD_LEN - 4080U);
        decoded_free(&dribbled);
        decoded_free(&baseline);
        free(stream);
    }
    {
        static const u8 framed[] =
            "\x1b[200~a\x1b[201~b";
        Decoded d;

        decoded_init(&d);
        decode_parts(framed, sizeof(framed) - 1U,
                     sizeof(framed) - 1U, false, false, &d);
        SAG_ASSERT_EQ_U64(d.paste.len, 1U);
        SAG_ASSERT_EQ_U64(d.paste.data[0], 'a');
        SAG_ASSERT_EQ_U64(d.event[d.len - 1U].kind, SAG_EV_KEY);
        SAG_ASSERT_EQ_U64(d.event[d.len - 1U].code, 'b');
        decoded_free(&d);
    }
    {
        static const u8 unterminated[] = "\x1b[200~tail";
        Decoded d;

        decoded_init(&d);
        decode_parts(unterminated, sizeof(unterminated) - 1U,
                     sizeof(unterminated) - 1U, false, false, &d);
        SAG_ASSERT_EQ_U64(d.paste.len, 4U);
        SAG_ASSERT_EQ_MEM(d.paste.data, "tail", 4U);
        SAG_ASSERT_EQ_U64(d.event[d.len - 1U].kind, SAG_EV_PASTE_END);
        decoded_free(&d);
    }
}

void test_input_mouse_and_focus(void)
{
    static const struct { unsigned cb; char final; u8 button; u16 mods; } rows[] = {
        {0, 'M', SAG_MB_LEFT, 0}, {1, 'M', SAG_MB_MIDDLE, 0},
        {2, 'M', SAG_MB_RIGHT, 0}, {0, 'm', SAG_MB_LEFT, 0},
        {1, 'm', SAG_MB_MIDDLE, 0}, {2, 'm', SAG_MB_RIGHT, 0},
        {32, 'M', SAG_MB_LEFT, 0}, {33, 'M', SAG_MB_MIDDLE, 0},
        {34, 'M', SAG_MB_RIGHT, 0}, {64, 'M', SAG_MB_WHEEL_UP, 0},
        {65, 'M', SAG_MB_WHEEL_DOWN, 0}, {66, 'M', SAG_MB_WHEEL_LEFT, 0},
        {67, 'M', SAG_MB_WHEEL_RIGHT, 0}, {128, 'M', SAG_MB_BACK, 0},
        {129, 'M', SAG_MB_FORWARD, 0},
        {48, 'M', SAG_MB_LEFT, SAG_MOD_CTRL},
        {40, 'M', SAG_MB_LEFT, SAG_MOD_ALT},
        {36, 'M', SAG_MB_LEFT, SAG_MOD_SHIFT}
    };
    size_t i;
    char seq[32];

    for (i = 0U; i < SAG_ARRAY_LEN(rows); i++) {
        int len = snprintf(seq, sizeof(seq), "\x1b[<%u;1;1%c",
                           rows[i].cb, rows[i].final);
        Expected e = {0};

        e.kind = SAG_EV_MOUSE;
        e.mods = rows[i].mods;
        e.ev = rows[i].final == 'm' ? SAG_KEY_RELEASE :
               (rows[i].cb & 32U) != 0U && (rows[i].cb & 64U) == 0U ?
                   SAG_KEY_REPEAT : SAG_KEY_PRESS;
        e.button = rows[i].button;
        check_vector((const u8 *)seq, (size_t)len, &e, false);
    }
    {
        static const u8 focus_in[] = "\x1b[I";
        static const u8 focus_out[] = "\x1b[O";
        Expected in = named(SAG_KEY_FOCUS_IN, 0U);
        Expected out = named(SAG_KEY_FOCUS_OUT, 0U);

        in.kind = SAG_EV_FOCUS;
        out.kind = SAG_EV_FOCUS;
        check_vector(focus_in, sizeof(focus_in) - 1U, &in, false);
        check_vector(focus_out, sizeof(focus_out) - 1U, &out, false);
    }
    {
        const u8 x10[] = {0x1B, '[', 'M', 32, 33, 33};
        Expected e = {0};

        e.kind = SAG_EV_MOUSE;
        e.ev = SAG_KEY_PRESS;
        e.button = SAG_MB_LEFT;
        check_vector(x10, sizeof(x10), &e, false);
    }
    {
        static const u8 max_coord[] = "\x1b[<0;65536;65536M";
        Expected e = {0};

        e.kind = SAG_EV_MOUSE;
        e.col = UINT16_MAX;
        e.row = UINT16_MAX;
        e.ev = SAG_KEY_PRESS;
        e.button = SAG_MB_LEFT;
        check_vector(max_coord, sizeof(max_coord) - 1U, &e, false);
    }
    assert_dropped((const u8 *)"\x1b[<0;;1M",
                   sizeof("\x1b[<0;;1M") - 1U);
    assert_dropped((const u8 *)"\x1b[<0;0;1M",
                   sizeof("\x1b[<0;0;1M") - 1U);
    assert_dropped((const u8 *)"\x1b[<0;65537;1M",
                   sizeof("\x1b[<0;65537;1M") - 1U);
    assert_dropped((const u8 *)"\x1b[<0:1;1;1M",
                   sizeof("\x1b[<0:1;1;1M") - 1U);
    assert_dropped((const u8 *)"\x1b[<3;1;1M",
                   sizeof("\x1b[<3;1;1M") - 1U);
}

void test_input_chunking_independence(void)
{
    static const u8 combined[] =
        "a\x1b[A\x1bOP\x1b[97:65;2u\x1b[<0;9;4M\x1b[I";
    Decoded baseline;
    size_t split;

    decoded_init(&baseline);
    decode_parts(combined, sizeof(combined) - 1U,
                 sizeof(combined) - 1U, false, true, &baseline);
    for (split = 0U; split <= sizeof(combined) - 1U; split++) {
        Decoded actual;

        decoded_init(&actual);
        decode_parts(combined, sizeof(combined) - 1U, split,
                     false, true, &actual);
        assert_decoded_equal(&baseline, &actual);
        decoded_free(&actual);
    }
    decoded_free(&baseline);
}

void test_input_escape_deadlines(void)
{
    TtyCaps caps = {0};
    In in;
    Key key;
    static const u8 esc = 0x1B;

    sag_input_init(&in, &caps);
    sag_input_feed(&in, &esc, 1U);
    SAG_ASSERT(!sag_input_next(&in, 1000, &key));
    SAG_ASSERT_EQ_I64(sag_input_deadline(&in, 1000), SAG_ESC_TIMEOUT_MS);
    SAG_ASSERT(sag_input_next(&in, 1000 + SAG_ESC_TIMEOUT_MS, &key));
    SAG_ASSERT_EQ_U64(key.code, SAG_KEY_ESCAPE);
    sag_input_free(&in);

    sag_input_init(&in, &caps);
    sag_input_feed(&in, &esc, 1U);
    SAG_ASSERT(!sag_input_next(&in, 2000, &key));
    sag_input_feed(&in, (const u8 *)"[A", 2U);
    SAG_ASSERT(sag_input_next(&in, 2001, &key));
    SAG_ASSERT_EQ_U64(key.code, SAG_KEY_UP);
    SAG_ASSERT_EQ_I64(sag_input_deadline(&in, 2001), -1);
    sag_input_free(&in);

    sag_input_init(&in, &caps);
    sag_input_feed(&in, (const u8 *)"\x1b" "i", 2U);
    SAG_ASSERT(sag_input_next(&in, 3000, &key));
    SAG_ASSERT_EQ_U64(key.code, 'i');
    SAG_ASSERT_EQ_U64(key.mods, SAG_MOD_ALT);
    sag_input_free(&in);

    caps.kitty_kbd = true;
    sag_input_init(&in, &caps);
    sag_input_feed(&in, &esc, 1U);
    SAG_ASSERT(!sag_input_next(&in, 4000, &key));
    SAG_ASSERT_EQ_I64(sag_input_deadline(&in, 4000), -1);
    SAG_ASSERT_EQ_I64(in.deadline, 0);
    sag_input_free(&in);

    caps.kitty_kbd = false;
    sag_input_init(&in, &caps);
    sag_input_feed(&in, &esc, 1U);
    SAG_ASSERT(!sag_input_next(&in, 4500, &key));
    sag_input_feed(&in, (const u8 *)"[", 1U);
    SAG_ASSERT(!sag_input_next(&in, 4510, &key));
    SAG_ASSERT_EQ_I64(sag_input_deadline(&in, 4510), SAG_ESC_TIMEOUT_MS);
    sag_input_feed(&in, (const u8 *)"A", 1U);
    SAG_ASSERT(sag_input_next(&in, 4530, &key));
    SAG_ASSERT_EQ_U64(key.code, SAG_KEY_UP);
    SAG_ASSERT_EQ_I64(sag_input_deadline(&in, 4530), -1);
    sag_input_free(&in);

    sag_input_init(&in, &caps);
    sag_input_feed(&in, (const u8 *)"\x1b[1;", 4U);
    SAG_ASSERT(!sag_input_next(&in, 5000, &key));
    SAG_ASSERT(sag_input_next(&in, 5000 + SAG_ESC_TIMEOUT_MS, &key));
    SAG_ASSERT_EQ_U64(key.code, SAG_KEY_ESCAPE);
    SAG_ASSERT(sag_input_next(&in, 5000 + SAG_ESC_TIMEOUT_MS, &key));
    SAG_ASSERT_EQ_U64(key.code, '[');
    SAG_ASSERT(sag_input_next(&in, 5000 + SAG_ESC_TIMEOUT_MS, &key));
    SAG_ASSERT_EQ_U64(key.code, '1');
    SAG_ASSERT(sag_input_next(&in, 5000 + SAG_ESC_TIMEOUT_MS, &key));
    SAG_ASSERT_EQ_U64(key.code, ';');
    sag_input_free(&in);
}

static void assert_pipe_blob(bool kitty, const u8 *expected, size_t len,
                             bool enable)
{
    TtyCaps caps = {0};
    u8 actual[128];
    int fd[2];
    ssize_t got;

    caps.kitty_kbd = kitty;
    SAG_ASSERT_EQ_I64(pipe(fd), 0);
    if (enable)
        sag_input_enable(fd[1], &caps);
    else
        sag_input_disable(fd[1]);
    SAG_ASSERT_EQ_I64(close(fd[1]), 0);
    got = read(fd[0], actual, sizeof(actual));
    SAG_ASSERT_EQ_I64(got, (i64)len);
    SAG_ASSERT_EQ_MEM(actual, expected, len);
    SAG_ASSERT_EQ_I64(close(fd[0]), 0);
}

void test_input_enable_blobs(void)
{
    static const u8 enable[] =
        "\x1b[?2004h\x1b[?1002h\x1b[?1006h\x1b[?1004h";
    static const u8 enable_kitty[] =
        "\x1b[?2004h\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[>21u";
    static const u8 disable[] =
        "\x1b[<u\x1b[?2004l\x1b[?1002l\x1b[?1006l\x1b[?1004l";

    assert_pipe_blob(false, enable, sizeof(enable) - 1U, true);
    assert_pipe_blob(true, enable_kitty, sizeof(enable_kitty) - 1U, true);
    assert_pipe_blob(false, disable, sizeof(disable) - 1U, false);
}

void test_input_unknown_csi(void)
{
    Bytebuf bytes;
    TtyCaps caps = {0};
    In in;
    Key key;
    size_t i;

    bytebuf_init(&bytes);
    for (i = 0U; i < 1000U; i++)
        bytebuf_printf(&bytes, "\x1b[?%zu;999z", i);
    sag_input_init(&in, &caps);
    sag_input_feed(&in, bytes.data, bytes.len);
    while (sag_input_next(&in, 1, &key))
        SAG_ASSERT_EQ_U64(key.kind, SAG_EV_NONE);
    SAG_ASSERT_EQ_U64(in.dropped, 1000U);
    sag_input_free(&in);
    bytebuf_free(&bytes);
}

void test_input_random_progress(void)
{
    u8 *bytes = sag_xmalloc(65536U);
    TtyCaps caps = {0};
    In in;
    Key key;
    u32 x = 1U;
    size_t i;
    size_t iterations = 0U;

    for (i = 0U; i < 65536U; i++) {
        x = x * 1664525U + 1013904223U;
        bytes[i] = (u8)(x >> 24);
    }
    sag_input_init(&in, &caps);
    sag_input_feed(&in, bytes, 65536U);
    while (sag_input_next(&in, (i64)iterations, &key)) {
        iterations++;
        SAG_ASSERT(iterations <= 65536U);
    }
    sag_input_eof(&in);
    while (sag_input_next(&in, 100000, &key)) {
        iterations++;
        SAG_ASSERT(iterations <= 65536U);
    }
    SAG_ASSERT(iterations <= 65536U);
    sag_input_free(&in);
    free(bytes);
}

void test_input_seed_and_cap(void)
{
    static const u8 pending_bytes[] = "x\x1b[A";
    Bytebuf pending;
    TtyCaps caps = {0};
    In in;
    Key key;
    u8 *oversized;

    bytebuf_init(&pending);
    bytebuf_append(&pending, pending_bytes, sizeof(pending_bytes) - 1U);
    sag_input_init(&in, &caps);
    sag_input_seed(&in, &pending);
    SAG_ASSERT(sag_input_next(&in, 1, &key));
    SAG_ASSERT_EQ_U64(key.code, 'x');
    SAG_ASSERT(sag_input_next(&in, 1, &key));
    SAG_ASSERT_EQ_U64(key.code, SAG_KEY_UP);
    SAG_ASSERT_EQ_U64(pending.len, sizeof(pending_bytes) - 1U);
    SAG_ASSERT_EQ_MEM(pending.data, pending_bytes, pending.len);
    sag_input_free(&in);
    bytebuf_free(&pending);

    oversized = sag_xmalloc(SAG_IN_MAX_BUFFER + 1U);
    (void)memset(oversized, 'q', SAG_IN_MAX_BUFFER + 1U);
    sag_input_init(&in, &caps);
    sag_input_feed(&in, oversized, SAG_IN_MAX_BUFFER + 1U);
    SAG_ASSERT_EQ_U64(in.buf.len - in.rd, SAG_IN_MAX_BUFFER);
    SAG_ASSERT_EQ_U64(in.dropped, 1U);
    sag_input_free(&in);
    free(oversized);
}

void test_input_large_streaming_paste(void)
{
    enum { PASTE_SIZE = 5 * 1024 * 1024 };
    static const u8 begin[] = "\x1b[200~";
    static const u8 end[] = "\x1b[201~";
    u8 block[4096];
    TtyCaps caps = {0};
    In in;
    Key key;
    size_t total = 0U;
    size_t chunks = 0U;
    size_t fed = 0U;
    size_t peak = 0U;

    (void)memset(block, 0xA5, sizeof(block));
    sag_input_init(&in, &caps);
    sag_input_feed(&in, begin, sizeof(begin) - 1U);
    SAG_ASSERT(sag_input_next(&in, 1, &key));
    SAG_ASSERT_EQ_U64(key.kind, SAG_EV_PASTE_BEGIN);
    while (fed < PASTE_SIZE) {
        size_t n = PASTE_SIZE - fed;

        if (n > sizeof(block))
            n = sizeof(block);
        sag_input_feed(&in, block, n);
        fed += n;
        while (sag_input_next(&in, (i64)fed, &key)) {
            if (key.kind == SAG_EV_PASTE_DATA) {
                size_t chunk_len;
                const u8 *chunk = sag_input_paste_chunk(&in, &chunk_len);

                SAG_ASSERT(chunk_len <= 4096U);
                if (chunk_len != 0U)
                    SAG_ASSERT_EQ_U64(chunk[0], 0xA5U);
                total += chunk_len;
                chunks++;
            }
        }
        if (in.buf.cap + in.paste.cap > peak)
            peak = in.buf.cap + in.paste.cap;
        SAG_ASSERT(peak <= 3U * SAG_IN_PASTE_CHUNK);
    }
    sag_input_feed(&in, end, sizeof(end) - 1U);
    while (sag_input_next(&in, 999999, &key)) {
        if (key.kind == SAG_EV_PASTE_DATA) {
            size_t chunk_len;

            (void)sag_input_paste_chunk(&in, &chunk_len);
            total += chunk_len;
            chunks++;
        } else {
            SAG_ASSERT_EQ_U64(key.kind, SAG_EV_PASTE_END);
        }
    }
    SAG_ASSERT_EQ_U64(total, PASTE_SIZE);
    SAG_ASSERT(chunks >= (size_t)PASTE_SIZE / 4096U);
    SAG_ASSERT(peak <= 3U * SAG_IN_PASTE_CHUNK);
    sag_input_free(&in);
}

void test_input_hostile_strings(void)
{
    static const u8 stream[] =
        "\x1b]title\x07"
        "\x1bPignored\x1b\\"
        "\x1b_apc\x1b\\"
        "\x1b^pm\x1b\\"
        "\x1bXsos\x1b\\x";
    Decoded d;

    decoded_init(&d);
    decode_parts(stream, sizeof(stream) - 1U, 7U, false, false, &d);
    SAG_ASSERT_EQ_U64(d.len, 1U);
    SAG_ASSERT_EQ_U64(d.event[0].code, 'x');
    SAG_ASSERT_EQ_U64(d.dropped, 5U);
    decoded_free(&d);

    {
        size_t body_len = SAG_IN_STRING_MAX + 1024U;
        u8 *body = sag_xmalloc(body_len + 6U);

        body[0] = 0x1BU;
        body[1] = ']';
        (void)memset(body + 2U, 'q', body_len);
        body[body_len + 2U] = 0x1BU;
        body[body_len + 3U] = '\\';
        body[body_len + 4U] = 'x';
        body[body_len + 5U] = 0U;
        decoded_init(&d);
        decode_parts(body, body_len + 5U, body_len / 2U,
                     false, false, &d);
        SAG_ASSERT_EQ_U64(d.len, 2U);
        SAG_ASSERT_EQ_U64(d.event[0].kind, SAG_EV_NONE);
        SAG_ASSERT_EQ_U64(d.event[1].code, 'x');
        decoded_free(&d);
        free(body);
    }
}
