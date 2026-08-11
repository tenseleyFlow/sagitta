#!/bin/sh
# Keep whole Sprint 42 definitions as one-record syntax-compiler fuzz seeds.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

for name in python rust go javascript typescript fortran fortran_fixed \
            json jsonc yaml toml
do
    awk 'BEGIN { ORS = "" }
         !/^[[:space:]]*#/ { printf "%s", $0 }
         END { printf "\n" }' \
        runtime/syntax/"$name".fl \
        > tests/fuzz/corpus/syn_def/12-"$name".fl
done
