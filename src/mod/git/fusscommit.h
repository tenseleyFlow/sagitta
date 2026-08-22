#ifndef YEW_MOD_GIT_FUSSCOMMIT_H
#define YEW_MOD_GIT_FUSSCOMMIT_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"
#include "util/buf.h"

/*
 * Resolve core.commentChar for a commit-message template.  An absent setting
 * selects '#'.  The setting "auto" selects the first candidate in Git's
 * prescribed order whose byte is not the first nonblank byte of any line
 * (blank means space or tab, matching the cleanup rule below).
 * A one-byte setting selects that byte directly.  Invalid settings and an
 * exhausted auto candidate set return false without changing *comment_char.
 */
bool yew_fuss_commit_select_comment(const u8 *message, size_t message_len,
                                    const u8 *setting, size_t setting_len,
                                    u8 *comment_char);

/*
 * Apply yew's COMMIT_EDITMSG cleanup to caller-owned storage.  `clean` must
 * be initialized and must not alias `message`; its previous contents are
 * replaced.  Comment recognition skips leading spaces and tabs.  Trailing
 * ASCII whitespace is removed, including CR from CRLF input.  Retained lines
 * are joined with '\n', without an outer newline.  The return value is false
 * exactly when the cleaned message has zero bytes.
 */
bool yew_fuss_commit_cleanup(Bytebuf *clean, const u8 *message,
                             size_t message_len, u8 comment_char);

/* Deliberately byte-exact: whitespace and NUL bytes are not "empty". */
bool yew_fuss_commit_empty(const u8 *message, size_t message_len);

#endif
