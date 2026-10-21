#ifndef YEW_MOD_GIT_CONFLICT_H
#define YEW_MOD_GIT_CONFLICT_H

#include "text/coords.h"
#include "util/buf.h"
#include "util/vec.h"

enum { YEW_CONFLICT_BYTES_MAX = 16 * 1024 * 1024,
       YEW_CONFLICT_LINES_MAX = 200000 };

typedef struct GitConflictStage {
    char oid[65];
    u32 mode;
    bool present;
} GitConflictStage;

typedef struct GitConflictIndex {
    GitConflictStage stage[3]; /* ancestor, current, incoming */
} GitConflictIndex;

/* Parses one literal path's ls-files --unmerged -z records. Failure is
 * atomic. Empty input is a valid, now-resolved index, not empty blobs. */
bool yew_git_conflict_index(GitConflictIndex *out, const u8 *bytes,
                             size_t len, const char *path);
bool yew_git_conflict_index_equal(const GitConflictIndex *a,
                                  const GitConflictIndex *b);

typedef struct GitConflictRange {
    Span envelope;
    Span current;
    Span base;
    Span incoming;
    u32 line;
    u32 width;
    bool has_base;
} GitConflictRange;
VEC_DECL(GitConflictRangeVec, GitConflictRange);

typedef enum GitConflictParseResult {
    YEW_CONFLICT_PENDING,
    YEW_CONFLICT_OK,
    YEW_CONFLICT_INVALID,
    YEW_CONFLICT_LIMIT,
    YEW_CONFLICT_BINARY
} GitConflictParseResult;

typedef struct GitConflictParse {
    const u8 *bytes; /* borrowed, immutable until drop */
    size_t len, at, line_at, marker_run;
    u32 line, width, state;
    GitConflictRange current;
    GitConflictRangeVec ranges;
    GitConflictParseResult result;
    const char *error; /* static diagnostic */
} GitConflictParse;

/* Zero-init before first use; start releases a previous parse. Width 0
 * detects >=7; explicit attribute widths may be smaller. Never publishes
 * partial ranges as a successful parse. Steps inspect at most byte_budget
 * input bytes, including lines longer than a scheduling slice. */
void yew_git_conflict_parse_start(GitConflictParse *p, const u8 *bytes,
                                  size_t len, u32 width);
GitConflictParseResult yew_git_conflict_parse_step(GitConflictParse *p,
                                                   size_t byte_budget);
void yew_git_conflict_parse_drop(GitConflictParse *p);

typedef enum GitConflictChoice {
    YEW_CONFLICT_CURRENT,
    YEW_CONFLICT_INCOMING,
    YEW_CONFLICT_BOTH
} GitConflictChoice;

/* Copies verified arm bytes, never marker/base bytes. A caller may shorten
 * an arm only after proving a Git-added EOF newline against its stage.
 * Both preserves current-first order with one explicit boundary newline.
 * Failure leaves out unchanged. Does not edit or validate Git identity. */
bool yew_git_conflict_choice(Bytebuf *out, const u8 *bytes, size_t len,
                              const GitConflictRange *range,
                              GitConflictChoice choice, bool crlf);

typedef struct GitConflictMatch GitConflictMatch;
/* Finds a unique whole-line arm in a stage, allowing only CRLF/LF and
 * Git's added terminal LF. Work is resumable; pointers remain borrowed.
 * Zero-byte arms are valid, with an empty match at zero. */
GitConflictMatch *yew_git_conflict_match_new(const u8 *source, size_t len,
                                            const u8 *arm, size_t arm_len);
GitConflictParseResult yew_git_conflict_match_step(GitConflictMatch *work,
                                                   size_t budget);
bool yew_git_conflict_match_span(const GitConflictMatch *work, Span *span,
                                  bool *missing_lf);
void yew_git_conflict_match_free(GitConflictMatch *work);

#endif
