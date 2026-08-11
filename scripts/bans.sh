#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-bans.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

all_files=$tmp/all-files
c_files=$tmp/c-files
source_files=$tmp/source-files
non_unicode_files=$tmp/non-unicode-files
ci_files=$tmp/ci-files
pty_files=$tmp/pty-files
piece_files=$tmp/piece-files
deterministic_fuzz_files=$tmp/deterministic-fuzz-files
syn_files=$tmp/syn-files
hits=$tmp/hits
: >"$hits"

find "$repo_dir/src" "$repo_dir/tests" -type f -print |
    LC_ALL=C sort >"$all_files"
while IFS= read -r file; do
    case $file in
        *.c|*.h) printf '%s\n' "$file" ;;
    esac
done <"$all_files" >"$c_files"
find "$repo_dir/src" -type f -print | LC_ALL=C sort >"$source_files"
find "$repo_dir/.github" -type f -print | LC_ALL=C sort >"$ci_files"
{
    find "$repo_dir/tests" -type f -print
    printf '%s\n' "$repo_dir/Makefile"
} | LC_ALL=C sort >"$pty_files"
printf '%s\n' "$repo_dir/src/text/piece.c" >"$piece_files"
find "$repo_dir/tests/fuzz" "$repo_dir/scripts" -type f -print |
    LC_ALL=C sort >"$deterministic_fuzz_files"
: >"$syn_files"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        # Theme loading is the one syntax subsystem that owns colors;
        # definitions and matching continue to traffic only in attrs.
        src/syn/theme.c|src/syn/theme.h) ;;
        src/syn/*) printf '%s\n' "$file" >>"$syn_files" ;;
    esac
    case ${file#"$repo_dir"/} in
        src/unicode/*) ;;
        *) printf '%s\n' "$file" ;;
    esac
done <"$source_files" >"$non_unicode_files"

scan()
{
    label=$1
    pattern=$2
    file_list=$3
    scan_hits=$tmp/scan
    : >"$scan_hits"
    while IFS= read -r file; do
        grep -nE -e "$pattern" "$file" 2>/dev/null |
            sed "s|^|${file#"$repo_dir"/}:|" >>"$scan_hits" || :
    done <"$file_list"
    if [ -s "$scan_hits" ]; then
        echo "ban: $label" >>"$hits"
        cat "$scan_hits" >>"$hits"
    fi
}

#
# Sprint 31 DoD 5: no conversion in src/fl/ may take a format that is not
# a literal in our own source.
#
# fmt.f interprets the §6 directive grammar itself precisely so a user
# template never reaches a C conversion -- a `%n` in a Fletch template is
# a percent sign.  That property is only as good as the call sites, so
# every printf-family call and every fl_raise in src/fl/ has to show a
# `"` where its format argument belongs.
#
# The one shape allowed through is a VARARG FORWARDER: a bounded v-
# variant whose last argument is the `ap` it was handed.  fl_raise and
# the diagnostics are exactly that, and they are what make the literal
# rule enforceable everywhere else.
#
# Argument positions differ per function, so the scanner counts
# top-level commas rather than looking for a quote anywhere on the line
# -- `snprintf(buf, sizeof(buf), fmt, ap)` has a paren and a comma
# inside its second argument, and a quote in its fourth would otherwise
# read as compliance.
#
format_literal_hits()
{
    fl_list=$1
    fl_out=$2
    : >"$fl_out"
    while IFS= read -r file; do
        awk '
        { line[NR] = $0 }
        function fmt_index(id) {
            if (id ~ /snprintf$/)  return 2
            if (id == "fl_raise")  return 2
            if (id ~ /vprintf$/)   return 0
            if (id == "printf")    return 0
            return 1
        }
        END {
            for (i = 1; i <= NR; i++) {
                # Comment bodies are prose about the rule, not calls --
                # this file has tripped five grep gates on its own
                # explanations already.
                if (line[i] ~ /^[ \t]*(\*|\/\*|\/\/)/)
                    continue
                own = length(line[i])
                buf = line[i] " " line[i+1] " " line[i+2] " " line[i+3]
                pos = 1
                while (1) {
                    rest = substr(buf, pos)
                    if (!match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/))
                        break
                    at = pos + RSTART - 1
                    tok = substr(buf, at, RLENGTH)
                    pos = at + RLENGTH
                    if (at > own)
                        break
                    sub(/[ \t]*\($/, "", tok)
                    if (tok !~ /printf$/ && tok != "fl_raise")
                        continue
                    want = fmt_index(tok)
                    depth = 1
                    args = 0
                    k = pos
                    fmt_at = pos
                    while (k <= length(buf) && depth > 0) {
                        ch = substr(buf, k, 1)
                        if (ch == "(") depth++
                        else if (ch == ")") depth--
                        else if (ch == "," && depth == 1) {
                            args++
                            if (args == want) fmt_at = k + 1
                        }
                        if (depth == 0) break
                        k++
                    }
                    if (args < want)
                        continue
                    call = substr(buf, at, k - at + 1)
                    # A declaration, not a call: every function in this
                    # family is variadic, so `...` in the argument list
                    # is the prototype and nothing else.
                    if (call ~ /\.\.\.[ \t]*\)$/)
                        continue
                    if (tok ~ /^v/ && call ~ /,[ \t]*ap[ \t]*\)$/)
                        continue
                    tail = substr(buf, fmt_at)
                    sub(/^[ \t]*/, "", tail)
                    if (substr(tail, 1, 1) != "\"")
                        printf "%d:%s\n", i, line[i]
                }
            }
        }' "$file" | sed "s|^|${file#"$repo_dir"/}:|" >>"$fl_out" || :
    done <"$fl_list"
}

