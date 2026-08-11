#!/bin/sh
# Keep every bundled definition as one syntax-compiler fuzz seed.
set -eu

LC_ALL=C
export LC_ALL
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-fuzz-seeds.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

awk -F '\t' '
    NR == 1 {
        if ($0 != "# definition\tlanguage\tsource-patterns\tgolden-rule\tscope\tcohort\tmin-goldens") bad = 1
        next
    }
    /^[[:space:]]*$/ || /^#/ { next }
    {
        if (NF != 7 || ($5 != "base" && $5 != "embed")) {
            bad = 1
            next
        }
        count = split($3, paths, ",")
        for (i = 1; i <= count; i++) {
            stars = paths[i]
            star_count = gsub(/\*/, "", stars)
            if (paths[i] !~ /^tests\/syn\/[A-Za-z0-9_.\/*-]+$/ ||
                paths[i] ~ /(^|\/)\.\.($|\/)/ || star_count > 1) bad = 1
        }
    }
    $5 == "base" {
        if (seen[$1]++) {
            printf "syntax fuzz seeds: duplicate base row for %s\n", $1 > "/dev/stderr"
            bad = 1
        }
        bases++
        print $1
    }
    END {
        if (bad || bases == 0) {
            print "syntax fuzz seeds: invalid manifest" > "/dev/stderr"
            exit 1
        }
    }
' tests/syn/manifest.tsv > "$tmp/stems"

while IFS= read -r stem; do
    definition=runtime/syntax/$stem.fl
    [ -f "$definition" ] || {
        echo "syntax fuzz seeds: missing definition: $definition" >&2
        exit 1
    }
    awk 'BEGIN { ORS = "" }
         !/^[[:space:]]*#/ { printf "%s", $0 }
         END { printf "\n" }' \
        "$definition" > "$tmp/12-$stem.fl"
done < "$tmp/stems"

rm -f tests/fuzz/corpus/syn_def/12-*.fl
for seed in "$tmp"/12-*.fl; do
    mv "$seed" tests/fuzz/corpus/syn_def/
done
