#!/bin/sh
set -eu

# Sprint 47's dedicated live-clangd lane.  All edits happen in a detached
# worktree, never in the checkout that invoked the test.
driver=${1:-build/tests/lsp/test_clangd_live}
required=${YEW_LSP_CI_REQUIRED:-0}

if ! clangd_path=$(command -v clangd); then
    if [ "$required" = 1 ]; then
        echo "FAIL: clangd not installed" >&2
        exit 1
    fi
    echo "SKIP: clangd not installed" >&2
    exit 0
fi

case "$driver" in
    /*) ;;
    *) driver=$(pwd)/$driver ;;
esac

scratch=$(mktemp -d "${TMPDIR:-/tmp}/yew-clangd-ci.XXXXXX")
worktree=$scratch/worktree
state=$scratch/state
compdb_first=$scratch/compile_commands.first.json
cleanup()
{
    git worktree remove --force "$worktree" >/dev/null 2>&1 || true
    rm -rf "$scratch"
}
trap cleanup EXIT HUP INT TERM

git worktree add --detach "$worktree" HEAD >/dev/null
mkdir -p "$state"
(
    cd "$worktree"
    make compile_commands.json
    cp compile_commands.json "$compdb_first"
    make compile_commands.json
    if ! cmp -s "$compdb_first" compile_commands.json; then
        echo "FAIL: compile_commands.json is not reproducible" >&2
        exit 1
    fi
)

XDG_STATE_HOME=$state YEW_CLANGD=$clangd_path "$driver" "$worktree"
git -C "$worktree" diff --exit-code
