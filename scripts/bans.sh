#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-bans.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

all_files=$tmp/all-files
c_files=$tmp/c-files
source_files=$tmp/source-files
allocator_files=$tmp/allocator-files
non_unicode_files=$tmp/non-unicode-files
ci_files=$tmp/ci-files
pty_files=$tmp/pty-files
piece_files=$tmp/piece-files
deterministic_fuzz_files=$tmp/deterministic-fuzz-files
syn_files=$tmp/syn-files
hits=$tmp/hits
: >"$hits"

# Sprint 56 unified the performance ledger under the plural directory.
# A revived singular copy would accept updates that no runner reads.
if [ -d "$repo_dir/tests/perf/baseline" ]; then
    echo "ban: singular tests/perf/baseline directory" >>"$hits"
    echo "tests/perf/baseline" >>"$hits"
fi

find "$repo_dir/src" "$repo_dir/tests" -type f -print |
    LC_ALL=C sort >"$all_files"
while IFS= read -r file; do
    case $file in
        *.c|*.h) printf '%s\n' "$file" ;;
    esac
done <"$all_files" >"$c_files"
find "$repo_dir/src" -type f -print | LC_ALL=C sort >"$source_files"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/util/base.c) ;;
        src/*.c|src/*.h|src/*/*.c|src/*/*.h|src/*/*/*.c|src/*/*/*.h)
            printf '%s\n' "$file" ;;
    esac
done <"$source_files" >"$allocator_files"
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

# Every portability pattern below carries a tiny positive control.  A grep
# gate that has silently stopped matching is worse than no gate: it leaves a
# green lane claiming a portability property it no longer checks.
scan_seed()
{
    label=$1
    pattern=$2
    source=$3
    if ! printf '%s\n' "$source" | grep -nE -e "$pattern" >/dev/null 2>&1; then
        echo "ban: the $label rule no longer fires on its own seed" >>"$hits"
    fi
}

spliced_hits()
{
    pattern=$1
    file_list=$2
    spliced_out=$3
    : >"$spliced_out"
    set --
    while IFS= read -r file; do
        set -- "$@" "$file"
    done <"$file_list"
    if [ "$#" -eq 0 ]; then
        return 0
    fi
    awk -v pattern="$pattern" -v repo="$repo_dir/" '
    function inspect() {
        if (logical ~ pattern)
            printf "%s:%d:%s\n", name, start, logical
        logical = ""
        start = 0
    }
    FNR == 1 {
        if (start != 0)
            inspect()
        name = FILENAME
        if (index(name, repo) == 1)
            name = substr(name, length(repo) + 1)
    }
    {
        physical = $0
        if (start == 0)
            start = FNR
        if (physical ~ /\\$/) {
            sub(/\\$/, "", physical)
            logical = logical physical
            next
        }
        logical = logical physical
        inspect()
    }
    END {
        if (start != 0)
            inspect()
    }' "$@" >>"$spliced_out" || :
}

scan_spliced()
{
    label=$1
    pattern=$2
    file_list=$3
    scan_hits=$tmp/scan-spliced
    spliced_hits "$pattern" "$file_list" "$scan_hits"
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
        function fmt_sink(id, depth) {
            depth = 0
            while ((id in format_alias) && depth < 64) {
                id = format_alias[id]
                depth++
            }
            return id
        }
        END {
            # YEW-F-027: an object-like macro alias does not make a
            # nonliteral printf-family format safe.  Resolve alias chains
            # before classifying call tokens so ordinary macro forwarding
            # cannot evade the literal-format boundary.
            for (i = 1; i <= NR; i++) {
                define = line[i]
                if (define !~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+[A-Za-z_][A-Za-z0-9_]*([ \t]|$)/)
                    continue
                sub(/^[ \t]*#[ \t]*define[ \t]+/, "", define)
                split(define, part, /[ \t]+/)
                format_alias[part[1]] = part[2]
            }
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
                    sink = fmt_sink(tok)
                    if (sink !~ /printf$/ && sink != "fl_raise")
                        continue
                    want = fmt_index(sink)
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
                    if (sink ~ /^v/ && call ~ /,[ \t]*ap[ \t]*\)$/)
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
    echo '#define FMT_ALIAS_1 bytebuf_printf'
    echo '#define FMT_ALIAS_2 FMT_ALIAS_1'
    echo 'void g(const char *t) { FMT_ALIAS_2(out, t); }'
    echo 'void h(void) { FMT_ALIAS_2(out, "%d", 1); }'
} >"$seed_file"
printf '%s\n' "$seed_file" >"$tmp/seed-list"
format_literal_hits "$tmp/seed-list" "$tmp/seed-hits"
seed_found=$(wc -l <"$tmp/seed-hits" | tr -d ' ')
if [ "$seed_found" != "4" ]; then
    echo "ban: the format-literal rule no longer fires on its own seed" \
        >>"$hits"
    echo "expected 4 violations in the seed, found $seed_found" >>"$hits"
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
fl_abort_pattern='(^|[^[:alnum:]_])(abort|assert)[[:space:]]*\(|^[[:space:]]*#[[:space:]]*define[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]+(abort|assert)([^[:alnum:]_]|$)'
: >"$fl_abort_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/fl/*) ;;
        *) continue ;;
    esac
    # YEW-F-028: naming abort/assert through an object-like macro still
    # creates the forbidden VM termination path; catch the forwarding
    # definition as well as an ordinary direct call.
    grep -nE -e "$fl_abort_pattern" "$file" \
        2>/dev/null | sed "s|^|${file#"$repo_dir"/}:|" >>"$fl_abort_hits" || :
done <"$source_files"
if [ -s "$fl_abort_hits" ]; then
    echo "ban: src/fl/ reports internal errors through yew_bug, not abort()" \
        >>"$hits"
    cat "$fl_abort_hits" >>"$hits"
fi
scan_seed "Fletch abort/assert macro forwarding" "$fl_abort_pattern" \
    '#define FL_DIE abort'

# YEW-F-029: aliasing qsort does not acquire stable ordering; reject the
# forwarding definition as well as direct qsort/qsort_r calls.
qsort_pattern='(^|[^[:alnum:]_])qsort(_r)?[[:space:]]*\(|^[[:space:]]*#[[:space:]]*define[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]+qsort(_r)?([^[:alnum:]_]|$)'
scan "qsort is unstable and qsort_r is ABI-divergent; use yew_sort_stable" \
    "$qsort_pattern" "$all_files"
scan_seed "qsort/qsort_r" "$qsort_pattern" \
    'void seeded(void) { qsort_r(rows, count, width, compare, ctx); }'
scan_seed "qsort/qsort_r macro forwarding" "$qsort_pattern" \
    '#define SORT_ROWS qsort'
# YEW-F-030 / YEW-F-031: token pasting `__attribute` with a trailing `__`
# must not hide GNU attribute or constructor syntax from the locked C11 gate.
# The incomplete stem has no valid place in this codebase, while matching it
# also retains the direct `__attribute__` check.
attribute_pattern='__attribute'
scan "__attribute__ is outside the locked C11 subset" \
    "$attribute_pattern" "$all_files"
scan_seed "token-pasted __attribute__" "$attribute_pattern" \
    'JOIN(__attribute, __)((unused)) static int seeded;'
scan "constructor registration is forbidden; use the explicit registry" \
    '(constructor|\.init_array)' "$c_files"
# YEW-F-032: `p ## thread_create` must not hide a pthread entry point from
# the single-threaded-core gate.  No thread_* stem is valid in yew source.
thread_pattern='(threads\.h|pthread|thread_[[:alnum:]_]+)'
scan "threads are forbidden in the single-threaded core" \
    "$thread_pattern" "$source_files"
scan_seed "token-pasted pthread API" "$thread_pattern" \
    'void seeded(void) { JOIN(p, thread_create)(t, 0, f, 0); }'
# YEW-F-033: __TIMESTAMP__ embeds filesystem modification time and is just
# as unreproducible as the compilation date/time macros.
repro_time_pattern='(__DATE__|__TIME__|__TIMESTAMP__)'
scan "compiler time macros break reproducible builds" \
    "$repro_time_pattern" "$source_files"
scan_seed "compiler time macros" "$repro_time_pattern" \
    'const char *seeded = __TIMESTAMP__;'
# YEW-F-034: forwarding mmap through an object-like macro retains the same
# truncate/SIGBUS hazard, so reject the alias definition and direct calls.
mmap_pattern='(^|[^[:alnum:]_])mmap[[:space:]]*\(|^[[:space:]]*#[[:space:]]*define[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]+mmap([^[:alnum:]_]|$)'
scan "mmap risks SIGBUS after truncation" \
    "$mmap_pattern" "$source_files"
scan_seed "mmap macro forwarding" "$mmap_pattern" '#define MAP_FILE mmap'
# YEW-F-035: an object-like alias to a libc allocator still bypasses the
# audited yew allocation boundary; reject forwarding definitions too.
allocator_pattern='(^|[^[:alnum:]_])(malloc|calloc|realloc|free|strdup|getdelim|getline|asprintf|vasprintf)[[:space:]]*\(|^[[:space:]]*#[[:space:]]*define[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]+(malloc|calloc|realloc|free|strdup|getdelim|getline|asprintf|vasprintf)([^[:alnum:]_]|$)'
scan "source allocations must use the audited yew allocator" \
    "$allocator_pattern" "$allocator_files"
scan_seed "libc allocator macro forwarding" "$allocator_pattern" \
    '#define ALLOCATE malloc'
scan "libc-owned cwd allocations must use yew_xgetcwd" \
    'getcwd[[:space:]]*\([[:space:]]*NULL[[:space:]]*,' \
    "$allocator_files"
scan "libc-owned realpath allocations must use yew_xrealpath" \
    'realpath[[:space:]]*\([^,]+,[[:space:]]*NULL[[:space:]]*\)' \
    "$allocator_files"

# YEW-F-036 / YEW-F-037: spelling a nearby NULL value through a pointer
# variable does not change getcwd/realpath ownership.  Follow the initialized
# identifier across a short, ordinary call-site window without rejecting the
# fixed caller-owned buffers that these APIs may legitimately fill.
null_path_alloc_calls()
{
    null_path_list=$1
    null_path_out=$2
    : >"$null_path_out"
    while IFS= read -r file; do
        awk '
        { line[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                rest = line[i]
                buf = line[i] " " line[i+1] " " line[i+2] " " line[i+3]
                while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*NULL/)) {
                    assign = substr(rest, RSTART, RLENGTH)
                    sub(/[ \t]*=.*/, "", assign)
                    cwd = "getcwd[ \t]*\\([ \t]*" assign "[ \t]*,"
                    path = "realpath[ \t]*\\([^,]+,[ \t]*" assign \
                           "[ \t]*\\)"
                    if (buf ~ cwd || buf ~ path) {
                        printf "%d:%s\n", i, line[i]
                        break
                    }
                    rest = substr(rest, RSTART + RLENGTH)
                }
            }
        }' "$file" | sed "s|^|${file#"$repo_dir"/}:|" \
            >>"$null_path_out" || :
    done <"$null_path_list"
}

