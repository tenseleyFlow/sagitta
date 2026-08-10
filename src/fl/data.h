#ifndef SAG_FL_DATA_H
#define SAG_FL_DATA_H

/*
 * Canonical Fletch pure-data bridge.
 *
 * Reading is syntax only: fl_parse_literal has no production for calls,
 * imports, identifiers-as-values, or statements, so this path grants and
 * executes nothing.  Maps retain their source insertion order.
 */

#include <stddef.h>

#include "fl/diag.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/base.h"
#include "util/buf.h"

enum {
    FL_DATA_MAX_BYTES = 8U * 1024U * 1024U,
    FL_DATA_MAX_NODES = 1000000U,
    FL_DATA_MAX_DEPTH = 32U,
    FL_DATA_MAX_STRING = 4096U
};

FlValue fl_data_read(FlVm *vm, const char *src, size_t len, DiagCtx *dc);
void fl_data_write(Bytebuf *out, FlValue v, u32 indent);

#endif /* SAG_FL_DATA_H */
