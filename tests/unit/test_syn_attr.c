#include "harness.h"

#include "syn/attr.h"

void test_syn_attr_parent_covers_closed_hierarchy_and_invalid_ids(void)
{
    static const u8 want[YEW_ATTR__COUNT] = {
        YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_KEYWORD, YEW_ATTR_KEYWORD,
        YEW_ATTR_KEYWORD, YEW_ATTR_TEXT, YEW_ATTR_TYPE, YEW_ATTR_TEXT,
        YEW_ATTR_CONSTANT, YEW_ATTR_CONSTANT, YEW_ATTR_CONSTANT,
        YEW_ATTR_STRING, YEW_ATTR_TEXT, YEW_ATTR_STRING, YEW_ATTR_STRING,
        YEW_ATTR_STRING, YEW_ATTR_TEXT, YEW_ATTR_COMMENT, YEW_ATTR_COMMENT,
        YEW_ATTR_TEXT, YEW_ATTR_FUNCTION, YEW_ATTR_FUNCTION,
        YEW_ATTR_FUNCTION, YEW_ATTR_TEXT, YEW_ATTR_VARIABLE,
        YEW_ATTR_VARIABLE, YEW_ATTR_VARIABLE, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
        YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
        YEW_ATTR_PUNCT, YEW_ATTR_PUNCT, YEW_ATTR_TEXT, YEW_ATTR_TAG,
        YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
        YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
        YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT, YEW_ATTR_TEXT,
        YEW_ATTR_KEYWORD, YEW_ATTR_OPERATOR, YEW_ATTR_NUMBER,
        YEW_ATTR_FUNCTION, YEW_ATTR_TEXT
    };
    u32 attr;

    for (attr = 0U; attr < YEW_ATTR__COUNT; attr++)
        YEW_ASSERT_EQ_U64(yew_syn_attr_parent((u8)attr), want[attr]);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent((u8)YEW_ATTR__COUNT),
                      YEW_ATTR_TEXT);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(UINT8_MAX), YEW_ATTR_TEXT);

    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_COMMENT_DOC),
                      YEW_ATTR_COMMENT);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_COMMENT_TODO),
                      YEW_ATTR_COMMENT);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_STRING_ESCAPE),
                      YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_STRING_INTERP),
                      YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_STRING_SPECIAL),
                      YEW_ATTR_STRING);
    YEW_ASSERT_EQ_U64(yew_syn_attr_parent(YEW_ATTR_CHARACTER),
                      YEW_ATTR_STRING);
}
