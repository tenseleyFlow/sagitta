#!/bin/sh
set -eu

root=${YEW_MANUAL_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
manual=${YEW_MANUAL_DIR:-$root/docs/manual}
book=${YEW_MANUAL_BOOK:-$root/docs/fletch-book/01-arrows-to-programs.md}
yew_bin=${YEW_MANUAL_BIN:-$root/build/yew}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-manual.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail()
{
    echo "check-manual: $*" >&2
    exit 1
}

chapters=
i=1
while [ "$i" -le 15 ]; do
    number=$(printf '%02d' "$i")
    chapter=$(find "$manual" -maxdepth 1 -type f -name "$number-*.md" -print)
    [ -n "$chapter" ] || fail "missing chapter $number"
    [ "$(printf '%s\n' "$chapter" | wc -l | tr -d ' ')" -eq 1 ] ||
        fail "chapter $number is ambiguous"
    chapters="$chapters $chapter"
    i=$((i + 1))
done

[ -x "$yew_bin" ] || fail "missing executable $yew_bin"
[ -f "$book" ] || fail "missing Fletch book chapter 1"
chapters="$chapters $book"

# Extract tagged Markdown fences without a Markdown dependency.  A runnable
# sh/batch block must be followed immediately (apart from blank lines) by its
# byte-exact output block.  Fletch blocks compile but have no output contract.
awk -v outdir="$tmp" '
BEGIN { manifest = outdir "/manifest" }
function die(message) {
    print FILENAME ":" FNR ": " message > "/dev/stderr"
    bad = 1
}
function known(t) {
    return t == "sh" || t == "batch" || t == "fletch" ||
           t == "keys" || t == "text" || t == "output"
}
FNR == 1 {
    if (inside) die("unterminated code fence")
    if (need_output) die("runnable block has no following output block")
    inside = 0
    need_output = 0
}
/^```/ {
    if (!inside) {
        tag = substr($0, 4)
        if (tag == "") {
            die("code fence has no language tag")
            next
        }
        if (!known(tag)) {
            die("unknown code-fence tag: " tag)
            next
        }
        if (need_output && tag != "output") {
            die("runnable block must be followed by output")
            need_output = 0
        }
        if (tag == "output") {
            if (!pending) {
                die("output block has no runnable input")
                next
            }
            path = outdir "/case-" pending ".expected"
            printf "%s", "" > path
            close(path)
            pending = 0
            need_output = 0
        } else if (tag == "sh" || tag == "batch" || tag == "fletch" ||
                   tag == "keys") {
            count++
            path = outdir "/case-" count ".input"
            print count "|" tag "|" FILENAME "|" (FNR + 1) >> manifest
        } else {
            path = "/dev/null"
        }
        inside = 1
        next
    }
    inside = 0
    close(path)
    if (tag == "sh" || tag == "batch") {
        pending = count
        need_output = 1
    }
    tag = ""
    path = ""
    next
}
inside { print $0 > path; next }
need_output && $0 !~ /^[[:space:]]*$/ {
    die("runnable block must be followed immediately by output")
    need_output = 0
    pending = 0
}
END {
    if (inside) die("unterminated code fence")
    if (need_output) die("runnable block has no following output block")
    if (count == 0) die("manual has no executable examples")
    if (bad) exit 1
}
' $chapters || fail "Markdown example extraction failed"

mkdir -p "$tmp/home" "$tmp/config" "$tmp/data" "$tmp/state" \
    "$tmp/cache" "$tmp/work"

run_sh()
{
    input=$1
    stdout=$2
    stderr=$3
    (cd "$root" && env -i \
        HOME="$tmp/home" \
        PATH="$root/build:/usr/bin:/bin:/usr/sbin:/sbin" \
        XDG_CONFIG_HOME="$tmp/config" \
        XDG_DATA_HOME="$tmp/data" \
        XDG_STATE_HOME="$tmp/state" \
        XDG_CACHE_HOME="$tmp/cache" \
        YEW_RUNTIME_DIR="$root/runtime" \
        sh -eu "$input") >"$stdout" 2>"$stderr"
}

run_batch()
{
    input=$1
    stdout=$2
    stderr=$3
    (cd "$tmp/work" && env -i \
        HOME="$tmp/home" \
        PATH="$root/build:/usr/bin:/bin:/usr/sbin:/sbin" \
        XDG_CONFIG_HOME="$tmp/config" \
        XDG_DATA_HOME="$tmp/data" \
        XDG_STATE_HOME="$tmp/state" \
        XDG_CACHE_HOME="$tmp/cache" \
        YEW_RUNTIME_DIR="$root/runtime" \
        "$yew_bin" --clean --batch "$input") >"$stdout" 2>"$stderr"
}

while IFS='|' read -r number tag source line; do
    input=$tmp/case-$number.input
    first=$tmp/case-$number.first
    second=$tmp/case-$number.second
    first_err=$tmp/case-$number.first.err
    second_err=$tmp/case-$number.second.err

    case $tag in
        sh)
            run_sh "$input" "$first" "$first_err" || {
                cat "$first_err" >&2
                fail "$source:$line: shell example failed"
            }
            run_sh "$input" "$second" "$second_err" || {
                cat "$second_err" >&2
                fail "$source:$line: shell example failed on repeat"
            }
            ;;
        batch)
            run_batch "$input" "$first" "$first_err" || {
                cat "$first_err" >&2
                fail "$source:$line: batch example failed"
            }
            run_batch "$input" "$second" "$second_err" || {
                cat "$second_err" >&2
                fail "$source:$line: batch example failed on repeat"
            }
            ;;
        fletch)
            env YEW_RUNTIME_DIR="$root/runtime" \
                "$yew_bin" fl --dump-ast "$input" \
                >"$first" 2>"$first_err" || {
                cat "$first_err" >&2
                fail "$source:$line: Fletch example does not parse"
            }
            continue
            ;;
        keys)
            fail "$source:$line: keys example requires a registered PTY case"
            ;;
        *) fail "$source:$line: internal unknown example tag $tag" ;;
    esac

    [ ! -s "$first_err" ] || {
        cat "$first_err" >&2
        fail "$source:$line: example wrote unexpected stderr"
    }
    [ ! -s "$second_err" ] || {
        cat "$second_err" >&2
        fail "$source:$line: repeated example wrote unexpected stderr"
    }
    cmp -s "$first" "$second" ||
        fail "$source:$line: example output is nondeterministic"
    expected=$tmp/case-$number.expected
    [ -f "$expected" ] || fail "$source:$line: missing expected output"
    cmp -s "$expected" "$first" || {
        diff -u "$expected" "$first" >&2 || true
        fail "$source:$line: output differs"
    }
done <"$tmp/manifest"

echo "check-manual: ok"
