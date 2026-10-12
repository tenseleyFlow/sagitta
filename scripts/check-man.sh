#!/bin/sh
set -eu

root=${YEW_MAN_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
man_dir=${YEW_MAN_DIR:-$root/docs/man}
src_dir=${YEW_MAN_SRC:-$root/src}
runtime=${YEW_MAN_RUNTIME:-$root/runtime/init.fl}
deferrals=${YEW_MAN_DEFERRALS:-$root/.docs/audits/deferrals.md}
makefile=${YEW_MAN_MAKEFILE:-$root/Makefile}
yew_bin=${YEW_MAN_BIN:-$root/build/yew}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-man.XXXXXX")
stage=$(mktemp -d "${TMPDIR:-/tmp}/yew-man-stage.XXXXXX")
trap 'rm -rf "$tmp" "$stage"' EXIT HUP INT TERM

fail()
{
    echo "check-man: $*" >&2
    exit 1
}

require_file()
{
    [ -f "$1" ] || fail "missing $1"
}

for page in yew.1 yew-fl.1 yew-pkg.1 yew-regex.7; do
    require_file "$man_dir/$page"
done
require_file "$src_dir/args.c"
require_file "$src_dir/edit/cmd.c"
require_file "$runtime"
require_file "$makefile"
require_file "$yew_bin"

# Public top-level options are the accepted yew_args_parse rows.  The two
# exclusions are deliberate non-user seams: one forces the bug exit in tests,
# and one is an explicit reserved-option refusal until its implementation
# lands.  Neither may leak into --help or the man page.
sed -n 's/.*strcmp(arg, "\(--[a-z0-9-]*\)").*/\1/p' \
    "$src_dir/args.c" |
    grep -v -e '^--selftest-bug$' -e '^--batch-strict$' |
    LC_ALL=C sort -u >"$tmp/source-options"

"$yew_bin" --help |
    sed -n 's/^  \(--[^[:space:]]*\).*/\1/p' |
    LC_ALL=C sort -u >"$tmp/help-options"

awk '
    /^\.SH OPTIONS$/ { inside = 1; next }
    inside && /^\.SH / { exit }
    inside { print }
' "$man_dir/yew.1" >"$tmp/options-section"
grep -Eo '\\-\\-[a-z][a-z0-9-]*|\\-\\-' "$tmp/options-section" |
    tr -d '\\' | LC_ALL=C sort -u >"$tmp/man-options"

cmp -s "$tmp/source-options" "$tmp/help-options" || {
    diff -u "$tmp/source-options" "$tmp/help-options" >&2 || true
    fail "yew_args_parse and --help options differ"
}
cmp -s "$tmp/source-options" "$tmp/man-options" || {
    diff -u "$tmp/source-options" "$tmp/man-options" >&2 || true
    fail "yew_args_parse and yew(1) OPTIONS differ"
}

# A raw ASCII double hyphen in a man source is not copy-safe.  Every option
# spelling must use the troff minus escape; literal prose such as C-- is not
# part of yew's pages and therefore needs no exception list.
if grep -nE '(^|[[:space:]"(])--[a-z][a-z0-9-]*' "$man_dir"/*.1 \
    >"$tmp/raw-hyphens"; then
    cat "$tmp/raw-hyphens" >&2
    fail "option uses ASCII hyphens instead of \\-"
fi

# Every source-tree YEW_* string literal is part of the operational or
# diagnostic environment surface.  Keep the documentation exact in both
# directions so stale test seams do not linger forever.
find "$src_dir" -type f \( -name '*.c' -o -name '*.h' \) \
    -exec grep -hEo '"YEW_[A-Z0-9_]+"' {} + |
    tr -d '"' | LC_ALL=C sort -u >"$tmp/source-env"
for page in "$man_dir/yew.1" "$man_dir/yew-fl.1" \
            "$man_dir/yew-pkg.1"; do
    awk '
        /^\.SH ENVIRONMENT$/ { inside = 1; next }
        inside && /^\.SH / { inside = 0 }
        inside { print }
    ' "$page"
done >"$tmp/environment-sections"
grep -hEo 'YEW_[A-Z0-9_]+' "$tmp/environment-sections" |
    LC_ALL=C sort -u >"$tmp/man-env"
cmp -s "$tmp/source-env" "$tmp/man-env" || {
    diff -u "$tmp/source-env" "$tmp/man-env" >&2 || true
    fail "source and documented YEW_* variables differ"
}

# Compare all registry entries that have a colon abbreviation, including
# their argument and range metadata.  The sed expression intentionally only
# accepts the one-line table shape: changing that shape without changing this
# checker must fail rather than silently document zero commands.
sed -n '/static const BuiltinMeta builtin_meta\[\]/,/^};/ s/^[[:space:]]*{"\([^"]*\)", "\([^"]*\)", \(YEW_RP_[A-Z]*\), "\([^"]*\)"},/\4|\1|\3|\2/p' \
    "$src_dir/edit/cmd.c" |
    sed -e 's/YEW_RP_FORBID/forbidden/' \
        -e 's/YEW_RP_OPT/optional/' \
        -e 's/YEW_RP_LINE/current-line default/' \
        -e 's/YEW_RP_BUFFER/whole-buffer default/' \
        -e 's/YEW_RP_REQUIRED/required/' \
        -e 's/|$/|-/' |
    LC_ALL=C sort >"$tmp/source-commands"
[ -s "$tmp/source-commands" ] || fail "could not extract E command metadata"
sed -n 's/^CMD //p' "$man_dir/yew.1" |
    LC_ALL=C sort >"$tmp/man-commands"
cmp -s "$tmp/source-commands" "$tmp/man-commands" || {
    diff -u "$tmp/source-commands" "$tmp/man-commands" >&2 || true
    fail "E command metadata and yew(1) differ"
}

# runtime/init.fl is intentionally simple enough that quote fields 2, 4 and
# 6 are mode, key and command.  Assert the extraction count before comparing
# so a future multiline bind cannot disappear from both sides unnoticed.
awk -F '"' '/^[[:space:]]*bind\("/ { print $2 "|" $4 "|" $6 }' \
    "$runtime" | LC_ALL=C sort >"$tmp/source-bindings"
source_bind_count=$(grep -Ec '^[[:space:]]*bind\(' "$runtime")
extracted_bind_count=$(wc -l <"$tmp/source-bindings" | tr -d ' ')
[ "$source_bind_count" -eq "$extracted_bind_count" ] ||
    fail "could not extract every runtime binding"
sed -n 's/^BIND \([^|]*|[^|]*|[^|]*\).*/\1/p' "$man_dir/yew.1" |
    LC_ALL=C sort >"$tmp/man-bindings"
cmp -s "$tmp/source-bindings" "$tmp/man-bindings" || {
    diff -u "$tmp/source-bindings" "$tmp/man-bindings" >&2 || true
    fail "runtime bindings and yew(1) KEYS differ"
}

"$yew_bin" --help-cmds | awk '{ print $1 }' |
    LC_ALL=C sort -u >"$tmp/built-commands"
cut -d '|' -f 3 "$tmp/man-bindings" | LC_ALL=C sort -u |
while IFS= read -r command; do
    grep -Fxq "$command" "$tmp/built-commands" ||
        fail "documented binding command does not resolve: $command"
done

# A source diagnostic that promises a man page must name a checked-in page
# that the install recipe actually copies.
find "$src_dir" -type f \( -name '*.c' -o -name '*.h' \) \
    -exec grep -hEo 'see man [a-z0-9-]+' {} + |
    awk '{ print $3 }' | LC_ALL=C sort -u >"$tmp/see-man"
while IFS= read -r page; do
    [ -n "$page" ] || continue
    found=
    for section in 1 2 3 4 5 6 7 8 9; do
        if [ -f "$man_dir/$page.$section" ]; then
            found=$man_dir/$page.$section
            break
        fi
    done
    [ -n "$found" ] || fail "source promises missing man page: $page"
    grep -Fq "docs/man/$(basename "$found")" "$makefile" ||
        fail "promised man page is not installed: $page"
done <"$tmp/see-man"

awk '
    /^\.SH LIMITATIONS$/ { inside = 1; next }
    inside && /^\.SH / { exit }
    inside { print }
' "$man_dir/yew.1" >"$tmp/limitations"
for phrase in \
    'no Unicode normalization' \
    'right-to-left text' \
    'lookaround and backreferences' \
    'snippet-expansion' \
    'rendered Markdown' \
    'interactive terminal buffer' \
    'filesystem watcher' \
    'Fuzzy matching folds ASCII case only' \
    'literal ampersand' \
    'aliases' \
    'OSC 52 is write-only'; do
    grep -Fq "$phrase" "$tmp/limitations" ||
        fail "LIMITATIONS misses permanent non-goal: $phrase"
done

# check-deferrals.sh owns the paragraph parser and exact release-note text
# comparison.  Point it at this page so the two gates share one law.
if [ -f "$deferrals" ] && grep -q '^## YEW-F-' "$deferrals"; then
    YEW_DEFERRAL_MAN="$man_dir/yew.1" \
    YEW_DEFERRAL_FILE="$deferrals" \
        "$root/scripts/check-deferrals.sh" >/dev/null
fi

mkdir -p "$stage/man1" "$stage/man7"
cp "$man_dir/yew.1" "$man_dir/yew-fl.1" "$man_dir/yew-pkg.1" \
    "$stage/man1/"
cp "$man_dir/yew-regex.7" "$stage/man7/"

if [ "${YEW_MAN_SKIP_FORMATTERS:-0}" != 1 ]; then
    command -v mandoc >/dev/null 2>&1 || fail "mandoc is required"
    mandoc -T lint "$man_dir/yew.1" "$man_dir/yew-fl.1" \
        "$man_dir/yew-pkg.1" >/dev/null
    # mandoc calls every .so redirect fragile as a style warning.  The
    # one-line redirect is the pinned contract here, so ignore exactly that
    # diagnostic while rejecting a failed redirect or any second warning.
    (cd "$stage" && mandoc -T lint man7/yew-regex.7) \
        >"$tmp/redirect-lint" 2>&1 || true
    sed '/WARNING: \.so is fragile, better use ln(1): so man1\/yew\.1$/d' \
        "$tmp/redirect-lint" >"$tmp/redirect-errors"
    [ ! -s "$tmp/redirect-errors" ] || {
        cat "$tmp/redirect-errors" >&2
        fail "mandoc rejected yew-regex(7)"
    }
    if command -v groff >/dev/null 2>&1; then
        (cd "$stage" && groff -man -z -ww man1/yew.1 man1/yew-fl.1 \
            man1/yew-pkg.1 man7/yew-regex.7) >/dev/null
    elif [ "${YEW_REQUIRE_GROFF:-0}" = 1 ]; then
        fail "groff is required in this lane"
    else
        echo "check-man: groff unavailable; groff lint skipped" >&2
    fi
fi

if [ "${YEW_MAN_SKIP_RESOLVE:-0}" != 1 ]; then
    command -v man >/dev/null 2>&1 || fail "man is required"
    command -v col >/dev/null 2>&1 || fail "col is required"
    MANPAGER=cat PAGER=cat man -M "$stage" yew-regex \
        2>"$tmp/resolve-warnings" |
        col -b >"$tmp/regex-rendered"
    sed '/WARNING: \.so is fragile, better use ln(1): so man1\/yew\.1$/d' \
        "$tmp/resolve-warnings" >"$tmp/resolve-errors"
    [ ! -s "$tmp/resolve-errors" ] || {
        cat "$tmp/resolve-errors" >&2
        fail "man emitted an unexpected redirect diagnostic"
    }
    grep -Fq 'modal terminal editor and its language' \
        "$tmp/regex-rendered" ||
        fail "staged man yew-regex did not open yew(1)"
fi

echo "check-man: ok"
