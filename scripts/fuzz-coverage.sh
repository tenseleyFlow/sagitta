#!/bin/sh
# Sprint 58 section 6: build-tree isolation, neutrality, and edge report.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 PLAIN_BUILD COVERAGE_BUILD REPORT" >&2
    exit 2
fi

plain_build=$1
coverage_build=$2
report=$3
shared=${FUZZ_COV_SHARED_TARGETS:-}
standalone=${FUZZ_COV_STANDALONE_TARGETS:-}

if [ -z "$shared" ] || [ -z "$standalone" ]; then
    echo "fuzz-coverage: target lists were not provided" >&2
    exit 2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-cov.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
run_id=0

mkdir -p "$(dirname "$report")"
{
    echo "# Fuzz coverage snapshot"
    echo
    echo 'Generated deterministically by `make fuzz-cov`.'
    echo
    echo "| Target | Edges | Coverage hash | Corpus entries |"
    echo "|---|---:|---|---:|"
} >"$report"

run_target()
{
    run_output=$1
    shift
    run_id=$((run_id + 1))
    run_root=$tmp_dir/run-$run_id
    mkdir -p "$run_root/state" "$run_root/config" "$run_root/cache"
    XDG_STATE_HOME=$run_root/state \
    XDG_CONFIG_HOME=$run_root/config \
    XDG_CACHE_HOME=$run_root/cache \
        "$@" >"$run_output" 2>"$run_output.err"
}

check_neutrality()
{
    target=$1
    shift
    run_target "$tmp_dir/plain" "$plain_build/$target" "$@"
    run_target "$tmp_dir/coverage" "$coverage_build/$target" "$@"
    if ! cmp -s "$tmp_dir/plain" "$tmp_dir/coverage" ||
       ! cmp -s "$tmp_dir/plain.err" "$tmp_dir/coverage.err"; then
        echo "fuzz-coverage: instrumentation changed $target output" >&2
        diff -u "$tmp_dir/plain" "$tmp_dir/coverage" >&2 || true
        diff -u "$tmp_dir/plain.err" "$tmp_dir/coverage.err" >&2 || true
        exit 1
    fi
}

append_report()
{
    target=$1
    shift
    run_target "$tmp_dir/report" "$coverage_build/$target" "$@" \
        --coverage-report
    values=$(LC_ALL=C awk '
        /coverage edges=/ {
            edges = hash = corpus = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^edges=/) { edges = $i; sub(/^edges=/, "", edges) }
                if ($i ~ /^hash=/) { hash = $i; sub(/^hash=/, "", hash) }
                if ($i ~ /^corpus=/) { corpus = $i; sub(/^corpus=/, "", corpus) }
            }
            if (edges != "" && hash != "" && corpus != "")
                print edges, hash, corpus
        }
    ' "$tmp_dir/report")
    if [ "$(printf '%s\n' "$values" | wc -l | tr -d ' ')" -ne 1 ]; then
        echo "fuzz-coverage: malformed report from $target" >&2
        cat "$tmp_dir/report" >&2
        exit 1
    fi
    set -- $values
    printf '| `%s` | %s | `%s` | %s |\n' "$target" "$1" "$2" "$3" \
        >>"$report"
}

for target in $shared; do
    check_neutrality "$target" --corpus-only
    append_report "$target" --corpus-only
done

for target in $standalone; do
    case $target in
        fuzz_undo) args="--iters=10000 --seed=1" ;;
        fuzz_textbuf) args="--iters=6 --seed=1" ;;
        fuzz_units) args="--iters=100000 --seed=1" ;;
        *) echo "fuzz-coverage: no standalone recipe for $target" >&2; exit 2 ;;
    esac
    # Intentional word splitting: every recipe above is a fixed list of
    # whitespace-free arguments owned by this script.
    # shellcheck disable=SC2086
    check_neutrality "$target" $args
    # shellcheck disable=SC2086
    append_report "$target" $args
done

echo "fuzz-coverage: $(printf '%s\n' $shared $standalone | wc -l | tr -d ' ') targets neutral"
echo "fuzz-coverage: report $report"
