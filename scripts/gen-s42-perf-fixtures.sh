#!/bin/sh
# Generate the frozen Sprint 42 syntax-performance fixtures.  Each fixture is
# deliberately language-shaped, then padded with trailing spaces to make its
# byte size reproducible without changing its line-oriented syntax workload.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$repo/tests/perf/fixtures/syn
scratch=$(mktemp -d "${TMPDIR:-/tmp}/yew-s42-perf.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir -p "$out"

finish_fixture()
{
    source=$1
    target=$2
    lines=$3
    bytes=$4
    current_lines=$(wc -l < "$source")
    current_bytes=$(wc -c < "$source")
    if [ "$current_lines" -ne "$lines" ] || [ "$current_bytes" -gt "$bytes" ]; then
        printf '%s: base is %s lines/%s bytes, expected %s lines/at most %s bytes\n' \
            "$target" "$current_lines" "$current_bytes" "$lines" "$bytes" >&2
        exit 1
    fi
    awk -v extra="$((bytes - current_bytes))" -v count="$lines" '
        {
            add = int(extra / count) + (NR <= (extra % count) ? 1 : 0)
            printf "%s", $0
            for (i = 0; i < add; i++) printf " "
            printf "\n"
        }
    ' "$source" > "$target"
    test "$(wc -l < "$target")" -eq "$lines"
    test "$(wc -c < "$target")" -eq "$bytes"
}

awk 'BEGIN {
    for (i = 1; i <= 6000; i++) {
        if (i == 1) print "import numpy as np"
        else if (i <= 61) print sprintf("s%02d = f\"row={%d:04d} {{ok}}\"", i - 1, i)
        else if (i % 11 == 0) print sprintf("a%d = np.asarray(d%d)", i, i)
        else if (i % 11 == 1) print sprintf("def norm_%d(v):", i)
        else if (i % 11 == 2) print "    n = np.sum(v) or 1"
        else if (i % 11 == 3) print "    return v / n"
        else if (i % 11 == 4) print sprintf("d%d = {\"x\": [%d]}", i, i)
        else if (i % 11 == 5) print sprintf("d%d = \047\047\047science\047\047\047", i)
        else if (i % 11 == 6) print sprintf("n%d = \"\"\"array\"\"\"", i)
        else if (i % 11 == 7) print sprintf("r%d = r\047\047\047C:\\data\047\047\047", i)
        else if (i % 11 == 8) print sprintf("q%d = r\"\"\"C:\\tmp\"\"\"", i)
        else if (i % 11 == 9) print sprintf("z%d = a%d[:, ::-1]", i, i - 9)
        else print sprintf("v%d = %d ** 2", i, i)
    }
}' > "$scratch/python"
finish_fixture "$scratch/python" "$out/py_kitchen.py" 6000 $((190 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 6000; i++) {
        if (i <= 40) printf "const RAW_%02d: &str = r###\"raw::%d\"###;\n", i, i
        else if (i == 41) print "/* /* /* /* /* /* /* /* /* /* /* /* /* /* /* /* /* /* /* /* core */ */ */ */ */ */ */ */ */ */ */ */ */ */ */ */ */ */ */ */"
        else if (i % 9 == 0) printf "fn map_%d<T: Copy + Ord>(x: T) -> Option<T> { Some(x) }\n", i
        else if (i % 9 == 1) printf "struct Node%d<'\''a, T> { value: T, next: &'\''a [T] }\n", i
        else if (i % 9 == 2) printf "impl<'\''a, T: Clone> Node%d<'\''a, T> {\n", i - 1
        else if (i % 9 == 3) print "    fn copied(&self) -> T { self.value.clone() }"
        else if (i % 9 == 4) print "}"
        else if (i % 9 == 5) printf "let byte_%d = b'\''x'\'';\n", i
        else if (i % 9 == 6) printf "let life_%d: &'\''static str = \"oak\";\n", i
        else if (i % 9 == 7) printf "match x%d { Some(v)=>v, None=>0 };\n", i
        else printf "// generic workload row %d\n", i
    }
}' > "$scratch/rust"
finish_fixture "$scratch/rust" "$out/rs_kitchen.rs" 6000 $((210 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        if (i == 1) print "//go:build linux && !tiny"
        else if (i == 2) print "package kitchen"
        else if (i % 8 == 0) printf "type R%d struct { I int `json:\"id\"`; N string `yaml:\"n\"` }\n", i
        else if (i % 8 == 1) printf "func map%d(xs []int) []int {\n", i
        else if (i % 8 == 2) print "\tout := make([]int, 0, len(xs))"
        else if (i % 8 == 3) print "\tfor _, x := range xs { out = append(out, x*x) }"
        else if (i % 8 == 4) print "\treturn out"
        else if (i % 8 == 5) print "}"
        else if (i % 8 == 6) printf "var raw%d = `line one\\nline two`\n", i
        else printf "const value%d = %d\n", i, i
    }
}' > "$scratch/go"
finish_fixture "$scratch/go" "$out/go_kitchen.go" 5000 $((150 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        if (i <= 200) printf "const tpl%03d = `row ${items[%d]?.name ?? `nested ${%d}`}`;\n", i, i, i
        else if (i <= 280) printf "const re%03d = /[a-z\\/]+\\d{2,4}/gi;\n", i - 200
        else if (i % 8 == 0) printf "interface R%d<T> { v: T; id: number }\n", i
        else if (i % 8 == 1) printf "function f%d<T>(xs: T[]): T | void {\n", i
        else if (i % 8 == 2) print "  return xs.find(Boolean);"
        else if (i % 8 == 3) print "}"
        else if (i % 8 == 4) printf "const n%d: number = %d;\n", i, i
        else if (i % 8 == 5) printf "export type K%d = keyof R%d<object>;\n", i, i - 5
        else if (i % 8 == 6) printf "const ok%d = n%d > 0;\n", i, i - 2
        else print "// TypeScript row"
    }
}' > "$scratch/typescript"
finish_fixture "$scratch/typescript" "$out/ts_kitchen.ts" 5000 $((165 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 4000; i++) {
        if (i == 1) print "module kitchen_arrays"
        else if (i % 9 == 0) printf "subroutine scale_%d(a, n)\n", i
        else if (i % 9 == 1) print "  real(kind=8), dimension(:), intent(inout) :: a"
        else if (i % 9 == 2) print "  integer, intent(in) :: n"
        else if (i % 9 == 3) print "  a(1:n) = a(1:n) * 2.0d0 &"
        else if (i % 9 == 4) print "             + 1.0d0"
        else if (i % 9 == 5) print "  write(*,*) '\''C:\\path\\n'\''"
        else if (i % 9 == 6) print "  print *, '\''it'\''\''\''s free form'\''"
        else if (i % 9 == 7) print "end subroutine"
        else printf "! free-form workload %d\n", i
    }
}' > "$scratch/fortran-free"
finish_fixture "$scratch/fortran-free" "$out/f90_kitchen.f90" 4000 $((140 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 4000; i++) {
        if (i % 8 == 0) printf "%5d FORMAT(1X,'\''FIXED FORM ROW'\'',I6)\n", i % 99999
        else if (i % 8 == 1) printf "      SUBROUTINE STEP%04d(A,N)\n", i
        else if (i % 8 == 2) print "      INTEGER N,I"
        else if (i % 8 == 3) print "      REAL A(N)"
        else if (i % 8 == 4) print "      DO 100 I=1,N"
        else if (i % 8 == 5) print "     1 A(I)=A(I)+FLOAT(I)"
        else if (i % 8 == 6) print "  100 CONTINUE"
        else print "C     FIXED FORM COMMENT WITH TEXT BEYOND COLUMN SEVENTY-TWO 1234567890"
    }
}' > "$scratch/fortran-fixed"
finish_fixture "$scratch/fortran-fixed" "$out/f77_kitchen.f" 4000 $((130 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 4000; i++) {
        if (i <= 30) printf "block_%02d: |2-\n", i
        else if (i <= 60) printf "  scalar line %02d\n", i - 30
        else if (i % 10 == 0) printf "root_%d:\n", i
        else if (i % 10 == 1) print "  level_one:"
        else if (i % 10 == 2) print "    level_two:"
        else if (i % 10 == 3) print "      level_three:"
        else if (i % 10 == 4) print "        level_four:"
        else if (i % 10 == 5) print "          flow: [a, {b: [c, {d: e}]}]"
        else if (i % 10 == 6) print "          enabled: true"
        else if (i % 10 == 7) print "          count: 42"
        else if (i % 10 == 8) print "          anchor: &oak value"
        else print "          alias: *oak"
    }
}' > "$scratch/yaml"
finish_fixture "$scratch/yaml" "$out/yaml_kitchen.yml" 4000 $((96 * 1024))

