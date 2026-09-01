#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
make_cmd=${MAKE:-make}
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-size-tools.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "size tools test: $*" >&2
    exit 1
}

mkdir -p "$scratch/build/src/util" "$scratch/build/src/fl" \
    "$scratch/build/src/mod/git" "$scratch/build/src/mod/lsp" \
    "$scratch/build/gen" "$scratch/bin"
printf 'lsp ai fuss plugins\n' >"$scratch/build/mods.stamp"
printf x >"$scratch/build/src/util/a.o"
printf x >"$scratch/build/src/fl/a.o"
printf x >"$scratch/build/src/mod/git/a.o"
printf x >"$scratch/build/src/mod/lsp/a.o"
printf x >"$scratch/build/src/mod/mods.o"
printf x >"$scratch/build/gen/runtime_blob.o"
dd if=/dev/zero of="$scratch/yew" bs=1 count=5000 2>/dev/null
dd if=/dev/zero of="$scratch/yew-no-gc" bs=1 count=5200 2>/dev/null

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
*/src/mod/lsp/a.o)
if [ "${SEEDED_LSP:-0}" = 1 ]; then text=41050; else text=90; fi
cat <<OUT
file :
section size addr
.text.lsp_seed $text 0
.rodata.lsp_seed 30 0
OUT
;;
*/src/mod/mods.o) cat <<'OUT'
file :
section size addr
.text.registry 10 0
OUT
;;
*/gen/runtime_blob.o) cat <<'OUT'
file :
section size addr
.text.accessors 12 0
.rodata.runtime 256 0
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
*/src/util/a.o)
printf '%s\n' '0 0000000000000100 T util_big'
if [ "${ZERO_SYMBOLS:-0}" = 1 ]; then printf '%s\n' '0 0000000000000000 t thunk'; fi
;;
*/src/fl/a.o)
if [ "${BIG_FL:-0}" = 1 ]; then size=0000000000020100; else size=0000000000000100; fi
printf '%s\n' "0 $size T fl_big"
;;
*/src/mod/git/a.o) printf '%s\n' '0 0000000000000080 T fuss_big' ;;
*/src/mod/lsp/a.o)
if [ "${SEEDED_LSP:-0}" = 1 ]; then size=0000000000041050; else size=0000000000000090; fi
printf '%s\n' "0 $size R lsp_seed_40k"
;;
*/src/mod/mods.o) printf '%s\n' '0 0000000000000010 T mods_name' ;;
*/gen/runtime_blob.o) printf '%s\n' '0 0000000000000256 R runtime_blob' ;;
esac
EOF
chmod +x "$scratch/bin/fake-size" "$scratch/bin/fake-nm"

MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" \
    --without-gc "$scratch/yew-no-gc" --top 4 >"$scratch/ledger"

grep -E '^core\.fl +100 +25 +0 +10 +135 ' "$scratch/ledger" >/dev/null || fail 'did not fold core.fl sections'
grep -E '^core\.util +100 +20 +5 +0 +125 ' "$scratch/ledger" >/dev/null || fail 'did not fold suffixed sections'
grep -E '^mod\.fuss +80 +20 +0 +0 +100 ' "$scratch/ledger" >/dev/null || fail 'did not read git-to-fuss module mapping'
grep -E '^mod\.lsp +90 +30 +0 +0 +120 ' "$scratch/ledger" >/dev/null || fail 'did not attribute the LSP seed fixture'
grep -E '^core\.main +10 +0 +0 +0 +10 ' "$scratch/ledger" >/dev/null || fail 'did not attribute the shared module registry to core'
grep -E '^runtime\.embedded +12 +256 +0 +0 +268 ' "$scratch/ledger" >/dev/null || fail 'did not attribute the generated runtime blob'
[ "$(grep -c '^  *100  core\.' "$scratch/ledger")" -eq 2 ] || fail 'top-symbol tie fixture missing'
first_tie=$(grep '^  *100  core\.' "$scratch/ledger" | sed -n '1p')
case $first_tie in *core.fl*) ;; *) fail 'symbol tie was not broken by bucket name' ;; esac
grep -F '.debug_info' "$scratch/ledger" >/dev/null || fail 'non-folded section was silently discarded'
grep -F '# unattributed-symbols 0/6 (0.0%)   limit 2.0%' "$scratch/ledger" >/dev/null || fail 'unattributed symbol ratio missing'
grep -F '# on-disk 5000   object-file-backed 748   link-file-residue 4252' "$scratch/ledger" >/dev/null || fail 'file-backed link residue included BSS'
grep -F '# final-file-backed 305   final-bss 10   object-bss 10' "$scratch/ledger" >/dev/null || fail 'BSS accounting was not explicit'
grep -F '# gc_saved_bytes 200   without-gc-on-disk 5200' "$scratch/ledger" >/dev/null || fail 'linker-GC savings were not recorded'