fl_files=$tmp/fl-files
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/fl/*) printf '%s\n' "$file" ;;
    esac
done <"$source_files" >"$fl_files"
format_literal_hits "$fl_files" "$tmp/format-literal-hits"
if [ -s "$tmp/format-literal-hits" ]; then
    echo "ban: a format in src/fl/ must be a literal in our own source" \
        >>"$hits"
    cat "$tmp/format-literal-hits" >>"$hits"
fi

#
# The seeded violation.  A rule nobody has watched fire is a rule that
# may have stopped working; this proves the scanner still catches the
# thing it exists to catch, and still lets the two compliant shapes
# through.
#
seed_dir=$tmp/seed
mkdir -p "$seed_dir"
seed_file=$seed_dir/seeded.c
{
    echo 'void a(const char *t) { bytebuf_printf(out, t); }'
    echo 'void b(const char *t) { (void)snprintf(q, sizeof(q), t, 1); }'
    echo 'void c(FlVm *vm, const char *t) { (void)fl_raise(vm, "type", t); }'
    echo 'void d(void) { bytebuf_printf(out, "%d", 1); }'
    echo 'void e(void) { (void)snprintf(q, sizeof(q), "%s.%s", a, b); }'
    echo 'void f(va_list ap) { (void)vsnprintf(m, sizeof(m), fmt, ap); }'
} >"$seed_file"
printf '%s\n' "$seed_file" >"$tmp/seed-list"
format_literal_hits "$tmp/seed-list" "$tmp/seed-hits"
seed_found=$(wc -l <"$tmp/seed-hits" | tr -d ' ')
if [ "$seed_found" != "3" ]; then
    echo "ban: the format-literal rule no longer fires on its own seed" \
        >>"$hits"
    echo "expected 3 violations in the seed, found $seed_found" >>"$hits"
    cat "$tmp/seed-hits" >>"$hits"
fi

#
# Sprint 32 DoD 10: the VM never aborts and never asserts.
#
# An internal invariant break goes through yew_bug -- a structured
# report and exit 4 -- because a bare abort() gives a reporter a signal
# and nothing else, and assert() is compiled out under NDEBUG, which
# turns the one check that mattered into no check at all.
#
fl_abort_hits=$tmp/fl-abort-hits
: >"$fl_abort_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/fl/*) ;;
        *) continue ;;
    esac
    grep -nE -e '(^|[^[:alnum:]_])(abort|assert)[[:space:]]*\(' "$file" \
        2>/dev/null | sed "s|^|${file#"$repo_dir"/}:|" >>"$fl_abort_hits" || :
done <"$source_files"
if [ -s "$fl_abort_hits" ]; then
    echo "ban: src/fl/ reports internal errors through yew_bug, not abort()" \
        >>"$hits"
    cat "$fl_abort_hits" >>"$hits"
fi

scan "qsort is unstable; use yew_sort_stable" \
    '(^|[^[:alnum:]_])qsort[[:space:]]*\(' "$all_files"
scan "__attribute__ is outside the locked C11 subset" \
    '__attribute__' "$all_files"
scan "constructor registration is forbidden; use the explicit registry" \
    'constructor' "$c_files"
scan "threads are forbidden in the single-threaded core" \
    '(threads\.h|pthread)' "$source_files"
scan "__DATE__ and __TIME__ break reproducible builds" \
    '(__DATE__|__TIME__)' "$source_files"
scan "mmap risks SIGBUS after truncation" \
    '(^|[^[:alnum:]_])mmap[[:space:]]*\(' "$source_files"
scan "locale-dependent Unicode APIs are forbidden" \
    '(wcwidth|wcswidth|mbrtowc|wchar\.h|setlocale|iconv)' "$source_files"
scan "Unicode width math belongs only in src/unicode" \
    '(0x1F3FB|0xFE0F|0x200D|EastAsian)' "$non_unicode_files"
scan "syntax definitions emit semantic attrs, never colors" \
    '(#[0-9a-fA-F]{6}|[Rr][Gg][Bb]|38;2|48;5)' "$syn_files"
scan "syntax owns byte spans; width math belongs in src/unicode" \
    'yew_(cp|str)_width' "$syn_files"
scan "pty creation must use the audited posix_openpt harness" \
    '(forkpty|openpty|-lutil)' "$pty_files"
scan "golden updates are forbidden in CI" \
    'YEW_PTY_UPDATE' "$ci_files"
scan "piece tree file I/O belongs to Sprint 8" \
    '(^|[^[:alnum:]_])(open|fopen|read)[[:space:]]*\(' "$piece_files"
scan "generated edit campaigns must use xorshift64*, not libc randomness" \
    '(^|[^[:alnum:]_])rand[[:space:]]*\(|(^|[^[:alnum:]_])srand[[:space:]]*\(|time[[:space:]]*\([[:space:]]*NULL[[:space:]]*\)' \
    "$deterministic_fuzz_files"
scan "clipboard subprocesses must never invoke a shell" \
    '(^|[^[:alnum:]_])(popen|system)[[:space:]]*\(' "$source_files"
scan "OSC 52 clipboard queries are forbidden" \
    '52;[^[:space:]]*\?' "$source_files"

# Sprint 37 DoD 2: all direct terminal-status and terminal-control syscalls
# stay behind the one poisoned boundary. The product-level smoke drill calls
# a guarded entry point under --batch; this static half prevents a new direct
# caller elsewhere in src/ from bypassing that guard entirely.
tty_syscall_hits=$tmp/tty-syscall-hits
: >"$tty_syscall_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/term/tty.c) continue ;;
    esac
    grep -nE -e '(^|[^[:alnum:]_])(tcsetattr|tcgetattr|ioctl|isatty)[[:space:]]*\(' \
        "$file" 2>/dev/null |
        sed "s|^|${file#"$repo_dir"/}:|" >>"$tty_syscall_hits" || :
done <"$source_files"
if [ -s "$tty_syscall_hits" ]; then
    echo "ban: terminal syscalls belong only in src/term/tty.c" >>"$hits"
    cat "$tty_syscall_hits" >>"$hits"
fi

register_set_hits=$tmp/register-set-hits
: >"$register_set_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/text/register.c|src/text/register.h) continue ;;
    esac
    grep -nE -e '(^|[^[:alnum:]_])yew_reg_set[[:space:]]*\(' \
        "$file" 2>/dev/null |
        sed "s|^|${file#"$repo_dir"/}:|" >>"$register_set_hits" || :
done <"$source_files"
if [ -s "$register_set_hits" ]; then
    echo "ban: register writes must use the yank/delete routing choke point" \
        >>"$hits"
    cat "$register_set_hits" >>"$hits"
fi

#
# Sprint 36 DoD 5: every option write goes through the one typed registry
# choke point.  The registry implementation, its public declaration, and
# the two specified front doors are the complete allow-list; a new caller
# anywhere else would create a second origin/on-change policy surface.
#
option_set_calls()
{
    option_list=$1
    option_out=$2
    : >"$option_out"
    while IFS= read -r file; do
        case ${file#"$repo_dir"/} in
            src/edit/option.c|src/edit/option.h|src/fl/flapi.c|src/ui/cmdline.c)
                continue
                ;;
        esac
        grep -nE -e '(^|[^[:alnum:]_])yew_opt_set[[:space:]]*\(' \
            "$file" 2>/dev/null |
            sed "s|^|${file#"$repo_dir"/}:|" >>"$option_out" || :
    done <"$option_list"
}

option_set_calls "$source_files" "$tmp/option-set-hits"
if [ -s "$tmp/option-set-hits" ]; then
    echo "ban: option writes must use the typed option front doors" >>"$hits"
    cat "$tmp/option-set-hits" >>"$hits"
fi

# Prove the allow-list catches the next call site instead of silently
# becoming decorative as source layout evolves.
option_seed=$tmp/seeded-option-call.c
echo 'void seeded(void) { (void)yew_opt_set(ed, 0, "x", 1, v, err); }' \
    >"$option_seed"
printf '%s\n' "$option_seed" >"$tmp/option-seed-list"
option_set_calls "$tmp/option-seed-list" "$tmp/option-seed-hits"
if [ "$(wc -l <"$tmp/option-seed-hits" | tr -d ' ')" != "1" ]; then
    echo "ban: the option-routing rule no longer fires on its own seed" \
        >>"$hits"
fi

register=$repo_dir/src/text/register.c
if grep -nE '(unicode/width\.h|yew_(cluster_)?width)' \
        "$register" >"$tmp/register-width-hits" 2>/dev/null; then
    echo "ban: register paste width calculations belong in src/unicode" \
        >>"$hits"
    sed 's|^|src/text/register.c:|' "$tmp/register-width-hits" >>"$hits"
fi
if grep -nE '(column|landed|content_column)\.v' \
        "$register" >"$tmp/register-column-math-hits" 2>/dev/null; then
    echo "ban: register paste must not perform cell-column arithmetic" \
        >>"$hits"
    sed 's|^|src/text/register.c:|' \
        "$tmp/register-column-math-hits" >>"$hits"
fi
for required in yew_off_to_ccol yew_ccol_to_off_padded \
                yew_ccol_shortfall yew_ccol_max; do
    if ! grep -F "$required" "$register" >/dev/null 2>&1; then
        echo "ban: register paste must route column math through $required" \
            >>"$hits"
    fi
done

if grep -nE 'yew_textbuf_|piece\.h' \
        "$repo_dir/tests/fuzz/oracle.c" >"$tmp/oracle-hits" 2>/dev/null; then
    echo "ban: the text-buffer oracle must remain implementation-independent" \
        >>"$hits"
    sed 's|^|tests/fuzz/oracle.c:|' "$tmp/oracle-hits" >>"$hits"
fi

tables=$repo_dir/src/unicode/tables.c
generated_marker="GENERATED by scripts/gen-unicode-tables from UCD 16.0.0"
if [ ! -f "$tables" ] ||
   ! grep -F "$generated_marker" "$tables" >/dev/null 2>&1; then
    echo "ban: src/unicode/tables.c lacks its generated-file marker" >>"$hits"
fi

# yew_bug is the single audited process-termination site required by the
# exit-code contract.  No other source file may call exit().
exit_hits=$tmp/exit
: >"$exit_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/util/log.c) continue ;;
    esac
    grep -nE -e '(^|[^[:alnum:]_])exit[[:space:]]*\(' "$file" 2>/dev/null |
        sed "s|^|${file#"$repo_dir"/}:|" >>"$exit_hits" || :
done <"$source_files"
if [ -s "$exit_hits" ]; then
    echo "ban: exit() is allowed only in src/util/log.c:yew_bug" >>"$hits"
    cat "$exit_hits" >>"$hits"
fi

registry=$repo_dir/tests/unit/registry.c
defs=$tmp/test-defs
: >"$defs"
for file in "$repo_dir"/tests/unit/test_*.c; do
    [ -f "$file" ] || continue
    sed -n 's/^void[[:space:]]\{1,\}test_\([[:alnum:]_]*\)[[:space:]]*(.*/\1/p' "$file" |
        while IFS= read -r name; do
            printf '%s\t%s\n' "${file#"$repo_dir"/}" "$name"
        done >>"$defs"
