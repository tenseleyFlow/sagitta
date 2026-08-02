#!/bin/sh

set -eu

symbols()
{
    grep -rEh 'cmd_' src | tr -cs 'A-Za-z0-9_' '\n' | \
        grep -E '^cmd_[a-z0-9_]+$'
}

check_counts()
{
    sort | uniq -c | awk '
        $1 != 2 { print "dispatch: " $0 > "/dev/stderr"; bad = 1 }
        END { exit bad }
    '
}

symbols | check_counts

seed=$(symbols | sed -n '1p')
if [ -z "$seed" ]; then
    echo "dispatch: no cmd_* symbols found" >&2
    exit 1
fi
if { symbols; printf '%s\n' "$seed"; } | check_counts >/dev/null 2>&1; then
    echo "dispatch: seeded second call site was accepted" >&2
    exit 1
fi

definitions=$(grep -En \
    '^[A-Za-z_][A-Za-z0-9_ *]*\([^;]*\)[[:space:]]*$' \
    src/edit/keys_default.c || :)
if [ "$(printf '%s\n' "$definitions" | sed '/^$/d' | wc -l | tr -d ' ')" \
    -ne 1 ] || ! printf '%s\n' "$definitions" | \
    grep -F 'sag_keys_default_install' >/dev/null 2>&1; then
    echo "dispatch: keys_default.c must contain data plus one install function" >&2
    printf '%s\n' "$definitions" >&2
    exit 1
fi

echo "dispatch: named-command integrity ok"