null_path_alloc_hits=$tmp/null-path-alloc-hits
null_path_alloc_calls "$allocator_files" "$null_path_alloc_hits"
if [ -s "$null_path_alloc_hits" ]; then
    echo "ban: NULL path allocations must use yew_xgetcwd/yew_xrealpath" \
        >>"$hits"
    cat "$null_path_alloc_hits" >>"$hits"
fi
null_path_seed=$tmp/null-path-seed.c
{
    echo 'char *a(void) { char *p = NULL; return getcwd(p, 0); }'
    echo 'char *b(const char *s) { char *p = NULL; return realpath(s, p); }'
} >"$null_path_seed"
printf '%s\n' "$null_path_seed" >"$tmp/null-path-seed-list"
null_path_alloc_calls "$tmp/null-path-seed-list" "$tmp/null-path-seed-hits"
if [ "$(wc -l <"$tmp/null-path-seed-hits" | tr -d ' ')" != "2" ]; then
    echo "ban: the NULL path-allocation rule no longer fires on its own seed" \
        >>"$hits"
fi
# YEW-F-038: mbtowc is the stateful locale-dependent predecessor of mbrtowc
# and belongs behind the same bespoke Unicode boundary.
locale_api_pattern='(wcwidth|wcswidth|mbrtowc|mbtowc|wchar\.h|langinfo\.h|setlocale|nl_langinfo|localeconv|iconv)'
scan "locale-dependent Unicode APIs are forbidden" \
    "$locale_api_pattern" "$source_files"
