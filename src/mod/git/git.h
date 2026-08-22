#ifndef YEW_MOD_GIT_GIT_H
#define YEW_MOD_GIT_GIT_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct GitCtx GitCtx;
typedef struct GitReq GitReq;
typedef struct YewJobCallbackOps YewJobCallbackOps;

#define YEW_GIT_COLLECT_MAX (16U * 1024U * 1024U)
#define YEW_GIT_READ_TIMEOUT_MS 5000
#define YEW_GIT_NET_TIMEOUT_MS 120000
#define YEW_GIT_MAX_INFLIGHT 4U
#define YEW_GIT_TTL_MS 500

typedef struct GitVersion {
    u16 major;
    u16 minor;
    u16 patch;
} GitVersion;

typedef enum GitAsyncState {
    YEW_GIT_ASYNC_UNTRIED,
    YEW_GIT_ASYNC_PENDING,
    YEW_GIT_ASYNC_READY,
    YEW_GIT_ASYNC_FAILED
} GitAsyncState;

typedef enum GitVerbKind {
    YEW_GV_READ,
    YEW_GV_MUTATE,
    YEW_GV_NET
} GitVerbKind;

typedef struct GitVerb {
    const char *name;
    GitVerbKind kind;
    i64 timeout_ms;
    bool needs_repo;
    bool needs_head;
} GitVerb;

typedef enum GitStatusCode {
    YEW_GIT_OK = 0,
    YEW_GIT_NO_GIT,
    YEW_GIT_NOT_REPO,
    YEW_GIT_BARE,
    YEW_GIT_NO_HEAD,
    YEW_GIT_DETACHED,
    YEW_GIT_NO_UPSTREAM,
    YEW_GIT_CONFLICTED,
    YEW_GIT_MID_MERGE,
    YEW_GIT_MID_REBASE,
    YEW_GIT_MID_CHERRY_PICK,
    YEW_GIT_MID_REVERT,
    YEW_GIT_MID_BISECT,
    YEW_GIT_LOCKED,
    YEW_GIT_AUTH,
    YEW_GIT_TIMEOUT,
    YEW_GIT_FAILED,
    YEW_GIT_PARSE
} GitStatusCode;

typedef struct GitResult {
    GitStatusCode state;
    const char *verb;
    const u8 *out;
    u64 out_len;
    const u8 *err;
    u64 err_len;
    u32 job_id;
} GitResult;

typedef struct GitParseErr {
    u64 off;
    char message[128];
} GitParseErr;

typedef enum GitEntryKind {
    GIT_E_ORDINARY,
    GIT_E_RENAME,
    GIT_E_UNMERGED,
    GIT_E_UNTRACKED,
    GIT_E_IGNORED
} GitEntryKind;

typedef struct GitEntry {
    GitEntryKind kind;
    char x;
    char y;
    char *path;
    u32 path_len;
    char *orig_path;
    u32 orig_len;
    u8 score;
    char index_oid[65];
    bool is_dir;
    bool submodule;
    bool staged;
    bool unstaged;
    bool untracked;
    bool conflicted;
    bool incoming;
} GitEntry;

typedef struct GitEntryList {
    GitEntry *data;
    size_t len;
} GitEntryList;

typedef struct GitPath {
    char *path;
    u32 len;
    bool is_dir;
} GitPath;

typedef struct GitPathList {
    GitPath *data;
    size_t len;
} GitPathList;

typedef struct GitIgnoreSet {
    GitPath *data;
    size_t len;
} GitIgnoreSet;

typedef struct GitRepo {
    char *git_dir;
    char *top_level;
    bool inside_work_tree;
    bool bare;
    u64 env_fp;
    i64 detected_ms;
} GitRepo;

typedef struct GitSnapshot {
    Arena a;
    GitStatusCode state;
    char *branch;
    char *upstream;
    char *head_oid;
    char *comment_char;       /* "#", one configured byte, or "auto" */
    u32 comment_char_len;
    i32 ahead;
    i32 behind;
    bool detached;
    bool unborn;
    bool conflicted;
    GitEntryList entries;
    GitIgnoreSet ignored;
    u32 gen;
    i64 taken_ms;
} GitSnapshot;

typedef struct GitBlameLine {
    u32 lineno;
    u32 commit;
} GitBlameLine;

typedef struct GitBlameLineList {
    GitBlameLine *data;
    size_t len;
} GitBlameLineList;

