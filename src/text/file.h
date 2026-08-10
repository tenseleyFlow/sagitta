#ifndef YEW_TEXT_FILE_H
#define YEW_TEXT_FILE_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "text/piece.h"

typedef enum {
    YEW_EOL_LF,
    YEW_EOL_CRLF,
    YEW_EOL_MIXED
} YewEol;

typedef struct FileMeta {
    YewEol eol;
    YewEol dominant_eol;
    u32 crlf_count;
    u32 lf_count;
    bool had_bom;
    bool binary;
    bool had_invalid_utf8;
    bool missing_final_nl;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    nlink_t nlink;
    dev_t dev;
    ino_t ino;
    bool exists;
    bool via_symlink;
    char *realpath;
    struct timespec mtime;
    u64 size_on_disk;
} FileMeta;

typedef enum {
    YEW_LOAD_OK,
    YEW_LOAD_ENOENT,
    YEW_LOAD_EACCES,
    YEW_LOAD_EISDIR,
    YEW_LOAD_TOO_LARGE,
    YEW_LOAD_IO
} YewLoadErr;

typedef enum {
    YEW_SAVE_OK,
    YEW_SAVE_IO,
    YEW_SAVE_PERM,
    YEW_SAVE_CHANGED_ON_DISK
} YewSaveErr;

YewLoadErr yew_file_load(const char *path, TextBuf **out, FileMeta *meta);
/* Sprint 24 D4 test hook: file reads performed so far.  Counting is the
 * only way to check that a deferred group costs one read, not forty. */
u64 yew_file_load_count(void);
void yew_file_load_count_reset(void);
YewSaveErr yew_file_save(const TextBuf *tb, FileMeta *meta,
                         const char *path);
/* Overwrite after accepting the destination identity observed now. */
YewSaveErr yew_file_save_force(const TextBuf *tb, FileMeta *meta,
                               const char *path);
YewSaveErr yew_file_write_atomic(const char *path, const u8 *bytes,
                                 size_t len, mode_t mode);

/*
 * Moves `from` to `to`, REFUSING to overwrite an existing `to`.
 *
 * Sprint 25 §7 sets an unreadable state file aside under a
 * second-resolution timestamp, so two failures inside one second
 * collide — and plain rename(2) would silently destroy the first one.
 * This lives here rather than in src/ws/ so there is exactly one place
 * in the tree that moves a file the user might still want, next to the
 * primitive that replaces one.
 */
YewSaveErr yew_file_move_aside(const char *from, const char *to);

void yew_filemeta_init(FileMeta *meta);
void yew_filemeta_dispose(FileMeta *meta);
void yew_filemeta_eol_bytes(const FileMeta *meta, const u8 **bytes,
                            size_t *len);

#endif