scan_seed "locale-dependent mbtowc" "$locale_api_pattern" \
    'int seeded(void) { return mbtowc(w, s, n); }'
# YEW-F-039: dlvsym is GNU's versioned native symbol lookup and violates the
# same Fletch-only plugin boundary as dlsym.
dynamic_loader_pattern='(^|[^[:alnum:]_])(dlopen|dlsym|dlvsym|dlclose|dlerror)[[:space:]]*\('
scan "native dynamic loading is forbidden; yew plugins are Fletch-only" \
    "$dynamic_loader_pattern" "$source_files"
scan_seed "native-dynamic-loading" "$dynamic_loader_pattern" \
    'void seeded(void) { (void)dlopen(path, flags); }'
scan_seed "versioned native-symbol loading" "$dynamic_loader_pattern" \
    'void seeded(void) { (void)dlvsym(handle, "name", "V1"); }'
# YEW-F-040: an object-like strerror_r alias preserves the incompatible GNU
# versus POSIX ABI and must not evade the portability boundary.
strerror_r_pattern='(^|[^[:alnum:]_])strerror_r[[:space:]]*\(|^[[:space:]]*#[[:space:]]*define[[:space:]]+[[:alpha:]_][[:alnum:]_]*[[:space:]]+strerror_r([^[:alnum:]_]|$)'
scan "strerror_r has incompatible GNU and POSIX ABIs; use strerror" \
    "$strerror_r_pattern" "$source_files"
scan_seed "strerror_r" "$strerror_r_pattern" \
    'void seeded(void) { (void)strerror_r(code, buf, sizeof(buf)); }'
scan_seed "strerror_r macro forwarding" "$strerror_r_pattern" \
    '#define ERROR_TEXT strerror_r'
# YEW-F-041: backtrace_symbols_fd is part of the same execinfo family and is
# likewise absent from the musl profile.
backtrace_pattern='(execinfo\.h|(^|[^[:alnum:]_])(backtrace|backtrace_symbols|backtrace_symbols_fd)[[:space:]]*\()'
scan "glibc backtrace APIs are unavailable in the musl profile" \
    "$backtrace_pattern" "$source_files"
scan_seed "glibc-backtrace" "$backtrace_pattern" \
    'void seeded(void) { (void)backtrace(frames, count); }'
scan_seed "glibc backtrace_symbols_fd" "$backtrace_pattern" \
    'void seeded(void) { backtrace_symbols_fd(frames, count, fd); }'
# YEW-F-042: getopt_long_only is a GNU extension alongside getopt_long and
# cannot enter the portable core merely by using the longer suffix.
gnu_api_pattern='(^|[^[:alnum:]_])(getline|getdelim|asprintf|vasprintf|getopt_long|getopt_long_only)[[:space:]]*\(|(^|[<"])err(or)?\.h[>"]|program_invocation_name'
scan "GNU-only libc APIs are forbidden in the portable core" \
    "$gnu_api_pattern" "$source_files"
