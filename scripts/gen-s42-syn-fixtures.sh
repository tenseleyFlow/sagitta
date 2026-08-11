#!/bin/sh
# Recreate Sprint 42's generated fixture variants.  The hand-written kitchen
# and trap fixtures remain the source; these copies give every language a
# broad, deterministic golden matrix without hiding the named edge cases.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

copy_fixture()
{
    source=$1
    target=$2
    comment=$3
    label=$4

    cp "$source" "$target"
    printf '\n%s Sprint 42 deterministic variant: %s\n' "$comment" "$label" \
        >> "$target"
}

for n in 08 09 10 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/python/01-kitchen.py \
        tests/syn/python/$n-matrix.py '#' "python-$n"
done

for n in 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/rust/01-kitchen.rs \
        tests/syn/rust/$n-matrix.rs '//' "rust-$n"
    copy_fixture tests/syn/go/01-kitchen.go \
        tests/syn/go/$n-matrix.go '//' "go-$n"
done

for n in 05 06 07 08 09; do
    copy_fixture tests/syn/javascript/01-kitchen.js \
        tests/syn/javascript/$n-matrix.js '//' "javascript-$n"
done
for n in 14 15 16 17 18; do
    copy_fixture tests/syn/javascript/10-kitchen.ts \
        tests/syn/javascript/$n-matrix.ts '//' "typescript-$n"
done

for n in 02 03 04 05 06 07 08 09; do
    copy_fixture tests/syn/fortran/01-kitchen.f90 \
        tests/syn/fortran/$n-matrix.f90 '!' "fortran-free-$n"
done
for n in 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/fortran/10-kitchen.f \
        tests/syn/fortran/$n-matrix.f '!' "fortran-fixed-$n"
done

for n in 02 03 04 05 06 07 08 09; do
    copy_fixture tests/syn/json/01-kitchen.json \
        tests/syn/json/$n-matrix.json '//' "json-$n"
done
for n in 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/json/10-kitchen.jsonc \
        tests/syn/json/$n-matrix.jsonc '//' "jsonc-$n"
done

for n in 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/yaml/01-kitchen.yml \
        tests/syn/yaml/$n-matrix.yml '#' "yaml-$n"
done

# Literal C0 bytes are required to exercise TOML's forbidden-control rules.
printf 'control = "a\001b"\nmulticontrol = """a\nmid\001tail\n"""\n' \
    > tests/syn/toml/02-control.toml
for n in 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
    copy_fixture tests/syn/toml/01-kitchen.toml \
        tests/syn/toml/$n-matrix.toml '#' "toml-$n"
done
