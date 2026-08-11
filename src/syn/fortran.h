#ifndef YEW_SYN_FORTRAN_H
#define YEW_SYN_FORTRAN_H

#include <stddef.h>

#include "util/base.h"

typedef enum SynFortranForm {
    YEW_FORTRAN_AUTO = 0,
    YEW_FORTRAN_FREE,
    YEW_FORTRAN_FIXED
} SynFortranForm;

typedef struct SynFortranScore {
    i32 free_form;
    i32 fixed_form;
    u32 nonblank;
    u32 signals;
} SynFortranScore;

void yew_syn_fortran_score_init(SynFortranScore *score);
void yew_syn_fortran_score_line(SynFortranScore *score,
                                const u8 *line, size_t len);
SynFortranForm yew_syn_fortran_score_result(const SynFortranScore *score);
SynFortranForm yew_syn_fortran_score_bytes(const u8 *text, size_t len,
                                           SynFortranScore *score_out);
bool yew_syn_fortran_ambiguous_path(const char *path);
bool yew_syn_fortran_legacy_path(const char *path);

#endif /* YEW_SYN_FORTRAN_H */
