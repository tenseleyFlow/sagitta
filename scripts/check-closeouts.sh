#!/bin/sh
# Sprint 59 section 2: prove every audit-front closeout covers its sprint DoD.
set -eu

if ! repo=$(git rev-parse --show-toplevel 2>/dev/null); then
    echo "check-closeouts: not inside a Git worktree" >&2
    exit 2
fi

closeout_dir=${YEW_CLOSEOUT_DIR:-$repo/.docs/audits}
sprint_root=${YEW_CLOSEOUT_SPRINT_ROOT:-$repo/.docs/sprints}
ledger=${YEW_CLOSEOUT_LEDGER:-$repo/.docs/audits/findings.md}
map_override=${YEW_CLOSEOUT_MAP:-}
tab=$(printf '\t')

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-closeouts.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
map=$tmp_dir/map
if [ -n "$map_override" ]; then
    cp "$map_override" "$map"
else
    cat >"$map" <<'EOF'
F01|unicode|02
F02|terminal|03 04 05 06
F03|text|07 08 09 10 11 12
F04|modal|13 14 15 16 17
F05|exec|18 19
F06|regex|20 21
F07|ui|22 23 24 25 26 27
F08|fletch|28 29 30 31 32 33 34 36
F09|recorder|35 38
F10|syntax|39 40 41 42
F11|lsp|43 44 45 46 47
F12|ai|48 49 50
F13|git|51 52 53
F14|plugins|54 55
F15|ci|00 01 37 56 57
EOF
fi

failures=0
fail()
{
    echo "check-closeouts: $*" >&2
    failures=$((failures + 1))
}

sprint_file()
{
    sprint=$1
    if [ "$sprint" = 57 ] &&
            [ -f "$sprint_root/13-performance/s57-size-and-embedded.md" ]; then
        printf '%s\n' "$sprint_root/13-performance/s57-size-and-embedded.md"
        return
    fi
    matches=$tmp_dir/sprint-$sprint
    find "$sprint_root" -type f -name "s$sprint-*.md" -print |
        LC_ALL=C sort >"$matches"
    count=$(wc -l <"$matches" | tr -d ' ')
    if [ "$count" -ne 1 ]; then
        fail "s$sprint resolves to $count sprint files"
        return 1
    fi
    sed -n '1p' "$matches"
}

extract_dod()
{
    sprint=$1
    file=$2
    LC_ALL=C awk -v sprint="s$sprint" '
        /^## Definition of Done$/ { in_dod = 1; next }
        in_dod && /^## / { exit }
        in_dod && /^[0-9]+[.] / {
            number = $1
            sub(/[.]$/, "", number)
            text = $0
            sub(/^[0-9]+[.] /, "", text)
            print sprint "." number "\t" text
            found++
        }
        END { if (found == 0) exit 1 }
    ' "$file"
}

parse_closeout()
{
    file=$1
    keys=$2
    waivers=$3
    LC_ALL=C awk -v source="$file" -v keys="$keys" -v waivers="$waivers" '
        function report(message) {
            print "check-closeouts: " source ": " message > "/dev/stderr"
            errors++
        }
        function finish(    copy, id) {
            if (key == "") return
            print key >> keys
            if (checked) {
                if (entry !~ /EVIDENCE:[[:space:]]*[^[:space:]]/)
                    report(key " is checked without EVIDENCE")
            } else {
                if (entry !~ /VERDICT:[[:space:]]*(blocking|waived:)/)
                    report(key " is unchecked without a blocking/waived VERDICT")
                if (entry ~ /VERDICT:[[:space:]]*waived:/) {
                    if (entry !~ /VERDICT:[[:space:]]*waived:[[:space:]]*[^[:space:]]/)
                        report(key " has an empty waiver justification")
                    copy = entry
                    while (match(copy, /YEW-F-[0-9][0-9][0-9]/)) {
                        id = substr(copy, RSTART, RLENGTH)
                        print key "\t" id >> waivers
                        copy = substr(copy, RSTART + RLENGTH)
                    }
                }
            }
            key = ""
            entry = ""
        }
        /^- \[[ xX]\] s[0-9][0-9][.][0-9]+([[:space:]]|$)/ {
            finish()
            line = $0
            checked = substr(line, 4, 1) != " "
            sub(/^- \[[ xX]\] /, "", line)
            split(line, fields, /[[:space:]]+/)
            key = fields[1]
            entry = $0
            next
        }
        key != "" { entry = entry "\n" $0 }
        END { finish(); if (errors != 0) exit 1 }
    ' "$file"
}

severity_for()
{
    finding=$1
    LC_ALL=C awk -F '|' -v want="$finding" '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s)
            sub(/[[:space:]]+$/, "", s)
            return s
        }
        /^\|[[:space:]]*YEW-F-/ {
            if (trim($2) == want) { print trim($3); found = 1; exit }
        }
        END { if (!found) exit 1 }
    ' "$ledger"
}

