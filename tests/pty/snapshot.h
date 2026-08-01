#ifndef SAG_TEST_PTY_SNAPSHOT_H
#define SAG_TEST_PTY_SNAPSHOT_H

#include <stdbool.h>

#include "util/buf.h"
#include "vt.h"

void snapshot_write(const VtScreen *v, Bytebuf *out);
bool snapshot_read(const Bytebuf *in, VtScreen *out, Bytebuf *msg);
bool snapshot_compare(const Bytebuf *got, const Bytebuf *want, Bytebuf *msg);

#endif
