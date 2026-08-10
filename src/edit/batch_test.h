#ifndef SAG_EDIT_BATCH_TEST_H
#define SAG_EDIT_BATCH_TEST_H

/* Sprint 37: the assertion host installed by sag --batch --test. */

#include <stdbool.h>

#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"

typedef struct FlVm FlVm;

typedef struct SagBatchTestLog {
    char *message;
    u8 level;
} SagBatchTestLog;

typedef struct SagBatchTestState {
    FlVm *vm;
    Bytebuf failure_records;
    SagBatchTestLog *logs;
    u32 nlogs;
    u32 caplogs;
    u64 assertions;
    u64 failures;
    bool skipped;
    bool installed;
    bool finished;
} SagBatchTestState;

/* One batch test runs in one process, so only one installed state is active. */
void sag_batch_test_init(SagBatchTestState *state);
bool sag_batch_test_install(SagBatchTestState *state, FlVm *vm);
void sag_batch_test_note_log(SagBatchTestState *state, SagLogLevel level,
                             const char *message);

/* Pass fd < 0 to use SAG_SCRIPT_RESULT_FD, falling back to stderr. */
bool sag_batch_test_finish(SagBatchTestState *state, int fd);
void sag_batch_test_free(SagBatchTestState *state);

#endif /* SAG_EDIT_BATCH_TEST_H */
