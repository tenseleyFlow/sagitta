#!/bin/sh
#
# Sprint 33 Testing Strategy: the runner's own tests.
#
# A TEST RUNNER THAT SILENTLY PASSES IS WORSE THAN NO RUNNER, so the
# eleven cases under tests/fletch/meta/ are expected outcomes of the
# runner itself -- five ways for a directive to fail, three
# configuration errors, an XPASS, a skip, and the one that has to
# pass: a directive inside a string literal, which must not fire.
#
# The totals are asserted as an exact line rather than case by case.
# A per-case check drifts into "any failure counts", and the whole
# point is that the runner distinguishes FAIL from CONFIG from SKIP.
set -eu

BUILD=${BUILD:-build}
RUNNER=${RUNNER:-$BUILD/fletch_run}
SAGITTA=${SAGITTA:-$BUILD/sagitta}
ROOT=tests/fletch/meta

WANT='fletch: total=11 pass=1 fail=6 skip=1 xfail=0 config=3'

out=$(LC_ALL=C "$RUNNER" --root "$ROOT" --sagitta "$SAGITTA" 2>&1 || true)
got=$(printf '%s\n' "$out" | grep '^fletch: total=' || true)

if [ "$got" != "$WANT" ]; then
    echo "check-fletch-meta: the runner's own outcomes changed" >&2
    echo "  want: $WANT" >&2
    echo "  got:  $got" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi

# The skip must be ANNOUNCED, exactly once (s01's discipline).
n=$(printf '%s\n' "$out" | grep -c '^HARNESS_SKIP ' || true)
if [ "$n" != "1" ]; then
    echo "check-fletch-meta: expected exactly one HARNESS_SKIP line, got $n" >&2
    exit 1
fi

# The XPASS must be named as such, not folded into the failure count
# silently -- a stale XFAIL is a different problem from a broken test.
if ! printf '%s\n' "$out" | grep -q '^XPASS xpass.fl:'; then
    echo "check-fletch-meta: an XFAIL that passes was not reported as XPASS" >&2
    exit 1
fi

# And the directive-in-a-string case must PASS, which is the property
# the whole comment-token design exists to provide.
if ! printf '%s\n' "$out" | grep -q '^PASS pass_directive_in_string.fl'; then
    echo "check-fletch-meta: a directive inside a string literal fired" >&2
    exit 1
fi

echo "check-fletch-meta: 11 cases, outcomes as expected"