scan_seed "GNU-libc-API" "$gnu_api_pattern" \
    'void seeded(void) { (void)getopt_long(argc, argv, opts, rows, idx); }'
scan_seed "GNU getopt_long_only" "$gnu_api_pattern" \
    'void seeded(void) { (void)getopt_long_only(argc, argv, opts, rows, idx); }'
scan_seed "program_invocation_name" "$gnu_api_pattern" \
    'const char *seeded = program_invocation_name;'
long_double_pattern='(^|[^[:alnum:]_])long[[:space:]]+double([^[:alnum:]_]|$)'
# YEW-F-043: translation phase 2 removes backslash-newline pairs before token
# recognition, so the ABI ban must inspect the same spliced logical lines.
scan_spliced "long double has different target ABIs; use the f64 model" \
    "$long_double_pattern" "$source_files"
scan_seed "long-double" "$long_double_pattern" \
    'long double seeded(long double value) { return value; }'
long_double_seed=$tmp/long-double-spliced.c
printf '%s\n' \
    'long \' \
    'double seeded(long \' \
    'double value) { return value; }' >"$long_double_seed"
printf '%s\n' "$long_double_seed" >"$tmp/long-double-spliced-list"
spliced_hits "$long_double_pattern" "$tmp/long-double-spliced-list" \
    "$tmp/long-double-spliced-hits"
if [ "$(wc -l <"$tmp/long-double-spliced-hits" | tr -d ' ')" != "1" ]; then
    echo "ban: the continued long-double rule no longer fires on its own seed" \
        >>"$hits"
fi
shim_check=$tmp/module-shims
if ! "$repo_dir/scripts/check-module-shims.sh" >"$shim_check" 2>&1; then
    echo "ban: disabled-module header/shim parity or honesty failed" >>"$hits"
    cat "$shim_check" >>"$hits"
fi
# YEW-F-045 and YEW-F-061: decimal constants name the same width-sensitive
# code points as their hexadecimal spellings. Catching them at the shared
# Unicode boundary also prevents register-local lookup tables.
unicode_width_pattern='(^|[^[:alnum:]_])(0[xX]1[fF]3[fF][bB]|0[xX][fF][eE]0[fF]|0[xX]200[dD]|127995|65039|8205)[uUlL]*([^[:alnum:]_]|$)|EastAsian'
scan "Unicode width math belongs only in src/unicode" \
    "$unicode_width_pattern" "$non_unicode_files"
scan_seed "decimal Unicode width constants" "$unicode_width_pattern" \
    'static const unsigned seeded[] = { 127995U, 65039U, 8205U };'
# YEW-F-046: a packed decimal value contains no hex or RGB spelling, but a
# syntax-side color role still violates the semantic-attribute boundary.
syntax_color_pattern='(#[0-9a-fA-F]{6}|[Rr][Gg][Bb]|38;2|48;5|(^|[^[:alnum:]_])([Ff][Gg]|[Bb][Gg]|[Ff]oreground|[Bb]ackground|[Cc]olor|[Cc]olour)([^[:alnum:]_]|$))'
scan "syntax definitions emit semantic attrs, never colors" \
    "$syntax_color_pattern" "$syn_files"
scan_seed "packed decimal syntax color" "$syntax_color_pattern" \
    'static const unsigned foreground = 16711680U;'
# YEW-F-047: local comparisons against the East Asian 0x1100 threshold
# reimplement cell width even when no width helper is named.
syntax_width_pattern='yew_(cp|str)_width|(^|[^[:alnum:]_])(0[xX]1100|4352)[uUlL]*([^[:alnum:]_]|$)'
scan "syntax owns byte spans; width math belongs in src/unicode" \
    "$syntax_width_pattern" "$syn_files"
scan_seed "syntax-local width threshold" "$syntax_width_pattern" \
    'unsigned seeded(unsigned cp) { return cp >= 0x1100U ? 2U : 1U; }'
# YEW-F-048: posix_openpt is the portable PTY-creation primitive, so omitting
# it left the ownership rule unable to reject a new ad hoc harness.  These
# four C files are the complete audited owner set: golden tests, shared live
# tests, terminal handover, and the raw-mode syscall fixture.  The shell audit
# fixture is data that emits the forbidden call into an isolated repository.
pty_creation_pattern='(^|[^[:alnum:]_])(forkpty|openpty|posix_openpt)[[:space:]]*\(|-lutil([^[:alnum:]_]|$)'
pty_creation_calls()
{
    pty_list=$1
    pty_out=$2
    : >"$pty_out"
    while IFS= read -r file; do
        case ${file#"$repo_dir"/} in
            tests/audit/f15_ban_miss.sh)
                continue
                ;;
            tests/pty/harness.c|tests/support/live_pty.c|\
            tests/unit/test_job_handover.c|tests/unit/test_tty.c)
                continue
                ;;
        esac
        grep -nE -e "$pty_creation_pattern" "$file" 2>/dev/null |
            sed "s|^|${file#"$repo_dir"/}:|" >>"$pty_out" || :
    done <"$pty_list"
}

