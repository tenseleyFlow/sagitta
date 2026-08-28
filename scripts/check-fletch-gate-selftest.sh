#!/bin/sh
#
# Sprint 33 DoD 2: every one of the seven coverage checks is PROVEN to
# fire by a seeded violation.
#
# A gate nobody has watched fail is a gate nobody knows works.  Each
# case below breaks exactly one thing in a scratch copy of the suite
# and requires the matching check number in the output -- not merely a
# non-zero exit, which any of the seven would produce.
#
# The scratch tree is a COPY: seeding a violation in the real suite and
# undoing it afterwards leaves the tree broken if the script dies in
# between, and this runs in CI.
set -eu

BUILD=${BUILD:-build}
RUNNER=$(cd "$(dirname "$0")/.." && pwd)/${RUNNER:-$BUILD/fletch_run}
YEW=$(cd "$(dirname "$0")/.." && pwd)/${YEW:-$BUILD/yew}
SPEC=.docs/fletch-spec.md

if [ ! -r "$SPEC" ]; then
    echo "check-fletch-gate-selftest: $SPEC is unreadable; the spec is the" \
         "gate's source of truth" >&2
    exit 2
fi

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM
# The suite is copied to the SAME relative path it normally lives at:
# the spec's `**Conformance:**` lines name `tests/fletch/...` from the
# repo root, so a scratch tree with a different shape makes check 6
# fire on every case and masks whichever one is under test.
mkdir -p "$tmp/tests"
cp -r tests/fletch "$tmp/tests/fletch"
cp "$SPEC" "$tmp/spec.md"

fails=0

# Runs the gate over the scratch tree and requires `check N:` in it.
expect_check() {
    want=$1
    what=$2
    out=$(cd "$tmp" && LC_ALL=C "$RUNNER" --check --root tests/fletch \
                          --spec spec.md --yew "$YEW" 2>&1 || true)
    if printf '%s\n' "$out" | grep -q "^check $want:"; then
        echo "  ok  check $want fires: $what"
    else
        echo "  FAIL check $want did not fire: $what" >&2
        printf '%s\n' "$out" | head -5 >&2
        fails=$((fails + 1))
    fi
    # Put the tree back for the next case.
    rm -rf "$tmp/tests/fletch" "$tmp/spec.md"
    cp -r tests/fletch "$tmp/tests/fletch"
    cp "$SPEC" "$tmp/spec.md"
}

# POSIX sed has no in-place flag, and BSD sed interprets the GNU `-i EXPR`
# spelling as a backup suffix.  Rewrite only scratch files explicitly.
rewrite() {
    expression=$1
    file=$2
    rewritten=$file.rewritten

    sed "$expression" "$file" >"$rewritten"
    mv "$rewritten" "$file"
}

echo "check-fletch-gate-selftest: seeding one violation per check"

# 1: a spec section with no test.  Adding a section to the spec is the
#    real-world trigger -- amend §16 and forget the fixture.
printf '\n## \302\24717 A section nobody tested\n\nBody.\n' >>"$tmp/spec.md"
expect_check 1 "a spec section with no test"

# 2: a grammar production with no COVERS token.
#
#    Seeded by giving the SPEC a new production rather than by deleting
#    a token from a file, which is both the real-world trigger and the
#    only reliable seed: most nonterminals are named by two files, so
#    removing one COVERS leaves the other still satisfying the check.
printf '\n```ebnf\nseeded_production = "x" ;\n```\n' >>"$tmp/spec.md"
expect_check 2 "a grammar production with no COVERS token"

# 3: a native with no COVERS token.
rewrite 's/ str\.cmp$//' "$tmp/tests/fletch/04-values-str.fl"
expect_check 3 "a native with no COVERS token"

# 4: an error kind with no ERROR_KIND case.
rm -f "$tmp/tests/fletch/errors/kind_div.fl"
rewrite 's/^# COVERS: kind:user kind:type kind:index kind:key kind:div kind:arity$/# COVERS: kind:user kind:type kind:index kind:key kind:arity/' \
    "$tmp/tests/fletch/09-errors.fl"
rewrite 's/^# COVERS: expr or_e and_e eq_e rel_e add_e mul_e unary postfix primary$/# COVERS: expr or_e and_e eq_e rel_e add_e mul_e unary postfix primary/' \
    "$tmp/tests/fletch/05-expressions.fl"
rewrite 's/^# COVERS: kind:div$//' "$tmp/tests/fletch/05-expressions.fl"
expect_check 4 "an error kind with no case"

# 5: an opcode with no COVERS token.
rewrite 's/ op:HALT op:NOT_NIL$//' "$tmp/tests/fletch/06-statements.fl"
expect_check 5 "an opcode with no COVERS token"

# 6a: a Conformance: line naming a file that does not exist.
rm -f "$tmp/tests/fletch/16-amendments.fl"
expect_check 6 "a Conformance: target that does not exist"

# 6b: 14-example.fl drifting from the spec's own §14 block.
rewrite '2s/^import str$/import  str/' "$tmp/tests/fletch/14-example.fl"
expect_check 6 "14-example.fl drifting from the spec block"

# 7 is the shell wrapper's git diff, so it is seeded against the
# committed ledger rather than the scratch tree.
stale=$(mktemp) || exit 2
sed 's/^-- totals:.*/-- totals: sections 1\/16/' tests/fletch/ledger.txt >"$stale"
if cmp -s "$stale" tests/fletch/ledger.txt; then
    echo "  FAIL check 7 could not be seeded (ledger has no totals line)" >&2
    fails=$((fails + 1))
else
    fresh=$(mktemp) || exit 2
    LC_ALL=C "$RUNNER" --ledger --yew "$YEW" >"$fresh"
    if cmp -s "$stale" "$fresh"; then
        echo "  FAIL check 7 did not notice a stale ledger" >&2
        fails=$((fails + 1))
    else
        echo "  ok  check 7 fires: a stale ledger.txt"
    fi
    rm -f "$fresh"
fi
rm -f "$stale"

if [ "$fails" -ne 0 ]; then
    echo "check-fletch-gate-selftest: $fails checks did not fire" >&2
    exit 1
fi
echo "check-fletch-gate-selftest: all seven checks proven to fire"
