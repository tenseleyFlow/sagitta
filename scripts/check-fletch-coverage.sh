#!/bin/sh
#
# Sprint 33 §3: the coverage gate.
#
# Seven checks, and the point of all of them is that THE SPEC IS THE
# DRIVER.  Amending .docs/fletch-spec.md without adding a test breaks
# the build in the same commit, which is the only moment anyone is
# willing to write the test.
#
#   1  every `## §N` heading has >= 1 file whose `# SPEC:` names it
#   2  every nonterminal defined in an EBNF block appears in a COVERS
#   3  every native from `yew fl --list-natives` appears in a COVERS
#   4  every §9 error kind has an `# ERROR_KIND:` case and a COVERS
#   5  every opcode appears in a `# COVERS: op:NAME`
#   6  every `**Conformance:**` target exists, and 14-example.fl still
#      starts with the spec's §14 block byte for byte
#   7  the committed ledger.txt is what the runner regenerates
#
# Checks 1-6 live in the runner, which already has the spec parsed and
# every directive in hand.  Check 7 is here because it is a git diff.
set -eu

BUILD=${BUILD:-build}
RUNNER=${RUNNER:-$BUILD/fletch_run}
YEW=${YEW:-$BUILD/yew}
LEDGER=tests/fletch/ledger.txt

if [ ! -x "$RUNNER" ]; then
    echo "check-fletch-coverage: $RUNNER is not built (make $RUNNER)" >&2
    exit 2
fi
if [ ! -x "$YEW" ]; then
    echo "check-fletch-coverage: $YEW is not built (make)" >&2
    exit 2
fi

rc=0

# Checks 1-6.
if ! LC_ALL=C "$RUNNER" --check --yew "$YEW"; then
    rc=1
fi

# Check 7: regenerate and diff.  Written to a temporary file and
# compared rather than overwriting the committed one, so a failing run
# leaves the tree exactly as it found it -- a gate that "fixes" the
# artifact it is checking reports green on the second run and has told
# you nothing.
tmp=$(mktemp) || exit 2
trap 'rm -f "$tmp"' EXIT INT TERM
LC_ALL=C "$RUNNER" --ledger --yew "$YEW" >"$tmp"
if ! cmp -s "$tmp" "$LEDGER"; then
    echo "check 7: $LEDGER is stale (run 'make fletch-ledger' and commit it)"
    diff -u "$LEDGER" "$tmp" | head -40 || true
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "check-fletch-coverage: all seven checks pass"
fi
exit "$rc"
