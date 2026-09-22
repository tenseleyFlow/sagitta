#ifndef YEW_UTIL_SECRET_H
#define YEW_UTIL_SECRET_H

/*
 * Sprint 57.23 §4: does an environment NAME say it holds a secret?
 *
 * The fragments are the ones the AI redactor's `env-assignment` rule
 * already matches (src/mod/ai/redact.c).  That list used to exist only
 * inside a regex string in an excisable module, and the shell completer's
 * value preview needs it in core so that MODULES="" still redacts.  A
 * unit test in the AI suite scans a secret-shaped assignment for every
 * fragment here, so the two lists cannot drift apart silently.
 *
 * Why redact at all: a completion pager is screen-shared far more often
 * than it is secret.
 */

#include <stdbool.h>
#include <stddef.h>

/* Case-insensitive substring match against every fragment.  NULL is not
 * a secret name. */
bool yew_secret_name(const char *name);

/* The fragment table itself, for the drift test.  Upper case ASCII. */
const char *const *yew_secret_fragments(size_t *n);

#endif
