#!/bin/sh
#
# Sprint 34 DoD 5: the mutation choke point, enforced.
#
# THE LAW.  A Fletch call becomes an editor effect in exactly ONE place —
# fl_dispatch, via sag_cmd_invoke.  Nothing under src/fl/ may reach into
# TextBuf, EditCtx, CursorSet, the undo log or the registers directly,
# because a direct mutation would (a) skip sag_undo_record, breaking undo
# atomicity, and (b) skip the s13 recorder tap, making invariant 10
# quietly false.  Two invariants, one shortcut, and no symptom until a
# user loses work.
#
# WHY A SCRIPT AND NOT A COMMENT.  The sprint states this as a review
# rule; a review rule is a person remembering.  This is the same claim
# with teeth, and it runs in `make test`.
#
# THE repl.c ALLOWANCE, stated rather than hidden.  Sprint 32's REPL
# edits its OWN prompt line: a private TextBuf that is not a user
# document, carries no undo tree the user can reach, and is not
# recordable.  It genuinely uses sag_edit_insert on that line, and that
# is not the hazard above — no amount of prompt editing can lose a user's
# work or desynchronise a macro.  The allowance is per-FILE and narrow on
# purpose: a future flapi.c cannot hide behind it, and adding a second
# allowance should require the same argument in writing.
#
# Comments are exempt: prose that NAMES a banned symbol (including this
# header, and repl.c's own explanation of what it shares with the editor)
# is documentation, not a call.

set -eu

BANNED='sag_edit_insert|sag_edit_delete|sag_textbuf_|sag_undo_record|sag_reg_set|sag_reg_yank|sag_reg_delete|sag_cset_'
ALLOWED_FILE='src/fl/repl.c'

# Hits in src/fl/, minus the allowance, minus comment lines.  A comment
# line here is one whose first non-blank character starts a block comment
# or continues one (` * `), or is a // line — the project's house style
# for every explanatory line in these files.
choke_hits()
{
    root=${1:-src/fl}
    grep -rnE "$BANNED" "$root" 2>/dev/null |
        grep -v "^$ALLOWED_FILE:" |
        awk -F: '{
            line = $0
            sub(/^[^:]*:[0-9]*:/, "", line)
            sub(/^[ \t]+/, "", line)
            if (line ~ /^\*/) next
            if (line ~ /^\/\*/) next
            if (line ~ /^\/\//) next
            print $0
        }'
}

hits=$(choke_hits src/fl || true)
if [ -n "$hits" ]; then
    echo "fl-choke: a Fletch source mutates the editor directly." >&2
    echo "fl-choke: route it through a registered command and" >&2
    echo "fl-choke: sag_cmd_invoke — see s34 deliverable 3." >&2
    printf '%s\n' "$hits" >&2
    exit 1
fi

# SELF-TEST, the same discipline check-cmd-dispatch.sh uses: a gate that
# has never rejected anything is a gate nobody has tested.  Seed the exact
# violation DoD 5 names and require a rejection.
seed_dir=$(mktemp -d)
trap 'rm -rf "$seed_dir"' EXIT INT TERM
mkdir -p "$seed_dir/src/fl"
cat >"$seed_dir/src/fl/flapi.c" <<'SEED'
/* A comment naming sag_edit_insert must NOT trip the gate. */
void seeded(void)
{
    sag_edit_insert(&ec, at, bytes, len);
}
SEED
if [ -z "$(cd "$seed_dir" && choke_hits src/fl || true)" ]; then
    echo "fl-choke: the seeded direct sag_edit_insert was accepted" >&2
    exit 1
fi

echo "fl-choke: ok"
