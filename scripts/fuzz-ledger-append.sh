#!/bin/sh
# Insert campaign rows at the end of the coverage table, before its schedule.
set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 LEDGER ROW_FILE..." >&2
    exit 2
fi

ledger=$1
shift
if [ ! -f "$ledger" ]; then
    echo "fuzz-ledger-append: missing ledger $ledger" >&2
    exit 2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-ledger.XXXXXX")
ledger_tmp=
cleanup()
{
    rm -rf "$tmp_dir"
    if [ -n "$ledger_tmp" ]; then
        rm -f "$ledger_tmp"
    fi
}
trap cleanup EXIT HUP INT TERM
rows=$tmp_dir/rows
: >"$rows"
for row_file do
    if [ "$(wc -l <"$row_file" | tr -d ' ')" -ne 1 ] ||
       ! grep -E '^\| [0-9]{4}-[0-9]{2}-[0-9]{2} \| `[^`]+` \| `fuzz_[A-Za-z0-9_]+` \|' \
           "$row_file" >/dev/null; then
        echo "fuzz-ledger-append: malformed row file $row_file" >&2
        exit 2
    fi
    cat "$row_file" >>"$rows"
done

ledger_tmp=$(mktemp "$(dirname "$ledger")/.yew-fuzz-ledger.XXXXXX")
LC_ALL=C awk -v rows="$rows" '
    BEGIN {
        row_count = 0
        while ((getline row < rows) > 0) campaign[++row_count] = row
        close(rows)
    }
    { line[NR] = $0 }
    END {
        marker = 0
        for (i = 1; i <= NR; i++) {
            if (line[i] == "## Pinned schedule") {
                marker = i
                break
            }
        }
        if (marker == 0) {
            for (i = 1; i <= NR; i++) print line[i]
            for (i = 1; i <= row_count; i++) print campaign[i]
        } else {
            last = marker - 1
            if (last > 0 && line[last] == "") last--
            for (i = 1; i <= last; i++) print line[i]
            for (i = 1; i <= row_count; i++) print campaign[i]
            print ""
            for (i = marker; i <= NR; i++) print line[i]
        }
    }
' "$ledger" >"$ledger_tmp"
chmod 0644 "$ledger_tmp"
mv "$ledger_tmp" "$ledger"
ledger_tmp=
