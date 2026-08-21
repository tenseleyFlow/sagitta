#!/bin/sh

set -eu
CDPATH=
export CDPATH

if ! command -v git >/dev/null 2>&1; then
    echo "HARNESS_SKIP git fixture: git not found"
    exit 0
fi

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] ||
   { [ "$#" -eq 2 ] && [ "$2" != "--print-hashes" ]; }; then
    echo "usage: $0 DEST [--print-hashes]" >&2
    exit 2
fi

dest=$1
print_hashes=false
if [ "$#" -eq 2 ]; then
    print_hashes=true
fi

if [ -e "$dest" ]; then
    echo "mkrepo.sh: destination already exists: $dest" >&2
    exit 2
fi

script_dir=$(cd "$(dirname "$0")" && pwd)
ledger=$script_dir/hashes.txt

# Do not let a developer's Git environment, templates, hooks, signing, or
# line-ending policy change the fixture.  Local config repeats the important
# settings so the repository describes its own contract after creation.
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE
unset GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_NAMESPACE
unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0
unset GIT_CONFIG GIT_CONFIG_PARAMETERS GIT_DEFAULT_HASH GIT_DEFAULT_REF_FORMAT
unset GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_AUTHOR_DATE
unset GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL GIT_COMMITTER_DATE
unset GIT_TEMPLATE_DIR
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_SYSTEM=/dev/null
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME='Yew Fixture'
export GIT_AUTHOR_EMAIL='fixture@yew.invalid'
export GIT_AUTHOR_DATE='2001-01-01T00:00:00 +0000'
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
export GIT_COMMITTER_DATE="$GIT_AUTHOR_DATE"
export LC_ALL=C
export TZ=UTC
umask 022

git_init()
{
    repo=$1
    mkdir -p "$repo"
    (
        cd "$repo"
        git -c init.defaultBranch=trunk \
            -c init.defaultObjectFormat=sha1 init -q
        git symbolic-ref HEAD refs/heads/trunk
        git config user.name "$GIT_AUTHOR_NAME"
        git config user.email "$GIT_AUTHOR_EMAIL"
        git config commit.gpgsign false
        git config tag.gpgsign false
        git config init.defaultBranch trunk
        git config core.autocrlf false
        git config core.filemode false
        git config core.quotepath false
        git config core.hooksPath .git/no-hooks
        git config status.renames true
    )
}

repeat_x()
{
    repeat_n=$1
    repeat_out=
    while [ "$repeat_n" -gt 0 ]; do
        repeat_out=${repeat_out}x
        repeat_n=$((repeat_n - 1))
    done
    printf '%s' "$repeat_out"
}

