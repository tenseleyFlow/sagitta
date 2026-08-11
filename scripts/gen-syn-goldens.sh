#!/bin/sh
# Regenerate byte-exact syntax span goldens for every bundled definition.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] ||
   { [ "$#" -eq 2 ] && [ "$2" != "all" ] && [ "$2" != "embed" ]; }; then
    echo "usage: $0 YEW [all|embed]" >&2
    exit 2
fi

yew=$1
scope=${2:-all}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-goldens.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for spec in \
    c:tests/syn/c:c \
    fletch:tests/syn/fletch:fl \
    ini:tests/syn/ini:ini \
    make:tests/syn/make:mk \
    markdown:tests/syn/markdown:md \
    sh:tests/syn/sh:sh \
    python:tests/syn/python:py \
    rust:tests/syn/rust:rs \
    go:tests/syn/go:go \
    javascript:tests/syn/javascript:js \
    typescript:tests/syn/javascript:ts \
    fortran:tests/syn/fortran:f90 \
    fortran_fixed:tests/syn/fortran:f \
    json:tests/syn/json:json \
    jsonc:tests/syn/json:jsonc \
    yaml:tests/syn/yaml:yml \
    toml:tests/syn/toml:toml \
    markdown:tests/syn/embed/markdown:md \
    html:tests/syn/embed/html:html \
    css:tests/syn/embed/css:css \
    make:tests/syn/embed/make:mk \
    sh:tests/syn/embed/sh:sh \
    javascript:tests/syn/embed/javascript:js \
    typescript:tests/syn/embed/typescript:ts
do
    def=${spec%%:*}
    rest=${spec#*:}
    dir=${rest%%:*}
    ext=${rest#*:}
    if [ "$scope" = embed ]; then
        case "$dir" in tests/syn/embed/*) ;; *) continue ;; esac
    fi
    for input in "$dir"/*."$ext"; do
        output=$tmp/$(printf '%s' "$input" | tr '/' '_').spans
        "$yew" syn dump runtime/syntax/"$def".fl --spans "$input" \
            > "$output"
        mv "$output" "${input%."$ext"}.spans"
    done
done
