#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-size-tools.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "size tools test: $*" >&2
    exit 1
}

mkdir -p "$scratch/build/src/util" "$scratch/build/src/fl" "$scratch/build/src/mod/git" "$scratch/bin"
printf 'lsp ai fuss plugins\n' >"$scratch/build/mods.stamp"
printf x >"$scratch/build/src/util/a.o"
printf x >"$scratch/build/src/fl/a.o"
printf x >"$scratch/build/src/mod/git/a.o"
printf x >"$scratch/build/src/mod/mods.o"
dd if=/dev/zero of="$scratch/yew" bs=1 count=5000 2>/dev/null

cat >"$scratch/Makefile" <<'EOF'
MODDIR_lsp     := lsp
MODDIR_ai      := ai
MODDIR_fuss    := git
MODDIR_plugins := plug
EOF

cat >"$scratch/bin/fake-size" <<'EOF'
#!/bin/sh
for last do :; done
case $last in
*/src/util/a.o) cat <<'OUT'
file :
section size addr
.text.foo 100 0
.rodata.str1.1 20 0
.data.rel.ro 5 0
.debug_info 70 0
OUT
;;
*/src/fl/a.o)
if [ "${BIG_FL:-0}" = 1 ]; then text=20100; else text=100; fi
cat <<OUT
file :
section size addr
.text $text 0
.rodata 25 0
.bss.cache 10 0
OUT
;;
*/src/mod/git/a.o) cat <<'OUT'
file :
section size addr
.text.git 80 0
.rodata.git 20 0
OUT
;;
*/src/mod/mods.o) cat <<'OUT'
file :
section size addr
.text.registry 10 0
OUT
;;
*debug*) cat <<'OUT'
file :
section size addr
.text 250 0
.debug_info 1 0
OUT
;;
*) cat <<'OUT'
file :
section size addr
.text 250 0
.rodata 50 0
.data 5 0
.bss 10 0
OUT
;;
esac
EOF
cat >"$scratch/bin/fake-nm" <<'EOF'
#!/bin/sh
for last do :; done
case $last in
*/src/util/a.o) printf '%s\n' '0 0000000000000100 T util_big' '0 0000000000000000 t thunk' ;;
*/src/fl/a.o)
if [ "${BIG_FL:-0}" = 1 ]; then size=0000000000020100; else size=0000000000000100; fi
printf '%s\n' "0 $size T fl_big"
;;
*/src/mod/git/a.o) printf '%s\n' '0 0000000000000080 T fuss_big' ;;
*/src/mod/mods.o) printf '%s\n' '0 0000000000000010 T mods_name' ;;
esac
EOF
chmod +x "$scratch/bin/fake-size" "$scratch/bin/fake-nm"

MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --top 3 >"$scratch/ledger"

grep -E '^core\.fl +100 +25 +0 +10 +135 ' "$scratch/ledger" >/dev/null || fail 'did not fold core.fl sections'
grep -E '^core\.util +100 +20 +5 +0 +125 ' "$scratch/ledger" >/dev/null || fail 'did not fold suffixed sections'
grep -E '^mod\.fuss +80 +20 +0 +0 +100 ' "$scratch/ledger" >/dev/null || fail 'did not read git-to-fuss module mapping'
grep -E '^core\.main +10 +0 +0 +0 +10 ' "$scratch/ledger" >/dev/null || fail 'did not attribute the shared module registry to core'
[ "$(grep -c '^  *100  core\.' "$scratch/ledger")" -eq 2 ] || fail 'top-symbol tie fixture missing'
first_tie=$(grep '^  *100  core\.' "$scratch/ledger" | sed -n '1p')
case $first_tie in *core.fl*) ;; *) fail 'symbol tie was not broken by bucket name' ;; esac
grep -F '.debug_info' "$scratch/ledger" >/dev/null || fail 'non-folded section was silently discarded'
grep -F '# zero-sized-symbols 1' "$scratch/ledger" >/dev/null || fail 'zero-sized symbol count missing'
grep -E '^unattributed +0 +0 +0 +0 +0 ' "$scratch/ledger" >/dev/null || fail 'explicit unattributed row missing'

cp "$scratch/ledger" "$scratch/baseline"
MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --baseline "$scratch/baseline" --format tsv >/dev/null ||
    fail 'identical baseline failed'

set +e
BIG_FL=1 MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --baseline "$scratch/baseline" >"$scratch/growth.out" 2>"$scratch/growth.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'bucket growth over the 16 KiB floor passed'
grep -F 'bucket core.fl grew by 20000 bytes' "$scratch/growth.err" >/dev/null || fail 'growth failure was unclear'
grep -F -- '-- grown since baseline (>1 KiB)' "$scratch/growth.out" >/dev/null || fail 'grown-symbol block missing'
grep -E '^  \+ *20000 +core\.fl ' "$scratch/growth.out" >/dev/null || fail 'grown symbol delta missing'

cp "$scratch/yew" "$scratch/yew-debug"
set +e
MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew-debug" >/dev/null 2>"$scratch/debug.err"
status=$?
set -e
[ "$status" -eq 2 ] || fail 'unstripped binary was accepted'
grep -F 'binary is not stripped' "$scratch/debug.err" >/dev/null || fail 'unstripped refusal was unclear'

cat >"$scratch/budgets" <<'EOF'
full 6000
minimal 1000
lsp-only 1200
ai-only 1200
fuss-only 1200
plugins-only 1200
EOF
entries=
for name in full minimal lsp-only ai-only fuss-only plugins-only; do
    bytes=1100
    [ "$name" = minimal ] && bytes=900
    [ "$name" = full ] && bytes=1400
    dd if=/dev/zero of="$scratch/$name" bs=1 count="$bytes" 2>/dev/null
    entries="$entries $name=$scratch/$name"
done
# shellcheck disable=SC2086
"$repo/scripts/size.sh" --budgets "$scratch/budgets" $entries >/dev/null || fail 'valid size set failed'

dd if=/dev/zero of="$scratch/full" bs=1 count=2000 2>/dev/null
set +e
# shellcheck disable=SC2086
"$repo/scripts/size.sh" --budgets "$scratch/budgets" $entries >/dev/null 2>"$scratch/additive.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'non-additive full build passed'
grep -F 'exceeds additive limit' "$scratch/additive.err" >/dev/null || fail 'additivity failure was unclear'

cat >"$scratch/tight" <<'EOF'
one 10
EOF
set +e
"$repo/scripts/size.sh" --budgets "$scratch/tight" "one=$scratch/full" >/dev/null 2>"$scratch/absolute.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'absolute budget breach passed'
grep -F '1 gate(s) failed' "$scratch/absolute.err" >/dev/null || fail 'absolute failure was unclear'

echo 'size tools test: ok'
