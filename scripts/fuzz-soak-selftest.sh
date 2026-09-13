#!/bin/sh
# Exercise a real 60-second worker without dirtying the committed corpus.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 COVERAGE_FUZZ_INPUT" >&2
    exit 2
fi

binary=$1
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-soak-selftest.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
ledger=$tmp_dir/ledger.md
admit_dir=$tmp_dir/admit
seed=580001
short_commit=$(git rev-parse --short=8 HEAD)

{
    echo '| Date | Commit | Lane | Target | Iterations | Seed | New edges | Total edges | Corpus size | Findings |'
    echo '|---|---|---|---|---:|---|---:|---:|---:|---|'
    echo
    echo '## Pinned schedule'
} >"$ledger"
scripts/fuzz-soak.sh "$(dirname "$binary")" fuzz_input 60 "$seed" \
    "$ledger" "$admit_dir"
rows=$(grep -c '| `fuzz_input` |' "$ledger" || true)
if [ "$rows" -ne 1 ] || ! grep -F "| $seed |" "$ledger" >/dev/null ||
   ! grep -F "\`$short_commit\`" "$ledger" >/dev/null ||
   ! awk '
       /\| `fuzz_input` \|/ { row = NR }
       /^## Pinned schedule$/ { marker = NR }
       END { exit !(row != 0 && marker != 0 && row < marker) }
   ' "$ledger"; then
    echo "soak-selftest: worker did not append exactly one correct row" >&2
    cat "$ledger" >&2
    exit 1
fi
echo "soak-selftest: ok"