reviewer_fixed_front()
{
    front=$1
    reviewer=$2
    [ -f "$ledger" ] || return 1
    commits=$tmp_dir/$front-fix-commits
    LC_ALL=C awk -F '|' -v front="$front" '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s)
            sub(/[[:space:]]+$/, "", s)
            return s
        }
        /^\|[[:space:]]*YEW-F-/ {
            row_front = trim($5)
            title = trim($6)
            if (row_front !~ ("^" front "([[:space:]]|$)")) next
            if (match(title, /fixed [0-9-]+ in `[0-9a-f]+`/)) {
                commit = substr(title, RSTART, RLENGTH)
                sub(/^fixed [0-9-]+ in `/, "", commit)
                sub(/`$/, "", commit)
                print commit
            }
        }
    ' "$ledger" | LC_ALL=C sort -u >"$commits"
    while IFS= read -r commit; do
        [ -n "$commit" ] || continue
        author=$(git -C "$repo" show -s --format='%an <%ae>' "$commit" 2>/dev/null || true)
        name=$(git -C "$repo" show -s --format='%an' "$commit" 2>/dev/null || true)
        email=$(git -C "$repo" show -s --format='%ae' "$commit" 2>/dev/null || true)
        if [ "$reviewer" = "$author" ] || [ "$reviewer" = "$name" ] ||
                [ "$reviewer" = "$email" ]; then
            return 0
        fi
    done <"$commits"
    return 1
}

front_count=0
while IFS='|' read -r front name sprints; do
    [ -n "$front" ] || continue
    front_count=$((front_count + 1))
    expected=$tmp_dir/$front-expected
    actual=$tmp_dir/$front-actual
    waivers=$tmp_dir/$front-waivers
    : >"$expected"
    : >"$actual"
    : >"$waivers"
    for sprint in $sprints; do
        if file=$(sprint_file "$sprint"); then
            if ! extract_dod "$sprint" "$file" >>"$expected"; then
                fail "$file has no numbered Definition of Done items"
            fi
        fi
    done

    closeout=$closeout_dir/closeout-$front-$name.md
    if [ ! -f "$closeout" ]; then
        fail "missing $closeout"
        continue
    fi
    if ! parse_closeout "$closeout" "$actual" "$waivers"; then
        failures=$((failures + 1))
    fi

    cut -f1 "$expected" | LC_ALL=C sort >"$expected.keys"
    LC_ALL=C sort "$actual" >"$actual.keys"
    while IFS= read -r key; do
        count=$(grep -Fxc "$key" "$actual.keys" || true)
        if [ "$count" -ne 1 ]; then
            fail "$closeout carries $key $count times (expected exactly once)"
        fi
    done <"$expected.keys"
    LC_ALL=C comm -13 "$expected.keys" "$actual.keys" >"$actual.extra"
    if [ -s "$actual.extra" ]; then
        while IFS= read -r key; do
            fail "$closeout carries unexpected DoD item $key"
        done <"$actual.extra"
    fi

    while IFS="$tab" read -r key finding; do
        [ -n "$finding" ] || continue
        if ! severity=$(severity_for "$finding"); then
            fail "$closeout $key waives unknown finding $finding"
            continue
        fi
        case $severity in
            C|H) fail "$closeout $key waives $severity finding $finding" ;;
        esac
    done <"$waivers"

    final=$(awk 'NF { line = $0 } END { print line }' "$closeout")
    case $final in
        'READY — '*|'READY - '*) state=READY ;;
        'NOT READY — '*|'NOT READY - '*) state='NOT READY' ;;
        *)
            fail "$closeout must end in READY or NOT READY plus one sentence"
            state=INVALID
            ;;
    esac
    case $final in
        *[.!?]) ;;
        *) fail "$closeout final verdict is not a complete sentence" ;;
    esac

    reviewer=$(sed -n 's/^Reviewer:[[:space:]]*//p' "$closeout")
    if [ "$(printf '%s\n' "$reviewer" | awk 'NF { n++ } END { print n+0 }')" -ne 1 ]; then
        fail "$closeout must name exactly one reviewer"
    elif [ "$state" = READY ] && [ "$reviewer" = UNASSIGNED ]; then
        fail "$closeout is READY with an unassigned reviewer"
    elif [ "$reviewer" != UNASSIGNED ] &&
            reviewer_fixed_front "$front" "$reviewer"; then
        fail "$closeout reviewer $reviewer authored a $front remediation commit"
    fi
done <"$map"

if [ "$front_count" -eq 0 ]; then
    fail "front map is empty"
fi
if [ "$failures" -ne 0 ]; then
    echo "check-closeouts: $failures failure(s)" >&2
    exit 1
fi
echo "check-closeouts: $front_count fronts structurally complete"