# The first JSON record has exactly 2 MiB before its newline.  The remaining
# 5,000 pretty-shaped records bring the frozen file to exactly 2,252 KiB.
awk 'BEGIN {
    prefix = "{\"payload\":\""
    suffix = "\"}"
    printf "%s", prefix
    for (i = length(prefix) + length(suffix); i < 2097152; i++) printf "x"
    printf "%s\n", suffix
}' > "$out/json_kitchen.json"
awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        if (i % 5 == 1) printf "  { \"id\": %d, \"name\": \"row-%d\",\n", i, i
        else if (i % 5 == 2) print "    \"enabled\": true, \"ratio\": 1.25e+3,"
        else if (i % 5 == 3) print "    \"escapes\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u263a\","
        else if (i % 5 == 4) print "    \"values\": [null, false, -12.5e-2]"
        else print "  },"
    }
}' > "$scratch/json-pretty"
finish_fixture "$scratch/json-pretty" "$scratch/json-pretty-padded" 5000 \
    $((2252 * 1024 - 2097152 - 1))
cat "$scratch/json-pretty-padded" >> "$out/json_kitchen.json"
test "$(wc -l < "$out/json_kitchen.json")" -eq 5001
test "$(wc -c < "$out/json_kitchen.json")" -eq $((2252 * 1024))
first_bytes=$(awk 'NR == 1 { print length($0); exit }' "$out/json_kitchen.json")
test "$first_bytes" -eq 2097152

printf 'generated Sprint 42 syntax perf fixtures\n'
