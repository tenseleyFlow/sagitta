#ifndef SAG_FL_FLAPI_H
#define SAG_FL_FLAPI_H

/*
 * Sprint 34 deliverable 3: the editor API, as seen from Fletch.
 *
 * THIS SLICE IS QUERIES ONLY.  Every native here READS the model and
 * returns a value; not one of them mutates.  That is deliberate
 * sequencing, not an oversight: the mutating half must go through a
 * registered command and sag_cmd_invoke (s34 §3's law, enforced by
 * scripts/check-fl-choke.sh), and the commands it needs -- ed.buf.open,
 * ed.edit.insert.at, ed.cursor.set and the rest of §3's table -- do not
 * exist yet.  Landing the read-only surface first means the choke gate
 * governs the file from its first line rather than being retrofitted
 * around a mutation that predates it.
 *
 * WHAT IT UNBLOCKS.  Until now nothing outside handle.c called the
 * resolvers, so a script could not hold an editor handle at all and
 * spec §9's "handle" kind was unreachable -- which is why amendment A2
 * could not be filed (coverage check 4 wants an `# ERROR_KIND: handle`
 * case, and there was no way to raise one).  With `buf.current()` and a
 * resolver behind it, a script can hold a handle, and the kind becomes
 * reachable the moment a buffer closes under one.
 *
 * NO RECEIVER SUGAR YET either.  Spec §4's `b.len()` is `.` applied to a
 * handle, which is a change to the VM's member-access path; these are
 * free functions on the `buf` module (`buf.len(b)`) until that lands, so
 * the two changes can be reviewed and gated apart.
 */

#include "fl/std.h"

/*
 * The editor modules, registered alongside the seven builtins.
 *
 * Present even when vm->ed is NULL -- a headless `sag fl` still sees
 * `buf`, and every native here raises spec §9's "handle" with "no editor
 * to resolve..." rather than being an undefined name.  A missing module
 * would report a typo for something that is merely unavailable, which is
 * the confusion invariant 3 exists to prevent.
 */
extern const FlModuleDef fl_mod_buf;

#endif /* SAG_FL_FLAPI_H */