pty_creation_calls "$pty_files" "$tmp/pty-creation-hits"
if [ -s "$tmp/pty-creation-hits" ]; then
    echo "ban: PTY creation must stay in the four audited test owners" \
        >>"$hits"
    cat "$tmp/pty-creation-hits" >>"$hits"
fi
pty_seed=$tmp/seeded-pty-call.c
echo 'int seeded(void) { return posix_openpt(0); }' >"$pty_seed"
printf '%s\n' "$pty_seed" >"$tmp/pty-seed-list"
pty_creation_calls "$tmp/pty-seed-list" "$tmp/pty-seed-hits"
if [ "$(wc -l <"$tmp/pty-seed-hits" | tr -d ' ')" != "1" ]; then
    echo "ban: the PTY-creation ownership rule no longer fires on its seed" \
        >>"$hits"
fi
# YEW-F-049: shell quote removal and line splicing can assemble the forbidden
# environment name without leaving its contiguous spelling in workflow text.
ci_golden_update_pattern='YEW_PTY_[^[:space:]]*UPDATE'
scan_spliced "golden updates are forbidden in CI" \
    "$ci_golden_update_pattern" "$ci_files"
scan_seed "split-name CI golden update" "$ci_golden_update_pattern" \
    'export YEW_PTY_"UPDATE"=1'
# YEW-F-050: positioned reads retain the same forbidden storage ownership as
# read(), even though their longer name evaded the original token boundary.
piece_io_pattern='(^|[^[:alnum:]_])(open|fopen|read|pread)[[:space:]]*\('
scan "piece tree file I/O belongs to Sprint 8" \
    "$piece_io_pattern" "$piece_files"
scan_seed "piece-tree pread" "$piece_io_pattern" \
    'long seeded(int fd, void *p, unsigned long n) { return pread(fd, p, n, 0); }'
shadow_draw_files=$tmp/shadow-draw-files
printf '%s\n' "$repo_dir/src/ui/shadowdraw.c" >"$shadow_draw_files"
scan "shadow insertion preview must compose without destructive row fill" \
    'yew_grid_fill[[:space:]]*\(' "$shadow_draw_files"
