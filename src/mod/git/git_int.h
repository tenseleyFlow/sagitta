#ifndef YEW_MOD_GIT_GIT_INT_H
#define YEW_MOD_GIT_GIT_INT_H

#include "mod/git/git.h"

typedef enum GitReqKind {
    YEW_GREQ_VERSION,
    YEW_GREQ_DETECT,
    YEW_GREQ_STATUS,
    YEW_GREQ_IGNORE,
    YEW_GREQ_UPSTREAM,
    YEW_GREQ_INCOMING,
    YEW_GREQ_LOG,
    YEW_GREQ_REFLOG,
    YEW_GREQ_BLOB,
    YEW_GREQ_VERB
} GitReqKind;

struct GitReq {
    Arena arena;
    GitReqKind kind;
    u32 job_id;
    bool literal_paths;
    bool retried;
    const u8 *stdin_bytes;
    u64 stdin_len;
    char *detect_root;
    u64 detect_env_fp;
    void *owner;
};

typedef u32 (*GitTestSpawnFn)(Ed *ed, const GitVerb *verb,
                              char *const *argv, const GitReq *req,
                              void *opaque, char *err, size_t errsz);

/* Pure cache/taxonomy helpers and explicit test seams.  Production calls
 * use yew_now_ms() and the generic job layer; tests can replace either
 * without teaching the public module API about the harness. */
bool yew_git_cache_fresh(i64 taken_ms, i64 now_ms);
GitStatusCode yew_git_probe_state(const GitRepo *repo);
void yew_git_test_now_set(Ed *ed, i64 now_ms);
void yew_git_test_spawn_set(GitTestSpawnFn spawn, void *opaque);
bool yew_git_test_complete(Ed *ed, u32 job_id, GitStatusCode state,
                           const u8 *out, u64 out_len,
                           const u8 *err, u64 err_len);
bool yew_git_test_complete_exit(Ed *ed, u32 job_id, GitStatusCode state,
                                int exit_code, const u8 *out, u64 out_len,
                                const u8 *err, u64 err_len);
GitStatusCode yew_git_test_auth_state(const u8 *bytes, u64 len);

#endif
