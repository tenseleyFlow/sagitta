#ifndef SAG_UI_CMDCOMP_H
#define SAG_UI_CMDCOMP_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;

enum { SAG_COMP_MAX = 500 };

typedef enum {
    SAG_COMP_CMD,
    SAG_COMP_PATH,
    SAG_COMP_BUFFER,
    SAG_COMP_OPTION,
    SAG_COMP_VALUE
} SagCompKind;

typedef struct {
    const char *text;
    const char *detail;
    u8 kind;
    bool is_dir;
    i32 score;
} CompItem;

VEC_DECL(Vec_CompItem, CompItem);

typedef struct CompSource {
    SagCompKind kind;
    u32 (*enumerate)(Ed *ed, const char *stem, Vec_CompItem *out);
} CompSource;

typedef struct CompMenu {
    Vec_CompItem items;
    i32 sel;
    Span replace;
    bool cycling;
} CompMenu;

typedef struct SagCompQuery {
    SagCompKind kind;
    const CompSource *source;
    const char *stem;
    Span replace;
} SagCompQuery;

/* Returns -1 when cand is not a bytewise subsequence of stem. */
i32 sag_comp_score(const char *stem, const char *cand);

/* Resolve an argspec position. token_index is zero for the command name. */
bool sag_comp_kind_for(const CmdEntry *entry, u32 token_index,
                       SagCompKind *kind);

/* Tolerant command-line source selection at the cursor. */
bool sag_comp_query(Ed *ed, const char *line, size_t len, size_t cursor,
                    Arena *scratch, SagCompQuery *out);

const CompSource *sag_comp_source(SagCompKind kind);
u32 sag_comp_enumerate(Ed *ed, SagCompKind kind, const char *stem,
                       Vec_CompItem *out);

/* Quote one completion so the Sprint 18 tokenizer reads one argv element. */
char *sag_comp_quote(Arena *arena, const char *text);
char *sag_comp_lcp(Arena *arena, const Vec_CompItem *items);

void sag_comp_menu_init(CompMenu *menu);
void sag_comp_menu_free(CompMenu *menu);

/* Unit-test seam: exercise the required DT_UNKNOWN/lstat path. */
void sag_comp_test_force_dtype_unknown(bool force);
u32 sag_comp_test_lstat_count(void);

#endif
