#!/bin/sh

set -eu
export LC_ALL=C

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$script_dir")
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-shim-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
failed=0

extract_declarations()
{
    awk '
        function clean(line, out, p, q) {
            out = ""
            while (length(line) != 0) {
                if (comment) {
                    p = index(line, "*/")
                    if (p == 0) return out
                    line = substr(line, p + 2)
                    comment = 0
                }
                p = index(line, "/*")
                q = index(line, "//")
                if (q != 0 && (p == 0 || q < p))
                    return out substr(line, 1, q - 1)
                if (p == 0) return out line
                out = out substr(line, 1, p - 1)
                line = substr(line, p + 2)
                comment = 1
            }
            return out
        }
        function emit(text, name) {
            while (match(text, /yew_[[:alnum:]_]+[[:space:]]*\(/)) {
                name = substr(text, RSTART, RLENGTH)
                sub(/[[:space:]]*\($/, "", name)
                print name
                text = substr(text, RSTART + RLENGTH)
            }
        }
        {
            line = clean($0)
            statement = statement " " line
            if (index(line, ";") != 0) {
                emit(statement)
                statement = ""
            }
        }
    ' "$@" | sort -u
}

extract_definitions()
{
    awk '
        /^[[:space:]]*FUSS_SHIM[[:space:]]*\(/ {
            if (match($0, /yew_[[:alnum:]_]+/))
                print substr($0, RSTART, RLENGTH)
            next
        }
        /^[[:alpha:]_][[:alnum:]_ *]*yew_[[:alnum:]_]+[[:space:]]*\(/ {
            if (match($0, /yew_[[:alnum:]_]+/))
                print substr($0, RSTART, RLENGTH)
        }
    ' "$@" | sort -u
}

action_successes()
{
    awk '
        function is_action(name) {
            return name ~ /^yew_ai_cmd_/ ||
                   name ~ /^yew_fuss_cmd_/ ||
                   name ~ /^yew_git_cmd_/ ||
                   name ~ /^yew_plug_cmd_/ ||
                   name == "yew_fuss_mode_enter" ||
                   name == "yew_plug_main" || name == "yew_pkg_main" ||
                   (name ~ /^yew_lsp_(require|info|log|start|stop|diagnostics|diag_step|hover|signature|goto_|references|rename|symbols)$/)
        }
        function braces(text, ch, n, i) {
            n = 0
            for (i = 1; i <= length(text); i++)
                if (substr(text, i, 1) == ch) n++
            return n
        }
        {
            line = $0
            if (current == "" &&
                line ~ /^[[:alpha:]_][[:alnum:]_ *]*yew_[[:alnum:]_]+[[:space:]]*\(/ &&
                match(line, /yew_[[:alnum:]_]+/)) {
                name = substr(line, RSTART, RLENGTH)
                if (is_action(name)) {
                    current = name
                    depth = 0
                    opened = 0
                }
            }
            if (current != "") {
                if (line ~ /return[[:space:]]+(true|YEW_CMD_OK|0U?)[[:space:]]*;/)
                    print current
                opens = braces(line, "{")
                closes = braces(line, "}")
                if (opens != 0) opened = 1
                depth += opens - closes
                if (opened && depth <= 0) {
                    current = ""
                    opened = 0
                    depth = 0
                }
            }
        }
    ' "$@" | sort -u
}

check_pair()
{
    module=$1
    shim=$2
    shift 2
    declarations=$scratch/$module.declarations
    definitions=$scratch/$module.definitions
    missing=$scratch/$module.missing
    extra=$scratch/$module.extra

    extract_declarations "$@" >"$declarations"
    extract_definitions "$shim" >"$definitions"
    comm -23 "$declarations" "$definitions" >"$missing"
    comm -13 "$declarations" "$definitions" >"$extra"
    if [ -s "$missing" ]; then
        echo "module shims: $module declarations missing from ${shim#"$repo"/}:" >&2
        sed 's/^/  /' "$missing" >&2
        failed=1
    fi
    if [ -s "$extra" ]; then
        echo "module shims: $module definitions absent from its boundary headers:" >&2
        sed 's/^/  /' "$extra" >&2
        failed=1
    fi
}

check_pair lsp "$repo/src/mod/lsp/shim.c" \
    "$repo/src/mod/lsp/lsp.h"
check_pair ai "$repo/src/mod/ai/shim.c" \
    "$repo/src/mod/ai/ai.h"
check_pair fuss "$repo/src/mod/git/shim.c" \
    "$repo/src/mod/git/git.h" "$repo/src/mod/git/editor.h" \
    "$repo/src/mod/git/fussmode.h"
check_pair plugins "$repo/src/mod/plug/shim.c" \
    "$repo/src/mod/plug/plug.h" "$repo/src/mod/plug/pkg.h"

for shim in "$repo"/src/mod/*/shim.c; do
    successes=$scratch/$(basename "$(dirname "$shim")").success
    action_successes "$shim" >"$successes"
    if [ -s "$successes" ]; then
        echo "module shims: disabled action can report success in ${shim#"$repo"/}:" >&2
        sed 's/^/  /' "$successes" >&2
        failed=1
    fi
done

# Positive controls: a declaration without a definition and an action that
# reports success must both be named. This keeps the policy gate from going
# decorative as the shell/awk implementation evolves.
printf '%s\n' \
    'bool yew_seed_defined(void);' \
    'bool yew_seed_missing(void);' >"$scratch/seed.h"
printf '%s\n' \
    'bool yew_seed_defined(void)' \
    '{' \
    '    return false;' \
    '}' >"$scratch/seed.c"
extract_declarations "$scratch/seed.h" >"$scratch/seed.declarations"
extract_definitions "$scratch/seed.c" >"$scratch/seed.definitions"
comm -23 "$scratch/seed.declarations" "$scratch/seed.definitions" \
    >"$scratch/seed.missing"
if [ "$(cat "$scratch/seed.missing")" != yew_seed_missing ]; then
    echo 'module shims: parity checker no longer names its missing-definition seed' >&2
    failed=1
fi
printf '%s\n' \
    'CmdStatus yew_plug_cmd_seed(CmdCtx *cx)' \
    '{' \
    '    (void)cx;' \
    '    return YEW_CMD_OK;' \
    '}' >"$scratch/success.c"
action_successes "$scratch/success.c" >"$scratch/seed.success"
if [ "$(cat "$scratch/seed.success")" != yew_plug_cmd_seed ]; then
    echo 'module shims: honesty checker no longer names its success seed' >&2
    failed=1
fi

[ "$failed" -eq 0 ] || exit 1
echo 'module shims: ok'
