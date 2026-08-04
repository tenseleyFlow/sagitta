#ifndef SAG_UI_CMDHIST_H
#define SAG_UI_CMDHIST_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

enum {
    SAG_HIST_MAX = 1000,
    SAG_HIST_LINE_MAX = 4096
};

typedef struct CmdHist CmdHist;

/* Persistent history uses the XDG state directory and never fails hard. */
CmdHist *sag_hist_open(const char *kind);
/* In-memory histories are used by --clean and --batch. */
CmdHist *sag_hist_open_memory(void);
void sag_hist_close(CmdHist *h);

void sag_hist_add(CmdHist *h, const char *line);
void sag_hist_flush(CmdHist *h);

size_t sag_hist_len(const CmdHist *h);
const char *sag_hist_at(const CmdHist *h, size_t index);
bool sag_hist_is_memory(const CmdHist *h);

typedef struct HistCur {
    i32 idx;
    char *stem;
    char *draft;
} HistCur;

/* Reset before a new walk. The first prev freezes draft as the search stem. */
void sag_hist_cur_reset(HistCur *c, const char *draft);
void sag_hist_cur_dispose(HistCur *c);
const char *sag_hist_prev(CmdHist *h, HistCur *c);
const char *sag_hist_next(CmdHist *h, HistCur *c);

#endif