set +e
ZERO_SYMBOLS=1 MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" >"$scratch/unattributed.out" 2>"$scratch/unattributed.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'unattributed ratio over 2% passed'
grep -F '# unattributed-symbols 1/7 (14.2%)   limit 2.0%' "$scratch/unattributed.out" >/dev/null || fail 'unattributed failure ratio missing'
grep -F 'unattributed zero-sized symbols 1/7 exceed 2%' "$scratch/unattributed.err" >/dev/null || fail 'unattributed failure was unclear'

cp "$scratch/ledger" "$scratch/baseline"
MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --baseline "$scratch/baseline" --format tsv >/dev/null ||
    fail 'identical baseline failed'

set +e
SEEDED_LSP=1 MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --baseline "$scratch/baseline" >"$scratch/growth.out" 2>"$scratch/growth.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'seeded 40 KiB LSP growth passed'
grep -F 'bucket mod.lsp grew by 40960 bytes' "$scratch/growth.err" >/dev/null || fail 'growth failure was unclear'
grep -F -- '-- grown since baseline (>1 KiB)' "$scratch/growth.out" >/dev/null || fail 'grown-symbol block missing'
grep -E '^  \+ *40960 +mod\.lsp .*lsp_seed_40k$' "$scratch/growth.out" >/dev/null || fail 'grown LSP symbol was not named'

# The explicit rebaseline path accepts the same intentional growth after the
# updated ledger is reviewed. The commit-message guard is tested separately.
SEEDED_LSP=1 MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" >"$scratch/seeded-baseline"
SEEDED_LSP=1 MAKEFILE="$scratch/Makefile" SIZE="$scratch/bin/fake-size" NM="$scratch/bin/fake-nm" CC=false \
    "$repo/scripts/size-ledger.sh" --build "$scratch/build" --binary "$scratch/yew" --baseline "$scratch/seeded-baseline" >/dev/null ||
    fail 'reviewed 40 KiB LSP rebaseline did not clear the growth gate'

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

if [ "$(uname -s)" = Linux ]; then
    "$make_cmd" -s -n -C "$repo" BUILD="$scratch/shipping" MODULES= SHIPPING=1 \
        "$scratch/shipping/yew" >"$scratch/shipping.flags"
    for flag in -O2 -DNDEBUG -ffunction-sections -fdata-sections \
                -fno-asynchronous-unwind-tables -fno-unwind-tables \
                -Wl,--gc-sections -Wl,--build-id=none; do
        grep -F -- "$flag" "$scratch/shipping.flags" >/dev/null ||
            fail "shipping profile omitted $flag"
    done
    "$make_cmd" -s -n -C "$repo" BUILD="$scratch/no-gc" MODULES= SHIPPING=1 \
        GC_SECTIONS=0 "$scratch/no-gc/yew" >"$scratch/no-gc.flags"
    if grep -F -- '-Wl,--gc-sections' "$scratch/no-gc.flags" >/dev/null; then
        fail 'GC_SECTIONS=0 retained linker garbage collection'
    fi
fi

echo 'size tools test: ok'
