#ifndef SAG_TEXT_FILE_H
#define SAG_TEXT_FILE_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "text/piece.h"

typedef enum {
    SAG_EOL_LF,
    SAG_EOL_CRLF,
    SAG_EOL_MIXED
} SagEol;

typedef struct FileMeta {
    SagEol eol;
    SagEol dominant_eol;
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
    SAG_LOAD_OK,
    SAG_LOAD_ENOENT,
    SAG_LOAD_EACCES,
    SAG_LOAD_EISDIR,
    SAG_LOAD_TOO_LARGE,
    SAG_LOAD_IO
} SagLoadErr;

typedef enum {
    SAG_SAVE_OK,
    SAG_SAVE_IO,
    SAG_SAVE_PERM,
    SAG_SAVE_CHANGED_ON_DISK
} SagSaveErr;

SagLoadErr sag_file_load(const char *path, TextBuf **out, FileMeta *meta);
SagSaveErr sag_file_save(const TextBuf *tb, const FileMeta *meta,
                         const char *path);
SagSaveErr sag_file_write_atomic(const char *path, const u8 *bytes,
                                 size_t len, mode_t mode);

void sag_filemeta_init(FileMeta *meta);
void sag_filemeta_dispose(FileMeta *meta);
void sag_filemeta_eol_bytes(const FileMeta *meta, const u8 **bytes,
                            size_t *len);

#endif
