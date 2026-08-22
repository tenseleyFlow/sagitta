/* Sprint 52: pure COMMIT_EDITMSG comment selection and cleanup. */
#include "harness.h"

#include "mod/git/fusscommit.h"

#define FC_BYTES(s) ((const u8 *)(s)), (sizeof(s) - 1U)

static void fc_assert_clean(const u8 *input, size_t input_len,
                            u8 comment_char, const u8 *want, size_t want_len)
{
    Bytebuf clean;
    bool nonempty;

    bytebuf_init(&clean);
    nonempty = yew_fuss_commit_cleanup(&clean, input, input_len, comment_char);
    YEW_ASSERT_EQ_U64(clean.len, want_len);
    YEW_ASSERT_EQ_MEM(clean.data, want, want_len);
    YEW_ASSERT(nonempty == (want_len != 0U));
    bytebuf_free(&clean);
}

void test_fusscommit_strips_comments_by_first_nonblank_byte(void)
{
    static const u8 input[] =
        "subject\n # template\n\t# tabbed template\nbody # retained\n  body"
        "\n\v# vertical-tab is not blank";
    static const u8 want[] =
        "subject\nbody # retained\n  body\n\v# vertical-tab is not blank";

    fc_assert_clean(input, sizeof(input) - 1U, (u8)'#',
                    want, sizeof(want) - 1U);

    fc_assert_clean(FC_BYTES("subject\r\n \t; template\r\nbody\r\n"),
                    (u8)';', FC_BYTES("subject\nbody"));
}

void test_fusscommit_trims_ascii_trailing_space_and_crlf(void)
{
    static const u8 input[] = "subject \t\v\f\r\nbody\t \r\nlast";
    static const u8 want[] = "subject\nbody\nlast";

    fc_assert_clean(input, sizeof(input) - 1U, (u8)'#',
                    want, sizeof(want) - 1U);
}

void test_fusscommit_collapses_blank_runs_and_trims_outer_blanks(void)
{
    static const u8 input[] =
        "\n \t\nsubject\n\n\t\n\nbody\n\n\t\n";
    static const u8 want[] = "subject\n\nbody";

    fc_assert_clean(input, sizeof(input) - 1U, (u8)'#',
                    want, sizeof(want) - 1U);
}

void test_fusscommit_comment_lines_do_not_create_blank_lines(void)
{
    static const u8 input[] = "subject\n# one\n  # two\nbody";
    static const u8 want[] = "subject\nbody";
    static const u8 separated[] =
        "subject\n\n# ignored\n\n\nbody\n# trailing";
    static const u8 separated_want[] = "subject\n\nbody";

    fc_assert_clean(input, sizeof(input) - 1U, (u8)'#',
                    want, sizeof(want) - 1U);
    fc_assert_clean(separated, sizeof(separated) - 1U, (u8)'#',
                    separated_want, sizeof(separated_want) - 1U);
}

void test_fusscommit_auto_uses_order_and_first_nonblank_bytes(void)
{
    static const u8 input[] = "  # heading\ntext ; retained\n\t; heading";
    static const u8 progressively_used[] = "#\n;\n@\n!\n$\n%\n^\n&\n|";
    static const u8 expected[] = ";@!$%^&|:";
    u8 selected = (u8)'?';
    size_t used;

    YEW_ASSERT(yew_fuss_commit_select_comment(
        input, sizeof(input) - 1U, FC_BYTES("auto"), &selected));
    YEW_ASSERT_EQ_U64(selected, (u8)'@');

    for (used = 1U; used <= sizeof(expected) - 1U; used++) {
        YEW_ASSERT(yew_fuss_commit_select_comment(
            progressively_used, used * 2U - 1U,
            FC_BYTES("auto"), &selected));
        YEW_ASSERT_EQ_U64(selected, expected[used - 1U]);
    }
}

void test_fusscommit_auto_ignores_candidates_inside_content(void)
{
    static const u8 input[] = "subject #1; @owner! $cash %done ^top &more | pipe:";
    u8 selected = (u8)'?';

    YEW_ASSERT(yew_fuss_commit_select_comment(
        input, sizeof(input) - 1U, FC_BYTES("auto"), &selected));
    YEW_ASSERT_EQ_U64(selected, (u8)'#');
}

void test_fusscommit_auto_reports_candidate_exhaustion(void)
{
    static const u8 input[] =
        "# a\n; b\n@ c\n! d\n$ e\n% f\n^ g\n& h\n| i\n: j";
    u8 selected = (u8)'?';

    YEW_ASSERT(!yew_fuss_commit_select_comment(
        input, sizeof(input) - 1U, FC_BYTES("auto"), &selected));
    YEW_ASSERT_EQ_U64(selected, (u8)'?');
}