done
LC_ALL=C sort -o "$defs" "$defs"

while IFS="$(printf '\t')" read -r file name; do
    [ -n "$name" ] || continue
    if [ ! -f "$registry" ] ||
       ! grep -E "T[[:space:]]*\([[:space:]]*${name}[[:space:]]*\)" "$registry" >/dev/null 2>&1; then
        if ! grep -F "ban: unregistered tests" "$hits" >/dev/null 2>&1; then
            echo "ban: unregistered tests" >>"$hits"
        fi
        echo "$file: test_$name" >>"$hits"
    fi
done <"$defs"

pty_registry=$repo_dir/tests/pty/registry.c
golden_dir=$repo_dir/tests/pty/goldens
if [ -f "$pty_registry" ]; then
    pty_cases=$tmp/pty-cases
    golden_refs=$tmp/golden-refs
    sed -n 's/^[[:space:]]*C[[:space:]]*([[:space:]]*\([[:alnum:]_]*\).*/\1/p' \
        "$pty_registry" | LC_ALL=C sort -u >"$pty_cases"
    if [ "$(wc -l <"$pty_cases" | tr -d ' ')" -lt 12 ]; then
        echo "ban: fewer than 12 registered pty cases" >>"$hits"
    fi
    sed -n 's/.*ptc_snapshot[[:space:]]*([^,]*,[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$pty_registry" | LC_ALL=C sort -u >"$golden_refs"
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        if [ ! -f "$golden_dir/$name.golden" ]; then
            echo "ban: referenced pty golden is missing" >>"$hits"
            echo "tests/pty/goldens/$name.golden" >>"$hits"
        fi
    done <"$golden_refs"
    if [ -d "$golden_dir" ]; then
        for file in "$golden_dir"/*.golden; do
            [ -f "$file" ] || continue
            name=$(basename "$file" .golden)
            #
            # Not orphaned if a REGISTERED CASE bears the name, even
            # when no ptc_snapshot spells it as a literal.  A case may
            # snapshot under its own name — `ptc_snapshot(c, c->test->
            # name)` — which is how Sprint 27's chrome review gives one
            # scene function four degradation variants: the case name
            # picks the environment, the scene AND the golden, so the
            # three cannot drift apart.
            #
            # The ban's claim is unchanged.  A golden still has to be
            # produced by something registered, and deleting or renaming
            # the case still orphans the file — which is the staleness
            # this exists to catch.
            if ! grep -Fx "$name" "$golden_refs" >/dev/null 2>&1 &&
               ! grep -Fx "$name" "$pty_cases" >/dev/null 2>&1; then
                echo "ban: orphaned pty golden" >>"$hits"
                echo "tests/pty/goldens/$name.golden" >>"$hits"
            fi
        done
    fi
fi

if [ -s "$hits" ]; then
    cat "$hits" >&2
    exit 1
fi

echo "bans: ok"
