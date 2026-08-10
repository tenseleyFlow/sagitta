#ifndef YEW_UI_CMDHIST_H
#define YEW_UI_CMDHIST_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

enum {
    YEW_HIST_MAX = 1000,
    YEW_HIST_LINE_MAX = 4096
};

typedef struct CmdHist CmdHist;

/* Persistent history uses the XDG state directory and never fails hard. */
CmdHist *yew_hist_open(const char *kind);

/*
 * Sprint 25 §8: per-workspace history, closing s18's deferral.
 *
 * `ws_dir` is a workspace state directory (<ws_dir>/history/<kind>), or
 * NULL to behave exactly like yew_hist_open.
 *
 * READS MERGE, WRITES DO NOT.  Both files are loaded — global first,
 * workspace second, so the most local entries land newest under s18's
 * newest-last convention and win the dedupe.  Appends and compaction go
 * only to the SCOPE's file.
 *
 * The pitfall this signature exists to prevent: merging at SAVE instead
 * of at load.  Writing the merged list back would copy every global
 * entry into the workspace file, and then into the next one, until
 * every workspace held everybody's history and none of it meant
 * anything.
 */
CmdHist *yew_hist_open_scoped(const char *kind, const char *ws_dir,
                              bool workspace_scope);
/* Where writes go.  NULL for an in-memory history. */
const char *yew_hist_path(const CmdHist *h);
/* In-memory histories are used by --clean and --batch. */
CmdHist *yew_hist_open_memory(void);
void yew_hist_close(CmdHist *h);

void yew_hist_add(CmdHist *h, const char *line);
void yew_hist_flush(CmdHist *h);

size_t yew_hist_len(const CmdHist *h);
const char *yew_hist_at(const CmdHist *h, size_t index);
bool yew_hist_is_memory(const CmdHist *h);

typedef struct HistCur {
    i32 idx;
    char *stem;
    char *draft;
} HistCur;

/* Reset before a new walk. The first prev freezes draft as the search stem. */
void yew_hist_cur_reset(HistCur *c, const char *draft);
void yew_hist_cur_dispose(HistCur *c);
const char *yew_hist_prev(CmdHist *h, HistCur *c);
const char *yew_hist_next(CmdHist *h, HistCur *c);

#endif
