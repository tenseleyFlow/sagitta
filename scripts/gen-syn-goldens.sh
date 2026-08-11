#!/bin/sh
# Regenerate byte-exact syntax span goldens for every bundled definition.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 YEW" >&2
    exit 2
fi

yew=$1

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
    toml:tests/syn/toml:toml
do
    def=${spec%%:*}
    rest=${spec#*:}
    dir=${rest%%:*}
    ext=${rest#*:}
    for input in "$dir"/*."$ext"; do
        "$yew" syn dump runtime/syntax/"$def".fl --spans "$input" \
            > "${input%."$ext"}.spans"
    done
done