# YEW-F-051: a caller can spell a destructive full-row fill as an ordinary
# loop and never name yew_grid_fill.  Match the behavior without rejecting
# shadow_blank_cells, whose nonzero bounded ranges implement composition.
shadow_full_fill_calls()
{
    shadow_list=$1
    shadow_out=$2
    : >"$shadow_out"
    while IFS= read -r file; do
        [ -f "$file" ] || continue
        awk '
        { line[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) {
                if (line[i] !~ /for[ \t]*\(/)
                    continue
                body = line[i] " " line[i+1] " " line[i+2] " " \
                       line[i+3] " " line[i+4] " " line[i+5] " " \
                       line[i+6] " " line[i+7]
                if (body ~ /for[ \t]*\([^;]*=[ \t]*0[uU]*[ \t]*;[^;]*<[ \t]*[^;]*(->|\.)[ \t]*cols[ \t]*;/ &&
                    body ~ /((->|\.)[ \t]*(cells|back|front)|(^|[^[:alnum:]_])(cells|back|front))[ \t]*\[[^]]+\][ \t]*=/)
                    printf "%d:%s\n", i, line[i]
            }
        }' "$file" | sed "s|^|${file#"$repo_dir"/}:|" >>"$shadow_out" || :
    done <"$shadow_list"
}

shadow_full_fill_calls "$shadow_draw_files" "$tmp/shadow-full-fill-hits"
if [ -s "$tmp/shadow-full-fill-hits" ]; then
    echo "ban: shadow insertion preview must not replace a complete grid row" \
        >>"$hits"
    cat "$tmp/shadow-full-fill-hits" >>"$hits"
fi
shadow_fill_seed=$tmp/seeded-shadow-full-fill.c
printf '%s\n' \
    'void seeded(Grid *g, Cell blank)' \
    '{' \
    '    size_t x;' \
    '    for (x = 0; x < g->cols; x++) g->cells[x] = blank;' \
    '}' >"$shadow_fill_seed"
printf '%s\n' "$shadow_fill_seed" >"$tmp/shadow-fill-seed-list"
shadow_full_fill_calls "$tmp/shadow-fill-seed-list" \
    "$tmp/shadow-fill-seed-hits"
if [ "$(wc -l <"$tmp/shadow-fill-seed-hits" | tr -d ' ')" != "1" ]; then
    echo "ban: the shadow full-row rule no longer fires on its own seed" \
        >>"$hits"
fi
fuss_mode_files=$tmp/fuss-mode-files
printf '%s\n' "$repo_dir/src/mod/git/fussmode.c" >"$fuss_mode_files"
# YEW-F-052: taking the live pane-root address permits a later indirect
# replacement, so the drawer boundary forbids both that escape and assignment.
fuss_pane_root_write_pattern='pane_root[[:space:]]*=[[:space:]]*($|[^=])|&[[:space:]]*([[:alnum:]_]+[[:space:]]*(->|\.)[[:space:]]*)*pane_root([^[:alnum:]_]|$)'
scan "F mode is a drawer and must not replace the live pane root" \
    "$fuss_pane_root_write_pattern" "$fuss_mode_files"
scan_seed "FUSS pane-root ownership" "$fuss_pane_root_write_pattern" \
    'PaneNode **slot = &ed->panes.pane_root;'
scan_seed "FUSS direct pane-root replacement" \
    "$fuss_pane_root_write_pattern" 'ed->pane_root ='
# YEW-F-053: the libc random family shares global state and is no more
# replayable than rand; generated campaigns stay on the pinned xorshift PRNG.
deterministic_random_pattern='(^|[^[:alnum:]_])(rand|srand|random|srandom)[[:space:]]*\(|time[[:space:]]*\([[:space:]]*NULL[[:space:]]*\)'
scan "generated edit campaigns must use xorshift64*, not libc randomness" \
    "$deterministic_random_pattern" "$deterministic_fuzz_files"
scan_seed "deterministic random-call" "$deterministic_random_pattern" \
    'long seeded(void) { return ran''dom(); }'
scan_seed "deterministic random-seed" "$deterministic_random_pattern" \
    'void seeded(void) { sran''dom(1U); }'
# YEW-F-054: direct variadic exec of a shell with -c is the same forbidden
# clipboard command-string path as popen or system; only argv tools are valid.
clipboard_shell_pattern='(^|[^[:alnum:]_])(popen|system)[[:space:]]*\(|(^|[^[:alnum:]_])(execl|execle|execlp)[[:space:]]*\([[:space:]]*"/(usr/)?bin/(ba|da|k|z)?sh"[^;]*"-c"'
scan "clipboard subprocesses must never invoke a shell" \
    "$clipboard_shell_pattern" "$source_files"
scan_seed "clipboard direct-shell exec" "$clipboard_shell_pattern" \
    'execl("/bin/sh", "sh", "-c", cmd, NULL);'
# YEW-F-055: raw append into a command-string buffer is interpolation too;
# program-derived arguments belong in YewJobSpec.argv regardless of helper.
job_interpolation_pattern='bytebuf_printf.*cmdline|sprintf.*shell|bytebuf_append[[:space:]]*\([[:space:]]*&?[[:space:]]*(cmdline|shell)[[:space:]]*,'
scan "programmatic job data must not be interpolated into shell text" \
    "$job_interpolation_pattern" "$source_files"
scan_seed "job-command-interpolation" "$job_interpolation_pattern" \
    'bytebuf_printf(&cmdline, "%s", path);'
scan_seed "job-command-append" "$job_interpolation_pattern" \
    'bytebuf_append(shell, path, strlen(path));'
# YEW-F-056: adjacent C string fragments concatenate at compile time, so
# quotes and source whitespace cannot hide the OSC 52 query payload marker.
osc52_query_pattern='52;([^[:space:]]*|[[:space:]"]*)\?'
scan "OSC 52 clipboard queries are forbidden" \
    "$osc52_query_pattern" "$source_files"
scan_seed "OSC 52 contiguous query" "$osc52_query_pattern" \
    'static const char query[] = "\033]52;c;?\a";'
scan_seed "OSC 52 split-literal query" "$osc52_query_pattern" \
    'static const char query[] = "\033]52;" "?\a";'

# Sprint 37 DoD 2: all direct terminal-status and terminal-control syscalls
# stay behind the one poisoned boundary. The product-level smoke drill calls
# a guarded entry point under --batch; this static half prevents a new direct
# caller elsewhere in src/ from bypassing that guard entirely.
tty_syscall_hits=$tmp/tty-syscall-hits
: >"$tty_syscall_hits"
# YEW-F-057: flushing terminal queues mutates tty state just like the existing
# termios controls and therefore belongs behind the same guarded owner.
tty_syscall_pattern='(^|[^[:alnum:]_])(tcsetattr|tcgetattr|tcflush|ioctl|isatty)[[:space:]]*\('
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/term/tty.c) continue ;;
    esac
    grep -nE -e "$tty_syscall_pattern" \
        "$file" 2>/dev/null |
        sed "s|^|${file#"$repo_dir"/}:|" >>"$tty_syscall_hits" || :
done <"$source_files"
if [ -s "$tty_syscall_hits" ]; then
    echo "ban: terminal syscalls belong only in src/term/tty.c" >>"$hits"
    cat "$tty_syscall_hits" >>"$hits"
fi
scan_seed "terminal syscall ownership" "$tty_syscall_pattern" \
    'int seeded(int fd) { return tcflush(fd, TCIFLUSH); }'

# YEW-F-058: the raw named-register setter is deliberately private.  An
# allow-listed implementation file could otherwise publish a wrapper and let
# arbitrary callers bypass the yank/delete/macro routing policy.
register_set_pattern='(^|[^[:alnum:]_])yew_reg_set[[:space:]]*\('
scan "register writes must use the routed front doors" \
    "$register_set_pattern" "$source_files"
scan_seed "register-routing" "$register_set_pattern" \
    'void seeded(void) { yew_reg_set(regs, name, value); }'

