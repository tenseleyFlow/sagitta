#!/bin/sh

set -eu
export LC_ALL=C

program=${0##*/}
generator=
yew=
busybox=
init=
output=
file_list=
runtime=
max_bytes=12582912
copies=
scratch=
out_tmp=
list_tmp=

fail()
{
    echo "$program: $*" >&2
    exit 1
}

usage()
{
    cat >&2 <<EOF
usage: $program --generator PATH --yew PATH --busybox PATH --init PATH
       --output PATH --file-list PATH [--runtime DIR]
       [--copy SOURCE DESTINATION]... [--copy-exec SOURCE DESTINATION]...
       [--max-bytes N]
EOF
    exit 2
}

absolute_file()
{
    case $2 in
        /*) ;;
        *) fail "$1 must be an absolute path: $2" ;;
    esac
    [ -f "$2" ] || fail "$1 is not a regular file: $2"
}

image_path()
{
    case $1 in
        /*) path=${1#/} ;;
        *) fail "image destination must begin with /: $1" ;;
    esac
    case /$path/ in
        *'/../'*|*'/./'*|*'//'*) fail "unsafe image destination: $1" ;;
    esac
    [ -n "$path" ] || fail 'image destination cannot be /'
    printf '%s\n' "$path"
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --generator) [ "$#" -ge 2 ] || usage; generator=$2; shift 2 ;;
        --yew) [ "$#" -ge 2 ] || usage; yew=$2; shift 2 ;;
        --busybox) [ "$#" -ge 2 ] || usage; busybox=$2; shift 2 ;;
        --init) [ "$#" -ge 2 ] || usage; init=$2; shift 2 ;;
        --output) [ "$#" -ge 2 ] || usage; output=$2; shift 2 ;;
        --file-list) [ "$#" -ge 2 ] || usage; file_list=$2; shift 2 ;;
        --runtime) [ "$#" -ge 2 ] || usage; runtime=$2; shift 2 ;;
        --max-bytes) [ "$#" -ge 2 ] || usage; max_bytes=$2; shift 2 ;;
        --copy)
            [ "$#" -ge 3 ] || usage
            [ -n "$copies" ] || copies=$(mktemp "${TMPDIR:-/tmp}/yew-embed-copies.XXXXXX")
            printf '%s\t%s\t0644\n' "$2" "$3" >>"$copies"
            shift 3
            ;;
        --copy-exec)
            [ "$#" -ge 3 ] || usage
            [ -n "$copies" ] || copies=$(mktemp "${TMPDIR:-/tmp}/yew-embed-copies.XXXXXX")
            printf '%s\t%s\t0755\n' "$2" "$3" >>"$copies"
            shift 3
            ;;
        --help|-h) usage ;;
        *) usage ;;
    esac
done

[ -n "$generator" ] || usage
[ -n "$yew" ] || usage
[ -n "$busybox" ] || usage
[ -n "$init" ] || usage
[ -n "$output" ] || usage
[ -n "$file_list" ] || usage
absolute_file generator "$generator"
absolute_file yew "$yew"
absolute_file busybox "$busybox"
absolute_file init "$init"
case $output in /*) ;; *) fail "output must be an absolute path: $output" ;; esac
case $file_list in /*) ;; *) fail "file-list must be an absolute path: $file_list" ;; esac
[ "$output" != "$file_list" ] || fail 'output and file-list must differ'
case $max_bytes in ''|*[!0-9]*) fail "invalid max-bytes: $max_bytes" ;; esac
[ "$max_bytes" -gt 0 ] || fail 'max-bytes must be positive'
if [ -n "$runtime" ]; then
    case $runtime in /*) ;; *) fail "runtime must be an absolute path: $runtime" ;; esac
    [ -d "$runtime" ] || fail "runtime is not a directory: $runtime"
fi

cleanup()
{
    [ -z "$scratch" ] || rm -rf "$scratch"
    [ -z "$copies" ] || rm -f "$copies"
    [ -z "$out_tmp" ] || rm -f "$out_tmp"
    [ -z "$list_tmp" ] || rm -f "$list_tmp"
}
trap cleanup EXIT HUP INT TERM
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-image.XXXXXX")
root=$scratch/root
raw=$scratch/embed.cpio
packed=$scratch/embed.cpio.gz
list=$scratch/embed.files

mkdir -p "$root/bin" "$root/dev" "$root/etc" "$root/proc" "$root/root" \
    "$root/run" "$root/sys" "$root/tmp" "$root/work"
chmod 0755 "$root" "$root/bin" "$root/dev" "$root/etc" "$root/proc" \
    "$root/root" "$root/run" "$root/sys" "$root/work"
chmod 01777 "$root/tmp"
cp "$yew" "$root/bin/yew"
cp "$busybox" "$root/bin/busybox"
cp "$init" "$root/init"
chmod 0755 "$root/bin/yew" "$root/bin/busybox" "$root/init"

for applet in awk cat chmod cmp cp cut dd dmesg echo env find grep head \
              ln mkdir mount mv poweroff printf rm sed sh sleep sort stat \
              sha256sum sync tail time timeout touch tr umount wc; do
    ln -s busybox "$root/bin/$applet"
done

if [ -n "$runtime" ]; then
    mkdir -p "$root/runtime"
    (cd "$runtime" && tar -cf - .) | (cd "$root/runtime" && tar -xf -)
fi

if [ -n "$copies" ]; then
    tab=$(printf '\t')
    while IFS="$tab" read -r source destination mode; do
        [ -n "$source" ] || fail 'copy source cannot be empty'
        absolute_file copy-source "$source"
        relative=$(image_path "$destination")
        parent=${relative%/*}
        if [ "$parent" != "$relative" ]; then
            mkdir -p "$root/$parent"
            chmod 0755 "$root/$parent"
        fi
        cp "$source" "$root/$relative"
        chmod "$mode" "$root/$relative"
    done <"$copies"
fi

# The archive must not inherit a restrictive caller umask or directory modes
# from a copied runtime tree.  Only /tmp is deliberately world-writable.
find "$root" -type d -exec chmod 0755 '{}' ';'
chmod 01777 "$root/tmp"

"$generator" "$root" "$raw" "$list"
gzip -n -9 -c "$raw" >"$packed"
bytes=$(wc -c <"$packed" | tr -d ' ')
case $bytes in ''|*[!0-9]*) fail 'cannot measure generated image' ;; esac
if [ "$bytes" -gt "$max_bytes" ]; then
    fail "image is $bytes bytes; limit is $max_bytes"
fi

out_dir=${output%/*}
list_dir=${file_list%/*}
[ -n "$out_dir" ] || out_dir=/
[ -n "$list_dir" ] || list_dir=/
mkdir -p "$out_dir" "$list_dir"
out_tmp=$(mktemp "$out_dir/.${output##*/}.XXXXXX")
list_tmp=$(mktemp "$list_dir/.${file_list##*/}.XXXXXX")
cp "$packed" "$out_tmp"
cp "$list" "$list_tmp"
chmod 0644 "$out_tmp" "$list_tmp"
mv "$out_tmp" "$output"
out_tmp=
mv "$list_tmp" "$file_list"
list_tmp=
printf '%s\n' "embedded image $output $bytes bytes"
