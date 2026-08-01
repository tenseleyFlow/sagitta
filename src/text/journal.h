#ifndef SAG_TEXT_JOURNAL_H
#define SAG_TEXT_JOURNAL_H

#include <stdbool.h>

#include "text/file.h"
#include "text/piece.h"
#include "util/base.h"

typedef struct Journal Journal;

enum {
    SAG_JOURNAL_INS = 1,
    SAG_JOURNAL_DEL = 2
};

Journal *sag_journal_open(const char *realpath, const FileMeta *m);
void sag_journal_record(Journal *j, u8 op, u64 off, const u8 *b, u64 n);
void sag_journal_sync(Journal *j);
bool sag_journal_ok(const Journal *j);
/* Close a dirty journal without deleting its recovery data. */
void sag_journal_close(Journal *j);
void sag_journal_discard(Journal *j);
bool sag_journal_replay(const char *path, TextBuf *tb, FileMeta *m);

#endif