make_long_path()
{
    # Build component-by-component so the creating syscall never receives a
    # PATH_MAX-sized argument.  Some filesystems still reject the walk; that
    # optional edge is omitted there without changing any pinned Git object.
    (
        cd "$1" || exit 1
        mkdir long-path || exit 1
        cd long-path || exit 1
        long_rel=long-path
        long_component=dddddddddddddddddddddddddddddddddddddddddddddddddd
        long_component=${long_component}${long_component}${long_component}
        long_component=${long_component}dddddddddddddddddddddddddddddddddddddddddddddddddd
        while [ $((4095 - ${#long_rel} - 1)) -gt 255 ]; do
            mkdir "$long_component" || exit 1
            cd "$long_component" || exit 1
            long_rel=$long_rel/$long_component
        done
        long_leaf_n=$((4095 - ${#long_rel} - 1))
        long_leaf=$(repeat_x "$long_leaf_n")
        : > "$long_leaf" || exit 1
    ) 2>/dev/null
}

git_init "$dest"
dest_abs=$(cd "$dest" && pwd)

(
    cd "$dest_abs"

    cat >.gitignore <<'EOF'
/ignored.log
/ignored-dir/
/.fixture-variants/
EOF
    printf 'staged base\n' >staged.txt
    printf 'modified base\n' >modified.txt
    printf 'staged and modified base\n' >staged-and-modified.txt
    printf 'rename payload\n' >rename-old.txt
    printf 'copy payload\n' >copy-source.txt
    printf 'regular file before symlink\n' >typechange.txt
    printf 'common\nbase\n' >conflict.txt
    git add -- .
    git commit -q -m 'fixture base'
    git tag base

    git checkout -q -b conflict-theirs
    printf 'common\ntheirs\n' >conflict.txt
    git add -- conflict.txt
    git commit -q -m 'conflict theirs'

    git checkout -q trunk
    printf 'common\nours\n' >conflict.txt
    git add -- conflict.txt
    git commit -q -m 'conflict ours'
    if git merge --no-edit conflict-theirs >/dev/null 2>&1; then
        echo "mkrepo.sh: expected merge conflict was not produced" >&2
        exit 1
    fi

    printf 'staged index\n' >staged.txt
    git add -- staged.txt
    printf 'modified worktree\n' >modified.txt
    printf 'staged and modified index\n' >staged-and-modified.txt
    git add -- staged-and-modified.txt
    printf 'staged and modified worktree\n' >staged-and-modified.txt
    git mv -- rename-old.txt rename-new.txt
    cp copy-source.txt copy-candidate.txt
    git add -- copy-candidate.txt
    rm typechange.txt
    ln -s copy-source.txt typechange.txt
    git add -- typechange.txt

    printf 'untracked\n' >untracked.txt
    mkdir untracked-dir
    printf 'nested untracked\n' >untracked-dir/entry.txt
    printf 'ignored\n' >ignored.log
    mkdir ignored-dir
    printf 'nested ignored\n' >ignored-dir/entry.txt

    printf 'space\n' >'space name.txt'
    awkward_quote="quote\"\$dollar.txt"
    printf 'quote and dollar\n' >"$awkward_quote"
    awkward_newline='line
break.txt'
    printf 'newline\n' >"$awkward_newline"
    printf 'dash n\n' >./-n
    printf 'cjk\n' >'漢字.txt'
)

# A 4095-byte repository-relative path is intentionally untracked so hosts
# that cannot represent it retain the same pinned object hashes.
if ! make_long_path "$dest_abs"; then
    rm -rf "$dest_abs/long-path"
fi

variants=$dest_abs/.fixture-variants
git_init "$variants/detached"
(
    cd "$variants/detached"
    printf 'detached fixture\n' >detached.txt
    git add -- detached.txt
    git commit -q -m 'detached fixture'
    git checkout -q --detach HEAD
)

git_init "$variants/unborn"

git -c init.defaultBranch=trunk -c init.defaultObjectFormat=sha1 \
    init -q --bare "$variants/upstream-remote.git"
git --git-dir="$variants/upstream-remote.git" symbolic-ref HEAD refs/heads/trunk
git_init "$variants/upstream"
(
    cd "$variants/upstream"
    printf 'upstream base\n' >shared.txt
    git add -- shared.txt
    git commit -q -m 'upstream base'
    git remote add origin ../upstream-remote.git
    git push -q -u origin trunk
)

git_init "$variants/upstream-seed"
(
    cd "$variants/upstream-seed"
    git remote add origin ../upstream-remote.git
    git fetch -q origin trunk
    git checkout -q -b trunk --track origin/trunk
    printf 'remote advance\n' >remote.txt
    git add -- remote.txt
    git commit -q -m 'remote advance'
    git push -q origin trunk
)
(
    cd "$variants/upstream"
    printf 'local advance\n' >local.txt
    git add -- local.txt
    git commit -q -m 'local advance'
    git fetch -q origin
)

hash_value()
{
    hash_repo=$1
    hash_name=$2
    git -C "$dest_abs/$hash_repo" rev-parse --verify "$hash_name"
}

emit_hashes()
{
    echo '# repo revision-or-index-spec sha1'
    emit_hash_row . refs/tags/base
    emit_hash_row . refs/heads/trunk
    emit_hash_row . refs/heads/conflict-theirs
    emit_hash_row . :staged.txt
    emit_hash_row . :staged-and-modified.txt
    emit_hash_row . :copy-candidate.txt
    emit_hash_row . :typechange.txt
    emit_hash_row . :1:conflict.txt
    emit_hash_row . :2:conflict.txt
    emit_hash_row . :3:conflict.txt
    emit_hash_row .fixture-variants/detached HEAD
    emit_hash_row .fixture-variants/upstream HEAD
    emit_hash_row .fixture-variants/upstream refs/remotes/origin/trunk
}

emit_hash_row()
{
    printf '%s %s %s\n' "$1" "$2" "$(hash_value "$1" "$2")"
}

if $print_hashes; then
    emit_hashes
    exit 0
fi

if [ ! -r "$ledger" ]; then
    echo "mkrepo.sh: missing pinned hash ledger: $ledger" >&2
    exit 1
fi

while read -r hash_repo hash_name hash_expected hash_extra; do
    case $hash_repo in
        ''|'#'*) continue ;;
    esac
    if [ -n "${hash_extra:-}" ]; then
        echo "mkrepo.sh: malformed hash ledger row: $hash_repo $hash_name" >&2
        exit 1
    fi
    hash_actual=$(hash_value "$hash_repo" "$hash_name")
    if [ "$hash_actual" != "$hash_expected" ]; then
        echo "mkrepo.sh: hash mismatch for $hash_repo $hash_name" >&2
        echo "  expected $hash_expected" >&2
        echo "  actual   $hash_actual" >&2
        exit 1
    fi
done < "$ledger"
