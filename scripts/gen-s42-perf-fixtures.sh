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
        else if (i % 9 == 6) printf "  print *, %cit%c%cs free form%c\n", 39, 39, 39, 39
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

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 16
        if (phase == 0) printf "let cooked_%d = \"value={{raw}} {%d + {inner: 2}.inner:>8}\";\n", i, i
        else if (phase == 1) printf "let triple_%d = \"\"\"row {%d:>{width}}\n", i, i
        else if (phase == 2) print "nested {{ braces }} and {call({depth: 2})}"
        else if (phase == 3) print "\"\"\";"
        else if (phase == 4) printf "let raw_%d = r###\"raw { braces } \\\\ path\"###;\n", i
        else if (phase == 5) printf "let generalized_%d = SQL\"select * from rows where id = ?\";\n", i
        else if (phase == 6) printf "fn arrow_%d(value: i64) -> i64 {\n", i
        else if (phase == 7) print "    if value > 0 { return value; } else { return 0; }"
        else if (phase == 8) print "}"
        else if (phase == 9) printf "#[bench(case = \"row-%d\")]\n", i
        else if (phase == 10) printf "unsafe c { int row_%d = %d; /* neutral C body */ }\n", i, i
        else if (phase == 11) printf "proc worker_%d(in value: i64, out result: i64) {\n", i
        else if (phase == 12) print "    spawn task(value); await result;"
        else if (phase == 13) print "}"
        else if (phase == 14) printf "//! Wolf performance row %d\n", i
        else printf "const mask_%d = 0xff_u64 | 0b1010_0101;\n", i
    }
}' > "$scratch/wolf"
finish_fixture "$scratch/wolf" "$out/wolf_kitchen.lu" 5000 $((256 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 6000; i++) {
        phase = (i - 1) % 12
        if (phase == 0) printf "#define YEWTAG_%d(x) ((x) + %d)\n", i, i
        else if (phase == 1) printf "template <class T, class U = std::vector<std::pair<T, int>>>\n"
        else if (phase == 2) printf "[[nodiscard]] auto map_%d(T value) -> U {\n", i
        else if (phase == 3) printf "    auto raw = R\"tag(row %d \\\\ \\\" text)tag\";\n", i
        else if (phase == 4) print "    if constexpr (requires { value.begin(); }) {"
        else if (phase == 5) print "        return U{value, 42}; // template branch"
        else if (phase == 6) print "    } else {"
        else if (phase == 7) print "        return U{};"
        else if (phase == 8) print "    }"
        else if (phase == 9) print "}"
        else if (phase == 10) printf "static_assert(sizeof(long long) >= %d);\n", i % 8
        else print "/* C++ block comment with <nested<vector<int>>> punctuation */"
    }
}' > "$scratch/native-systems"
finish_fixture "$scratch/native-systems" "$out/native_systems.cpp" 6000 $((320 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 14
        if (phase == 0) printf "data class Row%d<T>(val value: T, val label: String?)\n", i
        else if (phase == 1) printf "val raw%d = \"\"\"row ${items[%d]?.name ?: \"missing\"}\n", i, i
        else if (phase == 2) print "literal dollars and braces ${buildString { append(\"ok\") }}"
        else if (phase == 3) print "\"\"\".trimIndent()"
        else if (phase == 4) print "/* outer comment"
        else if (phase == 5) print "   /* nested comment */"
        else if (phase == 6) print "*/"
        else if (phase == 7) printf "fun <T : Comparable<T>> choose%d(a: T?, b: T): T =\n", i
        else if (phase == 8) print "    a?.takeIf { it > b } ?: b"
        else if (phase == 9) printf "@Deprecated(\"row-%d\")\n", i
        else if (phase == 10) printf "val range%d = 1 until %d\n", i, i
        else if (phase == 11) printf "when (val x%d = range%d.first) {\n", i, i - 1
        else if (phase == 12) print "    in 0..10 -> println(x)"
        else print "}"
    }
}' > "$scratch/native-vm"
finish_fixture "$scratch/native-vm" "$out/native_vm.kt" 5000 $((240 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 16
        if (phase == 0) printf "class Row%d\n", i
        else if (phase == 1) printf "  VALUE = %%Q{row #{%d + 1}}\n", i
        else if (phase == 2) printf "  REGEX = /row\\/#{%d}\\s+/ix\n", i
        else if (phase == 3) print "  TEXT = <<~YEW_DOC"
        else if (phase == 4) printf "    interpolated #{VALUE} row %d\n", i
        else if (phase == 5) print "    nested-looking #{items.map { |x| x.to_s }.join(\",\")}"
        else if (phase == 6) print "  YEW_DOC"
        else if (phase == 7) print "  RAW = <<-'YEW_RAW'"
        else if (phase == 8) print "    literal #{not_interpolated}"
        else if (phase == 9) print "  YEW_RAW"
        else if (phase == 10) printf "  def call_%d(value)\n", i
        else if (phase == 11) print "    value&.then { _1.to_s } || :missing"
        else if (phase == 12) print "  end"
        else if (phase == 13) print "end"
        else if (phase == 14) printf "symbol_%d = :row_%d\n", i, i
        else print "# Ruby performance row"
    }
}' > "$scratch/native-script"
finish_fixture "$scratch/native-script" "$out/native_script.rb" 5000 $((240 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 12
        if (phase == 0) print "{-# LANGUAGE DataKinds, TypeFamilies #-}"
        else if (phase == 1) print "{- outer block"
        else if (phase == 2) print "   {- nested block -}"
        else if (phase == 3) print "-}"
        else if (phase == 4) printf "data Row%d a = Row%d { value%d :: a } deriving (Eq, Show)\n", i, i, i
        else if (phase == 5) printf "mapRow%d :: (a -> b) -> Row%d a -> Row%d b\n", i, i - 1, i - 1
        else if (phase == 6) printf "mapRow%d f (Row%d x) = Row%d (f x)\n", i - 1, i - 2, i - 2
        else if (phase == 7) printf "qualified%d = Data.List.map (+ %d) [1..10]\n", i, i
        else if (phase == 8) printf "infixl 6 <+%d+>\n", i
        else if (phase == 9) printf "a <+%d+> b = a + b\n", i - 1
        else if (phase == 10) printf "type family Result%d a where\n", i
        else print "  Result1 Int = Integer"
    }
}' > "$scratch/native-functional"
finish_fixture "$scratch/native-functional" "$out/native_functional.hs" 5000 $((240 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 10
        if (phase == 0) printf "<row id=\"%d\" title=\"a large &amp; deterministic attribute %d\" data-owner=\"tenseley-flow\" data-profile=\"native-pack-performance\">\n", i, i
        else if (phase == 1) print "  <![CDATA[raw <xml> & bytes ]]>"
        else if (phase == 2) print "  <!-- comment with entities &lt; &gt; -->"
        else if (phase == 3) printf "  <value enabled=\"true\">&#x%x;</value>\n", i
        else if (phase == 4) print "  <?yew highlight=\"native-data\"?>"
        else if (phase == 5) print "  <nested xmlns:y=\"urn:yew:perf\">"
        else if (phase == 6) printf "    <y:item key=\"row-%d\">text &quot; value</y:item>\n", i
        else if (phase == 7) print "  </nested>"
        else if (phase == 8) print "</row>"
        else print "<!-- XML performance separator -->"
    }
}' > "$scratch/native-data"
finish_fixture "$scratch/native-data" "$out/native_data.xml" 5000 $((300 * 1024))

awk 'BEGIN {
    for (i = 1; i <= 5000; i++) {
        phase = (i - 1) % 14
        if (phase == 0) printf "resource \"yew_row\" \"r%d\" {\n", i
        else if (phase == 1) printf "  name = \"row-${var.prefix}-%d\"\n", i
        else if (phase == 2) print "  body = <<-YEW_DOC"
        else if (phase == 3) print "    %{ if var.enabled }"
        else if (phase == 4) print "    value = ${local.rows[0].name}"
        else if (phase == 5) print "    %{ else }"
        else if (phase == 6) print "    disabled"
        else if (phase == 7) print "    %{ endif }"
        else if (phase == 8) print "  YEW_DOC"
        else if (phase == 9) print "  raw = <<YEW_RAW"
        else if (phase == 10) print "literal ${not_a_template_here}"
        else if (phase == 11) print "YEW_RAW"
        else if (phase == 12) print "}"
        else print "# HCL performance row"
    }
}' > "$scratch/native-build"
finish_fixture "$scratch/native-build" "$out/native_build.hcl" 5000 $((240 * 1024))

printf 'generated Sprint 42/42.5 syntax perf fixtures\n'
