#ifndef YEW_WS_SYMWALK_H
#define YEW_WS_SYMWALK_H

/* Sprint 44: cooperative workspace discovery and symbol indexing.
 *
 * Re-indexing is explicit: workspace open and ed.compl.reindex call
 * yew_symwalk_start.  There is deliberately no filesystem watching,
 * inotify/kqueue integration, polling, or cross-workspace index in 1.0.
 */

#include <stdbool.h>

#include "util/base.h"
#include "util/vec.h"
#include "ws/walk.h"

typedef struct Ed Ed;

enum {
    YEW_SYMWALK_MAX_FILES = 20000,
    YEW_SYMWALK_MAX_SYMS_PER_FILE = 4000,
    YEW_SYMWALK_SCAN_LINES = 4
};

#define YEW_SYMWALK_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define YEW_SYMWALK_MAX_LINE_BYTES (64U * 1024U)
#define YEW_SYMWALK_BUDGET_US 2000

VEC_DECL(Vec_SymPath, u32);

typedef struct SymWalk {
    u32 job;
    Vec_SymPath queue;
    u32 next;
    u64 files_done;
    u64 files_total;
    u64 bytes_read;
    u64 long_files_skipped;
    bool running;
    bool capped;

    /* Private state for the bespoke fallback. */
    FileList files;
    WalkState *walk;
    bool files_init;
    bool discovery_done;
    bool fallback_reported;
    Vec_SymPath retired_jobs;
    void *scratch_syn;
    void *scan_buf;
    int scan_fd;
    u64 scan_size;
    u64 scan_at;
    u64 scan_line_bytes;
    u64 scan_line;
    u32 scan_symbols;
    size_t queue_at;
    size_t git_at;
    bool scan_bound;
    bool queueing_files;
} SymWalk;

void yew_symwalk_start(Ed *ed);
void yew_symwalk_pump(Ed *ed, i64 budget_us);
void yew_symwalk_stop(Ed *ed);
void yew_symwalk_dispose(Ed *ed);

#endif /* YEW_WS_SYMWALK_H */