typedef struct GitCommitMeta {
    char sha[65];
    char *author;
    char *author_mail;
    char *summary;
    i64 author_time;
    i16 author_tz_min;
    bool boundary;
} GitCommitMeta;

typedef struct GitCommitMetaList {
    GitCommitMeta *data;
    size_t len;
} GitCommitMetaList;

typedef struct GitLogRecord {
    char *oid;
    char *short_oid;
    i64 author_time;
    char *author;
    char *author_mail;
    char *parents;
    char *refs;
    char *subject;
    char *body;
} GitLogRecord;

typedef struct GitLogRecordList {
    GitLogRecord *data;
    size_t len;
} GitLogRecordList;

typedef struct GitReflogRecord {
    char *oid;
    char *short_oid;
    char *selector;
    char *message;
    i64 author_time;
    char *subject;
} GitReflogRecord;

typedef struct GitReflogRecordList {
    GitReflogRecord *data;
    size_t len;
} GitReflogRecordList;

/* Snapshot-owned slices point into `snap->a`; drop the whole snapshot
 * rather than freeing their read-only arena slices individually. */
void yew_git_snapshot_init(GitSnapshot *snap);
void yew_git_snapshot_drop(GitSnapshot *snap);

/* Pure byte parsers.  They perform no I/O and allocate returned strings and
 * arrays from the supplied arena (or the snapshot arena). */
bool yew_git_parse_status(GitSnapshot *snap, const u8 *buf, u64 n,
                          GitParseErr *err);
bool yew_git_parse_z_paths(Arena *a, const u8 *buf, u64 n,
                           GitPathList *paths, GitParseErr *err);
bool yew_git_parse_ignore(Arena *a, const u8 *buf, u64 n,
                          GitIgnoreSet *set, GitParseErr *err);
bool yew_git_ignored(const GitIgnoreSet *set, const char *path, u32 len);
u32 yew_git_parse_blame(Arena *a, const u8 *buf, u64 n,
                        GitBlameLineList *lines,
                        GitCommitMetaList *commits, GitParseErr *err);
bool yew_git_parse_log(Arena *a, const u8 *buf, u64 n,
                       GitLogRecordList *records, GitParseErr *err);
bool yew_git_parse_reflog(Arena *a, const u8 *buf, u64 n,
                          GitReflogRecordList *records, GitParseErr *err);

/* Editor-owned module lifecycle.  The stripped build implements the same
 * surface and routes user-facing calls through yew_mod_require(). */
void yew_git_state_init(Ed *ed);
void yew_git_state_free(Ed *ed);
GitAsyncState yew_git_avail_state(const Ed *ed);
GitAsyncState yew_git_detect_state(const Ed *ed);
GitStatusCode yew_git_detect_result(const Ed *ed);
bool yew_git_avail(Ed *ed, GitVersion *out);
GitStatusCode yew_git_detect(Ed *ed, GitRepo *out);
const GitRepo *yew_git_repo_cached(const Ed *ed);
const GitSnapshot *yew_git_snapshot(Ed *ed);
const GitLogRecordList *yew_git_log_records(const Ed *ed);
const GitResult *yew_git_result(const Ed *ed);
bool yew_git_refresh(Ed *ed, bool force);
void yew_git_invalidate(Ed *ed);
u64 yew_git_env_fingerprint(void);

/* Fetch one object by its full object id.  Binary output is published through
 * yew_git_result() and remains valid until the next generic completion. */
u32 yew_git_blob(Ed *ed, const char *oid, char *err, size_t errsz);

const GitVerb *yew_git_verb(const char *name);
const GitVerb *yew_git_verb_at(size_t index);
size_t yew_git_verb_count(void);
u32 yew_git_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                  const GitReq *req, char *err, size_t errsz);
/* Read-only module jobs that need their own completion owner, such as a
 * picker preview.  They inherit Git's locked environment and argv policy
 * without publishing through the single generic GitResult slot. */
u32 yew_git_spawn_callback(Ed *ed, const GitVerb *verb,
                           char *const *argv, void *owner,
                           const YewJobCallbackOps *ops,
                           char *err, size_t errsz);

const char *yew_git_state_str(GitStatusCode code);

CmdStatus yew_git_cmd_info(CmdCtx *cx);
CmdStatus yew_git_cmd_refresh(CmdCtx *cx);
CmdStatus yew_git_cmd_log(CmdCtx *cx);
CmdStatus yew_git_cmd_require(CmdCtx *cx);

#endif