#
# Report calls made outside an exact file:function owner set.  This is a
# deliberately small C lexer, not a parser: it removes comments and literals,
# tracks top-level function bodies, and leaves the compiler to parse C.  The
# source subset bans attributes and statement expressions, which keeps the
# ownership boundary unambiguous.
c_call_owners()
{
    call_list=$1
    call_pattern=$2
    call_allowed=$3
    call_out=$4
    : >"$call_out"
    while IFS= read -r file; do
        case $file in
            *.c) ;;
            *) continue ;;
        esac
        call_path=${file#"$repo_dir"/}
        awk -v path="$call_path" -v calls="$call_pattern" \
            -v allowed=",$call_allowed," '
            function scrub(s,    out, i, c, nextc) {
                out = ""
                for (i = 1; i <= length(s); i++) {
                    c = substr(s, i, 1)
                    nextc = substr(s, i + 1, 1)
                    if (in_comment) {
                        if (c == "*" && nextc == "/") {
                            in_comment = 0
                            out = out "  "
                            i++
                        } else {
                            out = out " "
                        }
                    } else if (quote != "") {
                        if (c == "\\") {
                            out = out "  "
                            i++
                        } else {
                            if (c == quote)
                                quote = ""
                            out = out " "
                        }
                    } else if (c == "/" && nextc == "*") {
                        in_comment = 1
                        out = out "  "
                        i++
                    } else if (c == "/" && nextc == "/") {
                        while (length(out) < length(s))
                            out = out " "
                        break
                    } else if (c == "\"" || c == single_quote) {
                        quote = c
                        out = out " "
                    } else {
                        out = out c
                    }
                }
                return out
            }
            function function_name(header,    p, before, name) {
                p = index(header, "(")
                if (p == 0 || index(substr(header, 1, p), "=") != 0)
                    return ""
                before = substr(header, 1, p - 1)
                sub(/[[:space:]]*$/, "", before)
                name = before
                sub(/^.*[^[:alnum:]_]/, "", name)
                if (name !~ /^[[:alpha:]_][[:alnum:]_]*$/)
                    return ""
                return name
            }
            BEGIN {
                single_quote = sprintf("%c", 39)
                depth = 0
                owner = ""
                pending = ""
            }
            {
                raw = $0
                code = scrub(raw)
                body = ""
                if (depth == 0 && code ~ /^[[:space:]]*#/) {
                    pending = ""
                    code = ""
                }
                if (depth == 0) {
                    open_at = index(code, "{")
                    if (open_at != 0) {
                        owner = function_name(pending " " \
                                              substr(code, 1, open_at - 1))
                        pending = ""
                        if (owner != "")
                            body = substr(code, open_at + 1)
                    } else {
                        pending = pending " " code
                        if (index(code, ";") != 0)
                            pending = ""
                    }
                } else if (owner != "") {
                    body = code
                }
                if (owner != "" && body ~ calls &&
                    index(allowed, "," path ":" owner ",") == 0)
                    print path ":" NR ":" raw
                opens = code
                closes = code
                gsub(/[^{]/, "", opens)
                gsub(/[^}]/, "", closes)
                depth += length(opens) - length(closes)
                if (depth == 0)
                    owner = ""
            }
        ' "$file" >>"$call_out"
    done <"$call_list"
}

# Sprint 36 DoD 5: every option write goes through the one typed registry
# choke point.  YEW-F-059 showed that exempting entire implementation files
# let a new wrapper launder writes, so this is an exact function-owner list.
option_set_calls()
{
    c_call_owners "$1" \
        '(^|[^[:alnum:]_])(yew_opt_set|yew_opt_set_for)[[:space:]]*[(]' \
        'src/edit/option.c:yew_opt_set,src/edit/option.c:builtin_set,src/fl/flapi.c:q_buf_opt_set,src/fl/flapi.c:fl_api_set_options,src/ui/cmdline.c:yew_opt_cmdline_set' \
        "$2"
}

option_set_calls "$source_files" "$tmp/option-set-hits"
if [ -s "$tmp/option-set-hits" ]; then
    echo "ban: option writes must use the typed option front doors" >>"$hits"
    cat "$tmp/option-set-hits" >>"$hits"
fi

#
# Sprint 55 DoD 7: git belongs exclusively to the explicit `yew pkg`
# subcommand.  Discovery may hash installed trees during editor startup,
# but it must never spawn git (a captive portal must not be able to hang
# opening an editor).  YEW-F-060 showed that exempting pkg.c let it export a
# laundering wrapper, so both the private spelling and its exact owners are
# pinned here.
#
pkg_git_calls()
{
    c_call_owners "$1" \
        '(^|[^[:alnum:]_])(yew_pkg_git|pkg_git)[[:space:]]*[(]' \
        'src/mod/plug/pkg.c:pkg_run_ok,src/mod/plug/pkg.c:pkg_doctor_paths,src/mod/plug/pkg.c:pkg_update' \
        "$2"
}

pkg_git_calls "$source_files" "$tmp/pkg-git-hits"
if [ -s "$tmp/pkg-git-hits" ]; then
    echo "ban: git package transport must not enter editor startup" >>"$hits"
    cat "$tmp/pkg-git-hits" >>"$hits"
fi

