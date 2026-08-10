#ifndef YEW_EDIT_BATCH_TEST_H
#define YEW_EDIT_BATCH_TEST_H

/* Sprint 37: the assertion host installed by yew --batch --test. */

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"

typedef struct FlVm FlVm;

typedef struct YewBatchTestLog {
    char *message;
    u8 level;
} YewBatchTestLog;

typedef struct YewBatchTestState {
    FlVm *vm;
    Bytebuf failure_records;
    YewBatchTestLog *logs;
    u32 nlogs;
    u32 caplogs;
    u64 assertions;
    u64 failures;
    bool skipped;
    bool installed;
    bool finished;
} YewBatchTestState;

/* One batch test runs in one process, so only one installed state is active. */
void yew_batch_test_init(YewBatchTestState *state);
bool yew_batch_test_install(YewBatchTestState *state, FlVm *vm);
void yew_batch_test_note_log(YewBatchTestState *state, YewLogLevel level,
                             const char *message);

/* Pass fd < 0 to use YEW_SCRIPT_RESULT_FD, falling back to stderr. */
bool yew_batch_test_finish(YewBatchTestState *state, int fd);
void yew_batch_test_free(YewBatchTestState *state);

#endif /* YEW_EDIT_BATCH_TEST_H */
