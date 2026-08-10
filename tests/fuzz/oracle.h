#ifndef YEW_TEST_FUZZ_ORACLE_H
#define YEW_TEST_FUZZ_ORACLE_H

#include "util/base.h"
#include "util/buf.h"

typedef struct {
    Bytebuf *data;
    size_t len;
    size_t cap;
} OracleLines;

typedef struct Oracle {
    OracleLines lines;
} Oracle;

void oracle_init(Oracle *o, const u8 *bytes, u64 len);
void oracle_free(Oracle *o);
void oracle_insert(Oracle *o, u64 at, const u8 *bytes, u64 len);
void oracle_delete(Oracle *o, u64 lo, u64 hi);
u64 oracle_len(const Oracle *o);
u64 oracle_line_count(const Oracle *o);
u64 oracle_line_start(const Oracle *o, u64 line);
u64 oracle_line_of(const Oracle *o, u64 off);
void oracle_materialize(const Oracle *o, Bytebuf *out);

#endif
