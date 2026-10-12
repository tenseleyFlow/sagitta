#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
yew=${YEW_TUTOR_BIN:-$root/build/yew}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-tutor-cli.XXXXXX")
trap 'find "$tmp" -depth -delete' EXIT HUP INT TERM
mkdir -p "$tmp/home"

run_tutor()
{
    env -i \
        HOME="$tmp/home" \
        PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        XDG_STATE_HOME="$tmp/state" \
        YEW_RUNTIME_DIR="$root/runtime" \
        "$yew" tutor "$@"
}

expected='1 arrows    incomplete Arrows and the mode model
2 units     incomplete Units, selections, and multiple cursors
3 execute   incomplete E mode and the shell
4 search    incomplete Search, replace, and jumps
5 workspace incomplete Panes, tabs, groups, and resume
6 fletch    incomplete Recordings and Fletch
7 fuss      incomplete FUSS mode and git'

run_tutor_program="$root/runtime/tutor/tutor.fl"
"$yew" fl --dump-ast "$run_tutor_program" >"$tmp/tutor.ast"
grep -Fq '(let "tutor_schema" (lit int 1))' "$tmp/tutor.ast"

find "$root/runtime/tutor/text" -type f -name 'ch??.txt' -exec \
    grep -hEo 'ch[0-9][0-9]\.t[0-9]+' {} + | LC_ALL=C sort \
    >"$tmp/task-ids"
[ "$(wc -l <"$tmp/task-ids" | tr -d ' ')" -eq 49 ]
[ "$(uniq "$tmp/task-ids" | wc -l | tr -d ' ')" -eq 49 ]
chapter=1
while [ "$chapter" -le 7 ]; do
    prefix=$(printf 'ch%02d.t' "$chapter")
    [ "$(grep -Fc "$prefix" "$tmp/task-ids")" -eq 7 ]
    chapter=$((chapter + 1))
done

: >"$tmp/module-ids"
for module in "$root"/runtime/tutor/ch??-*.fl; do
    "$yew" fl -c "$module"
    name=$(basename "$module")
    expression=$(printf '%s\n' \
        'import io' \
        "import \"tutor/$name\" as chapter" \
        'for task in chapter.tasks { io.print(task.id) }')
    YEW_RUNTIME_DIR="$root/runtime" \
        "$yew" fl --caps none -e "$expression" >>"$tmp/module-ids"
done
LC_ALL=C sort -o "$tmp/module-ids" "$tmp/module-ids"
cmp "$tmp/task-ids" "$tmp/module-ids"
[ "$(grep -hEo 'check:[[:space:]]*fn' \
      "$root"/runtime/tutor/ch??-*.fl | wc -l | tr -d ' ')" -eq 49 ]
[ "$(grep -hEo 'keys:[[:space:]]*"' \
      "$root"/runtime/tutor/ch??-*.fl | wc -l | tr -d ' ')" -eq 49 ]
if grep -nE '\.(replace|replace_lines|insert|delete|save|append|prepend)\(|io\.(write|append|remove|mkdir)\(|(^|[[:space:]])(edit|error)[[:space:]{(]' \
    "$root"/runtime/tutor/ch??-*.fl >"$tmp/impure"; then
    echo 'tutor cli: chapter checker module contains a write or raise' >&2
    cat "$tmp/impure" >&2
    exit 1
fi

run_tutor --list >"$tmp/list-one"
printf '%s\n' "$expected" >"$tmp/expected"
cmp "$tmp/expected" "$tmp/list-one"
[ ! -e "$tmp/state" ] || {
    echo 'tutor cli: --list created state' >&2
    exit 1
}

if run_tutor 1 --check >"$tmp/out" 2>"$tmp/err"; then
    echo 'tutor cli: --check accepted an incomplete chapter' >&2
    exit 1
fi
[ ! -s "$tmp/out" ]
grep -Fxq 'yew tutor: chapter incomplete: arrows (0/7 tasks complete)' \
    "$tmp/err"
[ ! -e "$tmp/state" ] || {
    echo 'tutor cli: --check created state' >&2
    exit 1
}

run_tutor --reset --list >"$tmp/list-two"
cmp "$tmp/list-one" "$tmp/list-two"
[ "$(find "$tmp/state/yew/tutor" -type f -name 'ch??.txt' | wc -l | tr -d ' ')" \
  -eq 7 ]
[ "$(find "$tmp/state/yew" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')" \
  -eq 1 ]

printf '%s\n' changed >"$tmp/state/yew/tutor/ch01.txt"
run_tutor --reset --list >"$tmp/list-three"
cmp "$tmp/list-one" "$tmp/list-three"
grep -Fq 'YEW TUTOR 1/7' "$tmp/state/yew/tutor/ch01.txt"

if run_tutor --no-such-option >"$tmp/out" 2>"$tmp/err"; then
    echo 'tutor cli: accepted an unknown option' >&2
    exit 1
fi
grep -Fq 'yew tutor: unknown option: --no-such-option' "$tmp/err"

if run_tutor eighth >"$tmp/out" 2>"$tmp/err"; then
    echo 'tutor cli: accepted an unknown chapter' >&2
    exit 1
fi
grep -Fq 'yew tutor: unknown chapter: eighth' "$tmp/err"

printf '%s\n' '{schema: 1, chapters: nope}' \
    >"$tmp/state/yew/tutor/tutor.fl"
if run_tutor --list >"$tmp/out" 2>"$tmp/err"; then
    echo 'tutor cli: accepted corrupt progress' >&2
    exit 1
fi
grep -Fq 'yew tutor: cannot read tutor progress' "$tmp/err"

echo 'tutor cli: ok'
