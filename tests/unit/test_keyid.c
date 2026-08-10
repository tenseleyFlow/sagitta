#include "harness.h"

#include <string.h>

#include "edit/keymap.h"

static Key key_named(u32 code, u16 mods, u16 event)
{
    Key key = {0};

    key.code = code;
    key.kind = YEW_EV_KEY;
    key.mods = mods;
    key.ev = (u8)event;
    return key;
}

static Key key_text(u32 code, u16 mods, const char *text)
{
    Key key = key_named(code, mods, YEW_KEY_PRESS);
    size_t len = strlen(text);

    YEW_ASSERT(len <= sizeof(key.text));
    key.ntext = (u8)len;
    memcpy(key.text, text, len);
    return key;
}

void test_keyid_canonical_rows(void)
{
    Key plain = key_text((u32)'a', 0U, "a");
    Key shifted = key_text((u32)'a', YEW_MOD_SHIFT, "A");
    Key upper = key_text((u32)'A', 0U, "A");
    Key ctrl = key_text((u32)'A', YEW_MOD_CTRL | YEW_MOD_SHIFT, "A");
    Key tab = key_named(YEW_KEY_TAB, YEW_MOD_SHIFT, YEW_KEY_PRESS);
    Key left = key_named(YEW_KEY_LEFT, YEW_MOD_ALT, YEW_KEY_PRESS);
    Key repeat = plain;
    Key release = plain;

    repeat.ev = YEW_KEY_REPEAT;
    release.ev = YEW_KEY_RELEASE;
    YEW_ASSERT_EQ_U64(yew_keyid(plain).v, (u64)'a' << 16U);
    YEW_ASSERT_EQ_U64(yew_keyid(shifted).v, (u64)'A' << 16U);
    YEW_ASSERT_EQ_U64(yew_keyid(upper).v, yew_keyid(shifted).v);
    YEW_ASSERT_EQ_U64(yew_keyid(ctrl).v,
                      ((u64)'a' << 16U) | YEW_MOD_CTRL);
    YEW_ASSERT_EQ_U64(yew_keyid(tab).v,
                      ((u64)YEW_KEY_TAB << 16U) | YEW_MOD_SHIFT);
    YEW_ASSERT_EQ_U64(yew_keyid(left).v,
                      ((u64)YEW_KEY_LEFT << 16U) | YEW_MOD_ALT);
    YEW_ASSERT_EQ_U64(yew_keyid(repeat).v, yew_keyid(plain).v);
    YEW_ASSERT_EQ_U64(yew_keyid(release).v, 0U);
    YEW_ASSERT_EQ_U64(yew_keyid(key_text((u32)'2', YEW_MOD_SHIFT, "@")).v,
                      (u64)'@' << 16U);
}

void test_keyid_shift_and_legacy_equivalence(void)
{
    u32 i;

    for (i = 0U; i < 16U; i++) {
        char lower[2] = {(char)('a' + i), '\0'};
        char upper_text[2] = {(char)('A' + i), '\0'};
        Key reports_shift = key_text((u32)lower[0], YEW_MOD_SHIFT,
                                     upper_text);
        Key spends_shift = key_text((u32)upper_text[0], 0U, upper_text);

        YEW_ASSERT_EQ_U64(yew_keyid(reports_shift).v,
                          yew_keyid(spends_shift).v);
        YEW_ASSERT_EQ_U64(yew_keyid(reports_shift).v,
                          (u64)(u8)upper_text[0] << 16U);
    }
    for (i = 1U; i <= 26U; i++) {
        Key legacy = key_named(i, 0U, YEW_KEY_PRESS);
        u64 expected = ((u64)('a' + i - 1U) << 16U) | YEW_MOD_CTRL;

        YEW_ASSERT_EQ_U64(yew_keyid(legacy).v, expected);
    }
}
