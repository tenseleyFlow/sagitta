#include "harness.h"

#include <string.h>

#include "edit/keymap.h"

static Key key_named(u32 code, u16 mods, u16 event)
{
    Key key = {0};

    key.code = code;
    key.kind = SAG_EV_KEY;
    key.mods = mods;
    key.ev = (u8)event;
    return key;
}

static Key key_text(u32 code, u16 mods, const char *text)
{
    Key key = key_named(code, mods, SAG_KEY_PRESS);
    size_t len = strlen(text);

    SAG_ASSERT(len <= sizeof(key.text));
    key.ntext = (u8)len;
    memcpy(key.text, text, len);
    return key;
}

void test_keyid_canonical_rows(void)
{
    Key plain = key_text((u32)'a', 0U, "a");
    Key shifted = key_text((u32)'a', SAG_MOD_SHIFT, "A");
    Key upper = key_text((u32)'A', 0U, "A");
    Key ctrl = key_text((u32)'A', SAG_MOD_CTRL | SAG_MOD_SHIFT, "A");
    Key tab = key_named(SAG_KEY_TAB, SAG_MOD_SHIFT, SAG_KEY_PRESS);
    Key left = key_named(SAG_KEY_LEFT, SAG_MOD_ALT, SAG_KEY_PRESS);
    Key repeat = plain;
    Key release = plain;

    repeat.ev = SAG_KEY_REPEAT;
    release.ev = SAG_KEY_RELEASE;
    SAG_ASSERT_EQ_U64(sag_keyid(plain).v, (u64)'a' << 16U);
    SAG_ASSERT_EQ_U64(sag_keyid(shifted).v, (u64)'A' << 16U);
    SAG_ASSERT_EQ_U64(sag_keyid(upper).v, sag_keyid(shifted).v);
    SAG_ASSERT_EQ_U64(sag_keyid(ctrl).v,
                      ((u64)'a' << 16U) | SAG_MOD_CTRL);
    SAG_ASSERT_EQ_U64(sag_keyid(tab).v,
                      ((u64)SAG_KEY_TAB << 16U) | SAG_MOD_SHIFT);
    SAG_ASSERT_EQ_U64(sag_keyid(left).v,
                      ((u64)SAG_KEY_LEFT << 16U) | SAG_MOD_ALT);
    SAG_ASSERT_EQ_U64(sag_keyid(repeat).v, sag_keyid(plain).v);
    SAG_ASSERT_EQ_U64(sag_keyid(release).v, 0U);
    SAG_ASSERT_EQ_U64(sag_keyid(key_text((u32)'2', SAG_MOD_SHIFT, "@")).v,
                      (u64)'@' << 16U);
}

void test_keyid_shift_and_legacy_equivalence(void)
{
    u32 i;

    for (i = 0U; i < 16U; i++) {
        char lower[2] = {(char)('a' + i), '\0'};
        char upper_text[2] = {(char)('A' + i), '\0'};
        Key reports_shift = key_text((u32)lower[0], SAG_MOD_SHIFT,
                                     upper_text);
        Key spends_shift = key_text((u32)upper_text[0], 0U, upper_text);

        SAG_ASSERT_EQ_U64(sag_keyid(reports_shift).v,
                          sag_keyid(spends_shift).v);
        SAG_ASSERT_EQ_U64(sag_keyid(reports_shift).v,
                          (u64)(u8)upper_text[0] << 16U);
    }
    for (i = 1U; i <= 26U; i++) {
        Key legacy = key_named(i, 0U, SAG_KEY_PRESS);
        u64 expected = ((u64)('a' + i - 1U) << 16U) | SAG_MOD_CTRL;

        SAG_ASSERT_EQ_U64(sag_keyid(legacy).v, expected);
    }
}
