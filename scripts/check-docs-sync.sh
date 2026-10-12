#!/bin/sh
set -eu

root=${YEW_DOCS_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
docs=${YEW_DOCS_DIR:-$root/docs}
manual=${YEW_DOCS_MANUAL:-$docs/manual}
man_dir=${YEW_DOCS_MAN:-$docs/man}
readme=${YEW_DOCS_README:-$root/README.md}
findings=${YEW_DOCS_FINDINGS:-$root/.docs/audits/findings.md}
owners=${YEW_DOCS_OWNERS:-$manual/.owners}
source=${YEW_DOCS_SOURCE:-$root/src}
runtime=${YEW_DOCS_RUNTIME:-$root/runtime/init.fl}
makefile=${YEW_DOCS_MAKEFILE:-$root/Makefile}
yew_bin=${YEW_DOCS_BIN:-$root/build/yew}
option_dump=${YEW_DOCS_OPTION_DUMP:-$root/build/tests/helpers/option_dump}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-docs-sync.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail()
{
    echo "check-docs-sync: $*" >&2
    exit 1
}

for path in "$docs" "$manual" "$man_dir" "$source"; do
    [ -d "$path" ] || fail "missing directory $path"
done
for path in "$readme" "$findings" "$owners" "$runtime" "$makefile" \
            "$yew_bin" "$option_dump"; do
    [ -e "$path" ] || fail "missing $path"
done

# The helper prints the linked descriptor table, so macro-backed constants
# are compared after C evaluation rather than duplicated in a shell parser.
"$option_dump" >"$tmp/source-options"
sed -n 's/^| `\([^`]*\)` | `\([^`]*\)` | \([^ |]*\) |$/\1|\2|\3/p' \
    "$manual/08-fletch.md" >"$tmp/manual-options"
[ -s "$tmp/manual-options" ] || fail "could not extract the option table"
cmp -s "$tmp/source-options" "$tmp/manual-options" || {
    diff -u "$tmp/source-options" "$tmp/manual-options" >&2 || true
    fail "compiled option defaults/scopes and manual differ"
}

# The man checker owns the exact environment, binding, option, and E-command
# inventories. Reuse its bidirectional comparisons without requiring host
# formatter tools in this source-synchronization target.
YEW_MAN_ROOT=$root \
YEW_MAN_DIR=$man_dir \
YEW_MAN_SRC=$source \
YEW_MAN_RUNTIME=$runtime \
YEW_MAN_MAKEFILE=$makefile \
YEW_MAN_BIN=$yew_bin \
YEW_MAN_SKIP_FORMATTERS=1 \
YEW_MAN_SKIP_RESOLVE=1 \
    "$root/scripts/check-man.sh" >/dev/null

# Resolve Markdown links relative to the document that contains them. URL,
# mail, and in-document anchors deliberately have no local filesystem target.
find "$docs" -type f -name '*.md' -print >"$tmp/markdown-files"
printf '%s\n' "$readme" >>"$tmp/markdown-files"
while IFS= read -r file; do
    grep -Eo '\]\([^)]+\)' "$file" 2>/dev/null || true
done <"$tmp/markdown-files" >"$tmp/link-rows"
while IFS= read -r row; do
    target=${row#']('}
    target=${target%')'}
    target=${target%%#*}
    case $target in
        ''|'#'*|http://*|https://*|mailto:*|ftp://*) continue ;;
    esac
    case $target in
        '<'*) target=${target#'<'}; target=${target%'>'} ;;
    esac
    found=0
    while IFS= read -r file; do
        if grep -Fq "]($target" "$file"; then
            path=$(dirname -- "$file")/$target
            [ -e "$path" ] || fail "$file: unresolved relative link $target"
            found=1
        fi
    done <"$tmp/markdown-files"
    [ "$found" -eq 1 ] || fail "could not locate relative link owner: $target"
done <"$tmp/link-rows"

# Documentation may cite only stable finding ids from the authoritative
# ledger. Sorting makes duplicate prose references harmless and deterministic.
find "$docs" -type f -exec grep -hEo 'YEW-F-[0-9][0-9][0-9]' {} + \
    2>/dev/null | LC_ALL=C sort -u >"$tmp/doc-findings" || true
grep -Eo 'YEW-F-[0-9][0-9][0-9]' "$findings" |
    LC_ALL=C sort -u >"$tmp/ledger-findings"
while IFS= read -r finding; do
    [ -n "$finding" ] || continue
    grep -Fxq "$finding" "$tmp/ledger-findings" ||
        fail "documentation cites unknown finding $finding"
done <"$tmp/doc-findings"

# Each ownership row must point at real source and a real manual chapter.
# The optional base comparison is used by PR CI: user-visible owned source
# cannot change alone unless the PR explicitly records no visible change.
while IFS=' ' read -r prefix chapter extra; do
    case $prefix in ''|'#'*) continue ;; esac
    [ -z "${extra:-}" ] || fail "malformed owners row: $prefix $chapter $extra"
    [ -e "$root/$prefix" ] || fail "owners source does not exist: $prefix"
    [ -f "$manual/$chapter" ] || fail "owners chapter does not exist: $chapter"
done <"$owners"

skip_owner_sync=0
case ${YEW_DOCS_NO_USER_VISIBLE_CHANGE:-0} in
    1|true) skip_owner_sync=1 ;;
esac
if [ -n "${YEW_DOCS_BASE:-}" ] && [ "$skip_owner_sync" -eq 0 ]; then
    if [ -n "${YEW_DOCS_CHANGED_FILE:-}" ]; then
        cp "$YEW_DOCS_CHANGED_FILE" "$tmp/changed"
    else
        git -C "$root" diff --name-only "$YEW_DOCS_BASE"...HEAD \
            >"$tmp/changed"
    fi
    while IFS=' ' read -r prefix chapter extra; do
        case $prefix in ''|'#'*) continue ;; esac
        touched=0
        while IFS= read -r changed; do
            case $changed in "$prefix"|"$prefix"*) touched=1 ;; esac
        done <"$tmp/changed"
        if [ "$touched" -eq 1 ] &&
           ! grep -Fxq "docs/manual/$chapter" "$tmp/changed"; then
            fail "$prefix changed without docs/manual/$chapter"
        fi
    done <"$owners"
fi

echo "check-docs-sync: ok"
