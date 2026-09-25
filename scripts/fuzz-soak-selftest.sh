#!/bin/sh
# Exercise a real 60-second worker without dirtying the committed corpus,
# then hold every campaign target to the worker's report contract: each
# must survive a short date-seeded campaign and append exactly one row.
set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 COVERAGE_FUZZ_INPUT [CAMPAIGN_TARGET...]" >&2
    exit 2
fi

binary=$1
shift
build_dir=$(dirname "$binary")
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-soak-selftest.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
ledger=$tmp_dir/ledger.md
admit_dir=$tmp_dir/admit
seed=580001
short_commit=$(git rev-parse --short=8 HEAD)
workflow=.github/workflows/fuzz-campaigns.yml

new_ledger()
{
    {
        echo '| Date | Commit | Lane | Target | Iterations | Seed | New edges | Total edges | Corpus size | Findings |'
        echo '|---|---|---|---|---:|---|---:|---:|---:|---|'
        echo
        echo '## Pinned schedule'
    } >"$1"
}

# One well-formed row for TARGET/SEED/commit, above the pinned marker.
check_row()
{
    row_ledger=$1
    row_target=$2
    row_seed=$3
    rows=$(grep -c "| \`$row_target\` |" "$row_ledger" || true)
    [ "$rows" -eq 1 ] &&
        grep -F "| $row_seed |" "$row_ledger" >/dev/null &&
        grep -F "\`$short_commit\`" "$row_ledger" >/dev/null &&
        awk -v t="| \`$row_target\` |" '
            index($0, t) { row = NR }
            /^## Pinned schedule$/ { marker = NR }
            END { exit !(row != 0 && marker != 0 && row < marker) }
        ' "$row_ledger"
}

# The campaign matrix and the coverage build must name the same targets,
# or a target could reach the nightly without passing the check below.
workflow_targets=$(sed -n "s/.*echo 'all=\\[\\(.*\\)\\]'.*/\\1/p" "$workflow" |
    tr -d '"' | tr ',' '\n' | LC_ALL=C sort)
given_targets=$(printf '%s\n' "$@" | LC_ALL=C sort)
if [ "$#" -ne 0 ] && { [ -z "$workflow_targets" ] ||
   [ "$workflow_targets" != "$given_targets" ]; }; then
    echo "soak-selftest: $workflow campaign targets differ from the" \
         "coverage build's" >&2
    printf '%s\n' "$workflow_targets" >"$tmp_dir/workflow.txt"
    printf '%s\n' "$given_targets" >"$tmp_dir/given.txt"
    diff -u "$tmp_dir/workflow.txt" "$tmp_dir/given.txt" >&2 || true
    exit 1
fi

new_ledger "$ledger"
scripts/fuzz-soak.sh "$build_dir" fuzz_input 60 "$seed" \
    "$ledger" "$admit_dir"
if ! check_row "$ledger" fuzz_input "$seed"; then
    echo "soak-selftest: worker did not append exactly one correct row" >&2
    cat "$ledger" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    echo "soak-selftest: ok"
    exit 0
fi

# The nightly seed is a UTC date; use one so the seed check is exercised
# with the same shape of number.  Stop at the first failure: a finding
# leaves tests/fuzz/crashes non-empty, which every later target refuses.
contract_seed=20260925
for target in "$@"; do
    target_ledger=$tmp_dir/$target.md
    new_ledger "$target_ledger"
    if ! YEW_SOAK_LANE=fuzz-nightly scripts/fuzz-soak.sh "$build_dir" \
            "$target" 1 "$contract_seed" "$target_ledger" \
            "$tmp_dir/admit-$target" >"$tmp_dir/$target.log" 2>&1 ||
       ! check_row "$target_ledger" "$target" "$contract_seed"; then
        echo "soak-selftest: $target failed a short campaign or its report" \
             "contract" >&2
        cat "$tmp_dir/$target.log" >&2
        exit 1
    fi
done
echo "soak-selftest: ok ($# campaign targets honour the report contract)"