void test_fusscommit_default_explicit_and_invalid_settings(void)
{
    static const u8 message[] = "message";
    u8 selected = (u8)'?';

    YEW_ASSERT(yew_fuss_commit_select_comment(
        message, sizeof(message) - 1U, NULL, 0U, &selected));
    YEW_ASSERT_EQ_U64(selected, (u8)'#');
    YEW_ASSERT(yew_fuss_commit_select_comment(
        message, sizeof(message) - 1U, FC_BYTES(";"), &selected));
    YEW_ASSERT_EQ_U64(selected, (u8)';');
    YEW_ASSERT(!yew_fuss_commit_select_comment(
        message, sizeof(message) - 1U, FC_BYTES("Auto"), &selected));
    YEW_ASSERT(!yew_fuss_commit_select_comment(
        message, sizeof(message) - 1U, FC_BYTES("\n"), &selected));
}

void test_fusscommit_preserves_embedded_nul_bytes(void)
{
    static const u8 input[] = {'a', 0U, 'b', ' ', '\n', 'c', 0U, 'd'};
    static const u8 want[] = {'a', 0U, 'b', '\n', 'c', 0U, 'd'};
    static const u8 adjacent[] = {
        0U, '#', 'x', ' ', '\t', '\n', '#', 'd', 'r', 'o', 'p', '\n',
        'z', ' ', 0U, ' ', '\t'
    };
    static const u8 adjacent_want[] = {0U, '#', 'x', '\n', 'z', ' ', 0U};

    fc_assert_clean(input, sizeof(input), (u8)'#', want, sizeof(want));
    fc_assert_clean(adjacent, sizeof(adjacent), (u8)'#',
                    adjacent_want, sizeof(adjacent_want));
}

void test_fusscommit_cleanup_is_idempotent(void)
{
    static const u8 input[] = "\nsubject  \n\n\nbody\t\n# template\n";
    Bytebuf once;
    Bytebuf twice;

    bytebuf_init(&once);
    bytebuf_init(&twice);
    YEW_ASSERT(yew_fuss_commit_cleanup(
        &once, input, sizeof(input) - 1U, (u8)'#'));
    YEW_ASSERT(yew_fuss_commit_cleanup(
        &twice, once.data, once.len, (u8)'#'));
    YEW_ASSERT_EQ_U64(twice.len, once.len);
    YEW_ASSERT_EQ_MEM(twice.data, once.data, once.len);

    bytebuf_append(&twice, "stale", sizeof("stale") - 1U);
    YEW_ASSERT(yew_fuss_commit_cleanup(
        &twice, FC_BYTES("replacement"), (u8)'#'));
    YEW_ASSERT_EQ_U64(twice.len, sizeof("replacement") - 1U);
    YEW_ASSERT_EQ_MEM(twice.data, "replacement", sizeof("replacement") - 1U);
    bytebuf_free(&twice);
    bytebuf_free(&once);
}

void test_fusscommit_empty_detection_is_byte_exact(void)
{
    static const u8 nul[] = {0U};
    static const u8 spaces[] = " \t\n";
    static const u8 comments[] = "# one\n  # two\n";
    Bytebuf clean;

    YEW_ASSERT(yew_fuss_commit_empty(NULL, 0U));
    YEW_ASSERT(!yew_fuss_commit_empty(nul, sizeof(nul)));
    YEW_ASSERT(!yew_fuss_commit_empty(spaces, sizeof(spaces) - 1U));

    bytebuf_init(&clean);
    YEW_ASSERT(!yew_fuss_commit_cleanup(
        &clean, spaces, sizeof(spaces) - 1U, (u8)'#'));
    YEW_ASSERT(yew_fuss_commit_empty(clean.data, clean.len));
    YEW_ASSERT(!yew_fuss_commit_cleanup(
        &clean, comments, sizeof(comments) - 1U, (u8)'#'));
    YEW_ASSERT(yew_fuss_commit_empty(clean.data, clean.len));
    bytebuf_free(&clean);
}

void test_fusscommit_rejects_invalid_pointer_length_pairs(void)
{
    Bytebuf clean;
    u8 selected = (u8)'?';

    bytebuf_init(&clean);
    YEW_ASSERT(!yew_fuss_commit_select_comment(
        NULL, 1U, NULL, 0U, &selected));
    YEW_ASSERT(!yew_fuss_commit_select_comment(
        NULL, 0U, NULL, 0U, NULL));
    YEW_ASSERT(!yew_fuss_commit_cleanup(&clean, NULL, 1U, (u8)'#'));
    YEW_ASSERT(!yew_fuss_commit_cleanup(NULL, NULL, 0U, (u8)'#'));
    bytebuf_free(&clean);
}
