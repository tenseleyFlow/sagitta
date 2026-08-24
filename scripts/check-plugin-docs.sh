#!/bin/sh
set -eu

tmp=${TMPDIR:-/tmp}/yew-plugin-docs.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

awk '
    /^ \* Plugins are Fletch/ { copying = 1 }
    copying {
        line = $0
        sub(/^ \* ?/, "", line)
        print line
        if (line == "short.")
            exit
    }
' src/mod/plug/plug.h >"$tmp/header"

awk '
    /^> Plugins are Fletch/ { copying = 1 }
    copying {
        line = $0
        sub(/^> ?/, "", line)
        print line
        if (line == "short.")
            exit
    }
' docs/plugins-authoring.md >"$tmp/guide"

if ! cmp -s "$tmp/header" "$tmp/guide"; then
    echo "plugin docs: trust contract quote drifted" >&2
    diff -u "$tmp/header" "$tmp/guide" >&2 || true
    exit 1
fi

ctx_rows=$(awk '
    /^This table is normative and frozen at plugin API 1:/ { table = 1; next }
    table && /^`ctx.command` accepts/ { exit }
    table && /^\| `ctx\./ { rows++ }
    END { print rows + 0 }
' docs/plugins-authoring.md)
[ "$ctx_rows" -eq 10 ] || {
    echo "plugin docs: expected 10 ctx reference rows, found $ctx_rows" >&2
    exit 1
}

for command in install update remove list; do
    grep -q "^### $command$" docs/pkg.md || {
        echo "plugin docs: missing yew pkg $command section" >&2
        exit 1
    }
done
grep -q '^### doctor' docs/pkg.md || {
    echo "plugin docs: missing yew pkg doctor section" >&2
    exit 1
}

grep -q 'This is not a cryptographic' docs/pkg.md || {
    echo "plugin docs: missing non-cryptographic hash statement" >&2
    exit 1
}

for status in 0 1 3 4; do
    grep -q "^| $status |" docs/pkg.md || {
        echo "plugin docs: missing exit status $status" >&2
        exit 1
    }
done

grep -q 'Sprint 59 assembles' docs/pkg.md || {
    echo "plugin docs: missing Sprint 59 man-assembly boundary" >&2
    exit 1
}

echo "plugin docs: contracts ok"