# Prove the boundary catches a newly seeded startup call.
pkg_git_seed=$tmp/seeded-pkg-git-call.c
echo 'void seeded(void) { (void)yew_pkg_git(argv, 1, 1, true, run); }' \
    >"$pkg_git_seed"
printf '%s\n' "$pkg_git_seed" >"$tmp/pkg-git-seed-list"
pkg_git_calls "$tmp/pkg-git-seed-list" "$tmp/pkg-git-seed-hits"
if [ "$(wc -l <"$tmp/pkg-git-seed-hits" | tr -d ' ')" != "1" ]; then
    echo "ban: the no-startup-git rule no longer fires on its own seed" \
        >>"$hits"
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

c_strip_comments_literals()
{
    awk '
        function scrub(s,    out, i, c, nextc) {
            out = ""
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                nextc = substr(s, i + 1, 1)
                if (in_comment) {
                    if (c == "*" && nextc == "/") {
                        in_comment = 0
                        out = out "  "
                        i++
                    } else {
                        out = out " "
                    }
                } else if (quote != "") {
                    if (c == "\\") {
                        out = out "  "
                        i++
                    } else {
                        if (c == quote)
                            quote = ""
                        out = out " "
                    }
                } else if (c == "/" && nextc == "*") {
                    in_comment = 1
                    out = out "  "
                    i++
                } else if (c == "/" && nextc == "/") {
                    break
                } else if (c == "\"" || c == single_quote) {
                    quote = c
                    out = out " "
                } else {
                    out = out c
                }
            }
            return out
        }
        BEGIN { single_quote = sprintf("%c", 39) }
        { print scrub($0) }
    ' "$1"
}

register_code=$tmp/register-code
c_strip_comments_literals "$register" >"$register_code"

# YEW-F-062: names do not establish types.  Strip comments and literals, find
# every cell-column declarator, then reject direct access to its representation
# regardless of the local name; register paste must use the coordinate API.
awk '
    function add_declarators(tail,    stop, n, parts, i, part, name) {
        stop = length(tail) + 1
        if (index(tail, ";") != 0 && index(tail, ";") < stop)
            stop = index(tail, ";")
        if (index(tail, ")") != 0 && index(tail, ")") < stop)
            stop = index(tail, ")")
        if (index(tail, "{") != 0 && index(tail, "{") < stop)
            stop = index(tail, "{")
        tail = substr(tail, 1, stop - 1)
        n = split(tail, parts, ",")
        for (i = 1; i <= n; i++) {
            part = parts[i]
            sub(/=.*/, "", part)
            sub(/^[[:space:]*]*/, "", part)
            name = part
            sub(/[^[:alnum:]_].*$/, "", name)
            if (name ~ /^[[:alpha:]_][[:alnum:]_]*$/ &&
                substr(part, length(name) + 1) !~ /^[[:space:]]*[(]/)
                names[name] = 1
        }
    }
    { source = source "\n" $0 }
    END {
        rest = source
        type_pattern = "(^|[^[:alnum:]_])(CCol|CellCol)[^[:alnum:]_]"
        while (match(rest, type_pattern)) {
            tail = substr(rest, RSTART + RLENGTH)
            add_declarators(tail)
            rest = tail
        }
        for (name in names) {
            member = "(^|[^[:alnum:]_])" name \
                     "[[:space:]]*(\\.|->[[:space:]]*)v([^[:alnum:]_]|$)"
            if (source ~ member) {
                print "direct cell-column representation access"
                exit
            }
        }
    }
' "$register_code" >"$tmp/register-column-math-hits"
if [ -s "$tmp/register-column-math-hits" ]; then
    echo "ban: register paste must not perform cell-column arithmetic" \
        >>"$hits"
    sed 's|^|src/text/register.c:|' \
        "$tmp/register-column-math-hits" >>"$hits"
fi
# YEW-F-063: helper spellings in comments and string literals are not routing
# evidence; require each call token in stripped C code.
for required in yew_off_to_ccol yew_ccol_to_off_padded \
                yew_ccol_shortfall yew_ccol_max; do
    if ! grep -E "(^|[^[:alnum:]_])${required}[[:space:]]*[(]" \
            "$register_code" >/dev/null 2>&1; then
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

# AI request and completion bytes have one audited sink.  That sink enforces
# the environment + typed-option dual gate; keeping its surface tiny makes a
# new unconditional body log a build failure rather than a privacy regression.
ai_body_hits=$tmp/ai-body-log
: >"$ai_body_hits"
grep -rnE 'yew_log[^;]*(ctx->prefix|ctx->suffix|->text\b|prompt|completion|body)' \
    "$repo_dir/src" --include='*.c' 2>/dev/null |
    grep -v 'yew_ai_debug_body' >"$ai_body_hits" || :
if [ -s "$ai_body_hits" ]; then
    echo "ban: AI prompt/completion bodies must use yew_ai_debug_body" >>"$hits"
    cat "$ai_body_hits" >>"$hits"
fi
ai_debug_body_refs=$(grep -rn 'yew_ai_debug_body' "$repo_dir/src" 2>/dev/null |
    wc -l | tr -d ' ')
if [ "$ai_debug_body_refs" -gt 4 ]; then
    echo "ban: yew_ai_debug_body exceeds four audited source references" >>"$hits"
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
