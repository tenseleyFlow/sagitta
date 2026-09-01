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
    /* Pinned last-loaded/saved bytes for exact save.check_disk=content. */
    TextSnap disk_snapshot;
    bool disk_snapshot_valid;
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
    YEW_SAVE_CHANGED_ON_DISK,
    /* Document bytes are durable, but backup retention did not commit. */
    YEW_SAVE_BACKUP_FAILED
} YewSaveErr;

typedef enum {
    YEW_SAVE_STRATEGY_AUTO = 0,
    YEW_SAVE_STRATEGY_ATOMIC,
    YEW_SAVE_STRATEGY_INPLACE
} YewSaveStrategy;

typedef enum {
    YEW_SAVE_CHECK_OFF = 0,
    YEW_SAVE_CHECK_MTIME,
    YEW_SAVE_CHECK_CONTENT
} YewSaveCheck;

#define YEW_SAVE_STRATEGY_DEFAULT YEW_SAVE_STRATEGY_AUTO
#define YEW_SAVE_STRATEGY_DEFAULT_TEXT "auto"
#define YEW_SAVE_CHECK_DISK_DEFAULT YEW_SAVE_CHECK_MTIME
#define YEW_SAVE_CHECK_DISK_DEFAULT_TEXT "mtime"
#define YEW_SAVE_CHECK_DISK_MAX_DEFAULT (UINT64_C(8) * 1024U * 1024U)
#define YEW_SAVE_CHECK_DISK_MAX_LIMIT (UINT64_C(4) * 1024U * 1024U * 1024U)
#define YEW_SAVE_BACKUP_KEEP_DEFAULT 1U
#define YEW_SAVE_BACKUP_KEEP_MAX 64U
#define YEW_SAVE_BACKUP_DIR_DEFAULT ""
#define YEW_PLUG_VERIFY_ON_LOAD_DEFAULT true

typedef struct YewSaveOpts {
    YewSaveStrategy strategy;
    YewSaveCheck check_disk;
    u64 check_disk_max;
    u32 backup_keep;
    /* Empty or NULL selects $XDG_STATE_HOME/yew/backup/. */
    const char *backup_dir;
} YewSaveOpts;

typedef struct YewAtomicWriteResult {
    YewSaveErr error;
    /* The destination rename completed, even if parent-directory sync did not. */
    bool committed;
} YewAtomicWriteResult;

YewLoadErr yew_file_load(const char *path, TextBuf **out, FileMeta *meta);
/* Existing paths name the same file when their device/inode identity agrees. */
bool yew_file_same_identity(const char *left, const char *right);
/* Sprint 24 D4 test hook: file reads performed so far.  Counting is the
 * only way to check that a deferred group costs one read, not forty. */
u64 yew_file_load_count(void);
void yew_file_load_count_reset(void);
YewSaveErr yew_file_save(const TextBuf *tb, FileMeta *meta,
                         const char *path);
YewSaveErr yew_file_save_opts(const TextBuf *tb, FileMeta *meta,
                              const char *path, const YewSaveOpts *opts);
/* Overwrite after accepting the destination identity observed now. */
YewSaveErr yew_file_save_force(const TextBuf *tb, FileMeta *meta,
                               const char *path);
YewSaveErr yew_file_save_force_opts(const TextBuf *tb, FileMeta *meta,
                                    const char *path,
                                    const YewSaveOpts *opts);
void yew_file_save_opts_default(YewSaveOpts *opts);
YewSaveErr yew_file_write_atomic(const char *path, const u8 *bytes,
                                 size_t len, mode_t mode);
YewAtomicWriteResult yew_file_write_atomic_result(const char *path,
                                                  const u8 *bytes,
                                                  size_t len, mode_t mode);
/* True when the requested document bytes are now the destination contents. */
bool yew_save_committed(YewSaveErr error);

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
/* Release the exact-content baseline when a buffer becomes non-resident. */
void yew_filemeta_content_forget(FileMeta *meta);
void yew_filemeta_eol_bytes(const FileMeta *meta, const u8 **bytes,
                            size_t *len);

#endif
