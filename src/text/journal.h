#ifndef SAG_TEXT_JOURNAL_H
#define SAG_TEXT_JOURNAL_H

#include <stdbool.h>

#include "text/file.h"
#include "text/piece.h"
#include "util/base.h"

typedef struct Journal Journal;
typedef struct EditCtx EditCtx;

enum {
    SAG_JOURNAL_INS = 1,
    SAG_JOURNAL_DEL = 2
};

/* IEEE CRC-32, suitable for streaming serialized records. */
u32 sag_crc32_begin(void);
u32 sag_crc32_add(u32 crc, const u8 *bytes, size_t len);
u32 sag_crc32_end(u32 crc);
u32 sag_crc32(const u8 *bytes, size_t len);

Journal *sag_journal_open(const char *realpath, const FileMeta *m);
bool sag_journal_record(Journal *j, u8 op, u64 off, const u8 *b, u64 n);
bool sag_journal_sync(Journal *j);
bool sag_journal_ok(const Journal *j);
/* Close a dirty journal without deleting its recovery data. */
void sag_journal_close(Journal *j);
void sag_journal_discard(Journal *j);
/* Read-only test for a journal whose source identity still matches. */
bool sag_journal_probe(const char *path, const FileMeta *m);
/* Discard the matching on-disk journal without first adopting it. */
bool sag_journal_discard_path(const char *path, const FileMeta *m);
bool sag_journal_replay(const char *path, TextBuf *tb, FileMeta *m);
/* Replay as one external undo transaction without journaling the replay. */
bool sag_journal_replay_edit(const char *path, EditCtx *ec, FileMeta *m);

#endif
