#include "harness.h"

#include <string.h>

#include "syn/fortran.h"
#include "util/buf.h"

typedef struct FormCase {
    const char *text;
    SynFortranForm want;
} FormCase;

void test_syn_fortran_twenty_hand_scored_inputs(void)
{
    static const FormCase cases[] = {
        {"C legacy comment", YEW_FORTRAN_FIXED},
        {"c legacy comment", YEW_FORTRAN_FIXED},
        {"* legacy comment", YEW_FORTRAN_FIXED},
        {"! legacy comment", YEW_FORTRAN_FIXED},
        {"     1continue", YEW_FORTRAN_FIXED},
        {"12345Xcontinue", YEW_FORTRAN_FIXED},
        {"      integer :: x &", YEW_FORTRAN_FREE},
        {"integer :: x ! note", YEW_FORTRAN_FREE},
        {"module m", YEW_FORTRAN_FREE},
        {"PROGRAM x", YEW_FORTRAN_FREE},
        {"end", YEW_FORTRAN_FREE},
        {"C legacy\nmodule m\nend", YEW_FORTRAN_FREE},
        {"C legacy\nmodule m", YEW_FORTRAN_FIXED},
        {"      call work", YEW_FORTRAN_AUTO},
        {"     0continue", YEW_FORTRAN_AUTO},
        {"! ", YEW_FORTRAN_AUTO},
        {"1234A1bad", YEW_FORTRAN_AUTO},
        {"  value ! note", YEW_FORTRAN_FREE},
        {"\tinteger :: x", YEW_FORTRAN_AUTO},
        {"program x\n     1cont", YEW_FORTRAN_FIXED},
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        SynFortranScore score;
        SynFortranForm got = yew_syn_fortran_score_bytes(
            (const u8 *)cases[i].text, strlen(cases[i].text), &score);

        YEW_ASSERT_EQ_U64(got, cases[i].want);
        YEW_ASSERT(score.nonblank != 0U);
    }
}

void test_syn_fortran_tie_limit_and_column_signal_are_pinned(void)
{
    char long_line[80];
    Bytebuf hundred;
    SynFortranScore score;
    SynFortranForm got;
    u32 i;

    (void)memset(long_line, ' ', sizeof(long_line));
    long_line[72] = 'x';
    got = yew_syn_fortran_score_bytes((const u8 *)long_line,
                                      sizeof(long_line), &score);
    YEW_ASSERT_EQ_U64(got, YEW_FORTRAN_FREE);
    YEW_ASSERT_EQ_I64(score.fixed_form, -2);
    YEW_ASSERT_EQ_I64(score.free_form, 0);

    bytebuf_init(&hundred);
    for (i = 0U; i < 100U; i++)
        bytebuf_append(&hundred, "x\n", 2U);
    bytebuf_append(&hundred, "C ignored after limit\n", 22U);
    got = yew_syn_fortran_score_bytes(hundred.data, hundred.len, &score);
    YEW_ASSERT_EQ_U64(got, YEW_FORTRAN_AUTO);
    YEW_ASSERT_EQ_U64(score.nonblank, 100U);
    YEW_ASSERT_EQ_U64(score.signals, 0U);
    bytebuf_free(&hundred);
}
