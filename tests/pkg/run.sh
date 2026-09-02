#!/usr/bin/env bash

set -euo pipefail

LC_ALL=C
TZ=UTC
export LC_ALL TZ

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
yew_arg=${1:-build/yew}
case "$yew_arg" in
    /*) yew_bin=$yew_arg ;;
    *) yew_bin=$repo_root/$yew_arg ;;
esac

if [[ ! -x "$yew_bin" ]]; then
    printf 'FAIL: yew binary is not executable: %s\n' "$yew_bin" >&2
    exit 1
fi
if ! command -v git >/dev/null 2>&1; then
    printf 'SKIP: git is required to construct the local pkg fixtures\n'
    exit 0
fi
real_git=$(command -v git)
host_os=$(uname -s)
host_arch=$(uname -m)
preload_faults=true
if command -v file >/dev/null 2>&1 &&
   file -- "$yew_bin" | grep -F 'static-pie linked' >/dev/null; then
    preload_faults=false
fi

build_fault_shim()
{
    local output=$1
    local -a arch_flags=()

    if [[ $host_os == Darwin ]]; then
        # Current macOS uses an arm64e-only /bin/sh even when yew itself is
        # arm64.  dyld loads an interposer before the Git wrapper can unset
        # DYLD_INSERT_LIBRARIES, so the shim must satisfy both native slices.
        if [[ $host_arch == arm64 || $host_arch == arm64e ]]; then
            arch_flags=(-arch arm64 -arch arm64e)
        fi
        ${CC:-cc} -std=c11 -fPIC -dynamiclib "${arch_flags[@]}" -o "$output" \
            "$repo_root/tests/torture/faultshim.c" \
            "$repo_root/tests/torture/faultshim-darwin.s"
    else
        ${CC:-cc} -std=c11 -fPIC -shared -o "$output" \
            "$repo_root/tests/torture/faultshim.c" -ldl
    fi
}

run_with_fault_shim()
{
    local shim=$1
    shift

    if [[ $host_os == Darwin ]]; then
        DYLD_INSERT_LIBRARIES="$shim" DYLD_FORCE_FLAT_NAMESPACE=1 "$@"
    else
        LD_PRELOAD="$shim" "$@"
    fi
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/yew-pkg.XXXXXX")
scratch=$(cd "$scratch" && pwd -P)
cleanup()
{
    rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_SYSTEM=/dev/null
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME='Yew Pkg Fixture'
export GIT_AUTHOR_EMAIL='pkg-fixture@yew.invalid'
export GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME
export GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
export GIT_AUTHOR_DATE='2002-02-02T02:02:02 +0000'
export GIT_COMMITTER_DATE=$GIT_AUTHOR_DATE
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR

passed=0
case_root=
source_repo=
plugin_name=
stdout_file=
stderr_file=
pkg_status=0

fail()
{
    printf 'FAIL [%s]: %s\n' "${current_case:-setup}" "$*" >&2
    if [[ -f "${stdout_file:-}" ]]; then
        sed 's/^/  stdout: /' "$stdout_file" >&2
    fi
    if [[ -f "${stderr_file:-}" ]]; then
        sed 's/^/  stderr: /' "$stderr_file" >&2
    fi
    exit 1
}

sed_rewrite()
{
    local expression=$1
    local path=$2
    local replacement=$path.sed.$$

    sed "$expression" "$path" >"$replacement"
    mv -- "$replacement" "$path"
}

begin_case()
{
    current_case=$1
    case_root=$scratch/$current_case
    mkdir -p "$case_root/xdg/data" "$case_root/xdg/state" \
             "$case_root/xdg/config" "$case_root/xdg/cache"
    export XDG_DATA_HOME=$case_root/xdg/data
    export XDG_STATE_HOME=$case_root/xdg/state
    export XDG_CONFIG_HOME=$case_root/xdg/config
    export XDG_CACHE_HOME=$case_root/xdg/cache
    stdout_file=$case_root/stdout
    stderr_file=$case_root/stderr
    printf 'CASE %s\n' "$current_case"
}

pass_case()
{
    passed=$((passed + 1))
    printf 'PASS %s\n' "$current_case"
}

run_pkg()
{
    : >"$stdout_file"
    : >"$stderr_file"
    set +e
    "$yew_bin" pkg "$@" >"$stdout_file" 2>"$stderr_file"
    pkg_status=$?
    set -e
}

run_pkg_without_git()
{
    : >"$stdout_file"
    : >"$stderr_file"
    mkdir -p "$case_root/no-git"
    set +e
    PATH="$case_root/no-git" "$yew_bin" pkg "$@" \
        >"$stdout_file" 2>"$stderr_file"
    pkg_status=$?
    set -e
}

assert_status()
{
    [[ "$pkg_status" -eq "$1" ]] ||
        fail "expected exit $1, got $pkg_status"
}

assert_not_status()
{
    [[ "$pkg_status" -ne "$1" ]] ||
        fail "did not expect exit $1"
}

assert_output_contains()
{
    if ! grep -F -- "$1" "$stdout_file" "$stderr_file" >/dev/null; then
        fail "output does not contain: $1"
    fi
}

assert_output_lacks()
{
    if grep -F -- "$1" "$stdout_file" "$stderr_file" >/dev/null; then
        fail "output unexpectedly contains: $1"
    fi
}

assert_stdout_exact()
{
    cmp -s "$stdout_file" <(printf '%s' "$1") ||
        fail 'stdout did not match the exact contract'
}

assert_stderr_exact()
{
    cmp -s "$stderr_file" <(printf '%s' "$1") ||
        fail 'stderr did not match the exact contract'
}

assert_file_contains()
{
    [[ -f "$1" ]] || fail "missing file: $1"
    grep -F -- "$2" "$1" >/dev/null || fail "$1 does not contain: $2"
}

assert_file_lacks()
{
    if [[ -f "$1" ]] && grep -F -- "$2" "$1" >/dev/null; then
        fail "$1 unexpectedly contains: $2"
    fi
}

lock_path()
{
    printf '%s/yew/plugins.lock' "$XDG_DATA_HOME"
}

plugin_dir()
{
    printf '%s/yew/plugins/%s' "$XDG_DATA_HOME" "$plugin_name"
}

assert_no_staging()
{
    if compgen -G "$XDG_DATA_HOME/yew/plugins/.pkg-tmp-*" >/dev/null; then
        fail "$1"
    fi
}

write_manifest()
{
    local repo=$1
    local name=$2
    cat >"$repo/plugin.fl" <<EOF
{
  name: "$name",
  version: "1.0.0",
  api: 1,
  entry: "src/main.fl",
  capabilities: [],
  events: [],
  description: "package integration fixture",
}
EOF
}

make_source()
{
    plugin_name=${1:-fixture-plugin}
    source_repo=$case_root/source-$plugin_name
    mkdir -p "$source_repo/src"
    git -c init.defaultBranch=trunk -c init.defaultObjectFormat=sha1 \
        init -q "$source_repo"
    git -C "$source_repo" config user.name "$GIT_AUTHOR_NAME"
    git -C "$source_repo" config user.email "$GIT_AUTHOR_EMAIL"
    git -C "$source_repo" config commit.gpgsign false
    git -C "$source_repo" config tag.gpgsign false
    git -C "$source_repo" config core.autocrlf false
    git -C "$source_repo" config core.hooksPath .git/no-hooks
    write_manifest "$source_repo" "$plugin_name"
    printf 'fn init(ctx) { ctx.msg("fixture loaded") }\n' \
        >"$source_repo/src/main.fl"
    git -C "$source_repo" add -- plugin.fl src/main.fl
    git -C "$source_repo" commit -q -m 'fixture base'
}

source_url()
{
    printf 'file://%s' "$source_repo"
}

advance_source()
{
    local message=${1:-fixture-advance}
    printf '# %s\n' "$message" >>"$source_repo/src/main.fl"
    git -C "$source_repo" add -- src/main.fl
    git -C "$source_repo" commit -q -m "$message"
}

install_head()
{
    run_pkg install "$source_repo"
    assert_status 0
    [[ -d "$(plugin_dir)" ]] || fail 'install did not create plugin directory'
    assert_file_contains "$(lock_path)" "\"$plugin_name\""
}

test_install_head()
{
    begin_case install_head
    make_source head-plugin
    install_head
    assert_file_contains "$(lock_path)" 'pin: "head"'
    pass_case
}

make_shipped_example_source()
{
    plugin_name=$1
    source_repo=$case_root/source-$plugin_name
    mkdir -p "$source_repo"
    cp -R "$repo_root/examples/plugins/$plugin_name/." "$source_repo/"
    git -c init.defaultBranch=trunk -c init.defaultObjectFormat=sha1 \
        init -q "$source_repo"
    git -C "$source_repo" config user.name "$GIT_AUTHOR_NAME"
    git -C "$source_repo" config user.email "$GIT_AUTHOR_EMAIL"
    git -C "$source_repo" config commit.gpgsign false
    git -C "$source_repo" config tag.gpgsign false
    git -C "$source_repo" config core.autocrlf false
    git -C "$source_repo" config core.hooksPath .git/no-hooks
    git -C "$source_repo" add -- .
    git -C "$source_repo" commit -q -m "ship $plugin_name example"
}

test_shipped_examples_install_as_local_repositories()
{
    begin_case shipped_examples_install_as_local_repositories
    for example in trailing-ws session-notes; do
        make_shipped_example_source "$example"
        run_pkg install "$source_repo"
        assert_status 0
        [[ -d "$(plugin_dir)" ]] ||
            fail "$example did not install into the managed plugin root"
        assert_file_contains "$(plugin_dir)/plugin.fl" "name: \"$example\""
        assert_file_contains "$(lock_path)" "\"$example\""
        assert_no_staging "$example install left a staging directory"
    done
    run_pkg list
    assert_status 0
    assert_output_contains $'session-notes\thead\t'
    assert_output_contains $'trailing-ws\thead\t'
    pass_case
}

test_install_tag()
{
    begin_case install_tag
    make_source tag-plugin
    git -C "$source_repo" tag v1.0.0
    run_pkg install "$(source_url)" --tag v1.0.0
    assert_status 0
    assert_file_contains "$(lock_path)" 'pin: "tag:v1.0.0"'
    pass_case
}

test_historical_pin_selects_historical_manifest()
{
    begin_case historical_pin_selects_historical_manifest
    make_source historical-plugin
    git -C "$source_repo" tag old-name
    sed_rewrite 's/name: "historical-plugin"/name: "newer-plugin"/' \
        "$source_repo/plugin.fl"
    git -C "$source_repo" add -- plugin.fl
    git -C "$source_repo" commit -q -m 'rename at head'
    plugin_name=historical-plugin
    run_pkg install "$(source_url)" --tag old-name
    assert_status 0
    [[ -d "$(plugin_dir)" ]] || fail 'historical manifest name was ignored'
    [[ ! -e "$XDG_DATA_HOME/yew/plugins/newer-plugin" ]] ||
        fail 'default HEAD manifest selected before historical checkout'
    pass_case
}

test_install_rev()
{
    local rev
    begin_case install_rev
    make_source rev-plugin
    rev=$(git -C "$source_repo" rev-parse HEAD)
    advance_source newer
    run_pkg install "$(source_url)" --rev "$rev"
    assert_status 0
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$rev" ]] ||
        fail 'revision install did not check out the requested commit'
    assert_file_contains "$(lock_path)" "pin: \"rev:$rev\""
    pass_case
}

test_install_branch()
{
    begin_case install_branch
    make_source branch-plugin
    git -C "$source_repo" checkout -q -b stable
    advance_source stable-change
    run_pkg install "$(source_url)" --branch stable
    assert_status 0
    assert_file_contains "$(lock_path)" 'pin: "branch:stable"'
    pass_case
}

test_list_porcelain()
{
    begin_case list_porcelain
    make_source list-plugin
    install_head
    run_pkg list
    assert_status 0
    assert_output_contains $'list-plugin\thead\t'
    assert_output_contains $'\tok\t'
    pass_case
}

test_doctor_clean()
{
    begin_case doctor_clean
    make_source doctor-plugin
    install_head
    run_pkg doctor doctor-plugin
    assert_status 0
    assert_output_contains doctor-plugin
    assert_output_contains ok
    pass_case
}

test_update_noop()
{
    local before
    begin_case update_noop
    make_source noop-plugin
    install_head
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    run_pkg update noop-plugin
    assert_status 0
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
        fail 'no-op update changed HEAD'
    assert_output_contains 'up to date'
    pass_case
}

test_update_fast_forward()
{
    local target
    begin_case update_fast_forward
    make_source ff-plugin
    install_head
    advance_source forward
    target=$(git -C "$source_repo" rev-parse HEAD)
    run_pkg update ff-plugin
    assert_status 0
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$target" ]] ||
        fail 'fast-forward update did not reach target'
    assert_file_contains "$(plugin_dir)/src/main.fl" '# forward'
    pass_case
}

test_update_dry_run()
{
    local before lock_before
    begin_case update_dry_run
    make_source dry-plugin
    install_head
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    lock_before=$case_root/lock.before
    cp "$(lock_path)" "$lock_before"
    advance_source dry-target
    run_pkg update dry-plugin --dry-run
    assert_status 0
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
        fail 'dry-run changed HEAD'
    cmp -s "$lock_before" "$(lock_path)" || fail 'dry-run changed lockfile'
    assert_output_contains dry-target
    pass_case
}

test_update_changelog_reports_remainder()
{
    local n
    begin_case update_changelog_reports_remainder
    make_source capped-plugin
    install_head
    for n in $(seq 1 52); do
        advance_source "change-$n"
    done
    run_pkg update capped-plugin --dry-run
    assert_status 0
    assert_output_contains '... and 2 more'
    pass_case
}

test_update_changelog_failure_aborts_unchanged()
{
    local before lock_before op old_path
    begin_case update_changelog_failure_aborts_unchanged
    make_source changelog-failure-plugin
    install_head
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    lock_before=$case_root/lock.before
    cp "$(lock_path)" "$lock_before"
    advance_source reporting-failure-target
    mkdir -p "$case_root/bin"
    cat >"$case_root/bin/git" <<'EOF'
#!/usr/bin/env bash
for arg in "$@"; do
    if [[ "$arg" == "${YEW_TEST_FAIL_GIT_OP:-}" ]]; then
        printf '\033]8;;https://invalid.example\auntrusted git failure\033]8;;\a\n' >&2
        exit 9
    fi
done
exec "$YEW_TEST_REAL_GIT" "$@"
EOF
    chmod +x "$case_root/bin/git"
    old_path=$PATH
    export YEW_TEST_REAL_GIT=$real_git
    for op in log rev-list; do
        export YEW_TEST_FAIL_GIT_OP=$op
        PATH="$case_root/bin:$old_path"
        run_pkg update changelog-failure-plugin
        PATH=$old_path
        assert_status 3
        [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
            fail "$op failure changed HEAD"
        cmp -s "$lock_before" "$(lock_path)" ||
            fail "$op failure changed lockfile"
        assert_output_contains '\x1b]8;;https://invalid.example'
        if LC_ALL=C grep $'\033' "$stdout_file" "$stderr_file" >/dev/null; then
            fail "$op failure emitted a raw escape byte"
        fi
    done
    unset YEW_TEST_FAIL_GIT_OP YEW_TEST_REAL_GIT
    pass_case
}

test_update_force_push_refused()
{
    local before lock_before
    begin_case update_force_push_refused
    make_source force-plugin
    install_head
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    lock_before=$case_root/lock.before
    cp "$(lock_path)" "$lock_before"
    git -C "$source_repo" checkout -q --orphan rewritten
    git -C "$source_repo" rm -q -rf -- .
    mkdir -p "$source_repo/src"
    write_manifest "$source_repo" "$plugin_name"
    printf 'fn init(ctx) { ctx.msg("rewritten") }\n' >"$source_repo/src/main.fl"
    git -C "$source_repo" add -- plugin.fl src/main.fl
    git -C "$source_repo" commit -q -m rewritten
    git -C "$source_repo" branch -M trunk
    run_pkg update force-plugin
    assert_status 1
    assert_output_contains 'is not an ancestor'
    assert_output_contains 'force-pushed or history was rewritten'
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
        fail 'force-push refusal changed HEAD'
    cmp -s "$lock_before" "$(lock_path)" ||
        fail 'force-push refusal changed lockfile'
    pass_case
}

test_update_moved_tag_refused()
{
    local before lock_before
    begin_case update_moved_tag_refused
    make_source moved-tag-plugin
    git -C "$source_repo" tag release
    run_pkg install "$(source_url)" --tag release
    assert_status 0
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    lock_before=$case_root/lock.before
    cp "$(lock_path)" "$lock_before"
    advance_source moved-release
    git -C "$source_repo" tag -f release >/dev/null
    run_pkg update moved-tag-plugin
    assert_status 1
    assert_output_contains 'history was rewritten'
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
        fail 'moved-tag refusal changed HEAD'
    cmp -s "$lock_before" "$(lock_path)" ||
        fail 'moved-tag refusal changed lockfile'
    pass_case
}

test_update_dirty_refused()
{
    local before
    begin_case update_dirty_refused
    make_source dirty-plugin
    install_head
    before=$(git -C "$(plugin_dir)" rev-parse HEAD)
    advance_source clean-target
    printf '# local edit\n' >>"$(plugin_dir)/src/main.fl"
    run_pkg update dirty-plugin
    assert_status 1
    assert_output_contains dirty
    assert_output_contains --discard-local
    [[ "$(git -C "$(plugin_dir)" rev-parse HEAD)" == "$before" ]] ||
        fail 'dirty refusal changed HEAD'
    assert_file_contains "$(plugin_dir)/src/main.fl" '# local edit'
    pass_case
}

test_update_untracked_is_not_deleted()
{
    begin_case update_untracked_refused_and_discarded
    make_source untracked-plugin
    install_head
    advance_source untracked-target
    printf 'local\n' >"$(plugin_dir)/local.txt"
    run_pkg update untracked-plugin
    assert_status 0
    [[ -f "$(plugin_dir)/local.txt" ]] || fail 'update deleted untracked file'
    pass_case
}

test_doctor_accepts_drift()
{
    local before after
    begin_case doctor_accepts_drift
    make_source accept-plugin
    install_head
    before=$(lock_path)
    cp "$before" "$case_root/lock.before"
    printf '# accepted local edit\n' >>"$(plugin_dir)/src/main.fl"
    run_pkg doctor accept-plugin --accept
    assert_status 0
    after=$(lock_path)
    cmp -s "$case_root/lock.before" "$after" &&
        fail 'doctor --accept did not record the new tree'
    run_pkg doctor accept-plugin
    assert_status 0
    assert_output_contains ok
    pass_case
}

test_doctor_fixes_drift()
{
    begin_case doctor_fixes_drift
    make_source fix-plugin
    install_head
    printf '# unwanted local edit\n' >>"$(plugin_dir)/src/main.fl"
    run_pkg doctor fix-plugin --fix
    assert_status 0
    assert_file_lacks "$(plugin_dir)/src/main.fl" '# unwanted local edit'
    run_pkg doctor fix-plugin
    assert_status 0
    assert_output_contains ok
    pass_case
}

test_doctor_fix_preserves_added_paths()
{
    begin_case doctor_fix_removes_added_paths
    make_source fix-added-plugin
    install_head
    printf 'untracked\n' >"$(plugin_dir)/added.txt"
    run_pkg doctor fix-added-plugin --fix
    assert_status 1
    [[ -e "$(plugin_dir)/added.txt" ]] || fail 'doctor --fix deleted added path'
    pass_case
}

test_doctor_accepts_tree_and_head_together()
{
    local head
    begin_case doctor_accepts_tree_and_head_together
    make_source accept-both-plugin
    install_head
    printf '# committed local\n' >>"$(plugin_dir)/src/main.fl"
    git -C "$(plugin_dir)" add -- src/main.fl
    git -C "$(plugin_dir)" commit -q -m 'local commit'
    printf '# uncommitted too\n' >>"$(plugin_dir)/src/main.fl"
    head=$(git -C "$(plugin_dir)" rev-parse HEAD)
    run_pkg doctor accept-both-plugin --accept
    assert_status 0
    assert_file_contains "$(lock_path)" "rev: \"$head\""
    run_pkg doctor accept-both-plugin
    assert_status 0
    assert_output_contains ok
    pass_case
}

test_doctor_reports_paths_and_capabilities()
{
    begin_case doctor_reports_paths_and_capabilities
    make_source report-plugin
    printf 'ignored.txt\n' >"$source_repo/.gitignore"
    git -C "$source_repo" add -- .gitignore
    git -C "$source_repo" commit -q -m 'ignore fixture path'
    install_head
    printf '# changed\n' >>"$(plugin_dir)/src/main.fl"
    printf 'new\n' >"$(plugin_dir)/added.txt"
    printf 'ignored but added\n' >"$(plugin_dir)/ignored.txt"
    run_pkg doctor report-plugin
    assert_status 1
    assert_output_contains $'report-plugin\tpath\tchanged\tsrc/main.fl'
    assert_output_contains $'report-plugin\tpath\tadded\tadded.txt'
    assert_output_contains $'report-plugin\tpath\tadded\tignored.txt'
    assert_output_contains $'report-plugin\tcapability\tfs\tundeclared\tunset'
    pass_case
}

test_remove_managed_plugin()
{
    begin_case remove_managed_plugin
    make_source remove-plugin
    install_head
    run_pkg remove remove-plugin
    assert_status 0
    [[ ! -e "$(plugin_dir)" ]] || fail 'remove left the plugin directory'
    assert_file_lacks "$(lock_path)" '"remove-plugin"'
    pass_case
}

test_remove_trust_failure_rolls_back()
{
    local trust saved

    begin_case remove_trust_failure_rolls_back
    make_source rollback-plugin
    install_head
    trust=$XDG_STATE_HOME/yew/trust.fl
    saved=$case_root/trust.fl.saved
    mv "$trust" "$saved"
    mkdir "$trust"
    run_pkg remove rollback-plugin
    assert_status 3
    [[ -d "$trust" ]] || fail 'failed trust save replaced its directory sentinel'
    rmdir "$trust"
    mv "$saved" "$trust"
    [[ -d "$(plugin_dir)" ]] || fail 'failed remove did not restore plugin'
    assert_file_contains "$(lock_path)" '"rollback-plugin"'
    if find "$XDG_DATA_HOME/yew" -maxdepth 1 -name '.pkg-trash-*' | grep . >/dev/null; then
        fail 'failed remove stranded plugin quarantine'
    fi
    run_pkg doctor rollback-plugin
    assert_status 0
    assert_output_contains ok
    pass_case
}

test_invalid_bare_spec()
{
    begin_case invalid_bare_spec
    run_pkg install user/repository
    assert_status 1
    assert_output_contains 'gh:user/repo'
    assert_output_contains 'bare'
    pass_case
}

test_cli_diagnostic_sanitizes_terminal_controls()
{
    local hostile=$'https://host/repo\tline\nbyte\033c1\233del\177'

    begin_case cli_diagnostic_sanitizes_terminal_controls
    run_pkg install "$hostile"
    assert_status 1
    assert_output_contains '\x09'
    assert_output_contains '\x0a'
    assert_output_contains '\x1b'
    assert_output_contains '\x9b'
    assert_output_contains '\x7f'
    if LC_ALL=C grep -q $'\t' "$stdout_file" "$stderr_file"; then
        fail 'CLI diagnostic emitted a raw tab byte'
    fi
    if LC_ALL=C grep -q $'\033' "$stdout_file" "$stderr_file" ||
       LC_ALL=C grep -q $'\233' "$stdout_file" "$stderr_file" ||
       LC_ALL=C grep -q $'\177' "$stdout_file" "$stderr_file"; then
        fail 'CLI diagnostic emitted a raw terminal-control byte'
    fi
    pass_case
}

test_invalid_ref_rejected()
{
    begin_case invalid_ref_rejected
    make_source invalid-ref-plugin
    run_pkg install "$(source_url)" --branch '-upload-pack=bad'
    assert_status 1
    assert_output_contains 'invalid'
    [[ ! -e "$(plugin_dir)" ]] || fail 'invalid ref installed a plugin'
    pass_case
}

test_malicious_lock_name_cannot_escape_plugins()
{
    local victim lock
    begin_case malicious_lock_name_cannot_escape_plugins
    make_source safe-plugin
    install_head
    victim=$XDG_DATA_HOME/yew/victim
    mkdir -p "$victim"
    printf 'keep\n' >"$victim/sentinel"
    lock=$(lock_path)
    sed_rewrite 's/"safe-plugin"/"..\/victim"/' "$lock"
    run_pkg remove '../victim'
    assert_status 1
    [[ -f "$victim/sentinel" ]] || fail 'remove escaped plugin root'
    run_pkg doctor
    assert_status 1
    [[ -f "$victim/sentinel" ]] || fail 'doctor escaped plugin root'
    run_pkg update
    assert_status 1
    [[ -f "$victim/sentinel" ]] || fail 'update escaped plugin root'
    pass_case
}

test_pin_options_are_mutually_exclusive()
{
    begin_case pin_options_are_mutually_exclusive
    make_source exclusive-plugin
    run_pkg install "$(source_url)" --tag v1 --branch trunk
    assert_status 1
    assert_output_contains 'choose exactly one of --rev, --tag, or --branch'
    [[ ! -e "$(plugin_dir)" ]] || fail 'conflicting pin options installed plugin'
    pass_case
}

test_dash_prefixed_name_uses_option_terminator()
{
    begin_case dash_prefixed_name_uses_option_terminator
    make_source --dash-plugin
    install_head
    run_pkg list -- --dash-plugin
    assert_status 0
    assert_output_contains --dash-plugin
    run_pkg doctor -- --dash-plugin
    assert_status 0
    advance_source dash-update
    run_pkg update -- --dash-plugin
    assert_status 0
    assert_file_contains "$(plugin_dir)/src/main.fl" '# dash-update'
    pass_case
}

test_corrupt_lock_preserved()
{
    local corrupt
    begin_case corrupt_lock_preserved
    make_source corrupt-plugin
    mkdir -p "$(dirname "$(lock_path)")"
    corrupt='{schema: 1, plugins: { "broken": {'
    printf '%s' "$corrupt" >"$(lock_path)"
    run_pkg install "$(source_url)"
    assert_status 1
    [[ "$(cat "$(lock_path)")" == "$corrupt" ]] ||
        fail 'install overwrote a corrupt lockfile'
    assert_output_contains lock
    pass_case
}

test_force_relock_discards_corrupt_lock()
{
    begin_case force_relock_discards_corrupt_lock
    make_source relock-plugin
    mkdir -p "$(dirname "$(lock_path)")"
    printf '{schema: 1, plugins: { "broken": {' >"$(lock_path)"
    run_pkg install "$(source_url)" --force-relock
    assert_status 0
    assert_file_contains "$(lock_path)" '"relock-plugin"'
    assert_file_lacks "$(lock_path)" '"broken"'
    assert_output_contains '--force-relock discards'
    pass_case
}

test_timeout_argument_validation()
{
    begin_case timeout_argument_validation
    make_source timeout-plugin
    run_pkg install "$(source_url)" --timeout 0
    assert_status 1
    assert_output_contains '--timeout requires positive seconds'
    [[ ! -e "$(plugin_dir)" ]] || fail 'invalid timeout installed a plugin'
    run_pkg install "$(source_url)" --timeout 5
    assert_status 0
    pass_case
}

test_network_timeout_kills_stalled_git()
{
    local old_path
    begin_case network_timeout_kills_stalled_git
    mkdir -p "$case_root/bin"
    cat >"$case_root/bin/git" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "--version" ]]; then
    exec "$YEW_TEST_REAL_GIT" "$@"
fi
sleep 30
EOF
    chmod +x "$case_root/bin/git"
    old_path=$PATH
    export YEW_TEST_REAL_GIT=$real_git
    PATH="$case_root/bin:$old_path"
    run_pkg install https://127.0.0.1.invalid/black-hole.git --timeout 1
    PATH=$old_path
    unset YEW_TEST_REAL_GIT
    assert_status 3
    assert_stdout_exact ''
    assert_stderr_exact $'yew pkg: error: git clone timed out after 1s (--timeout SECONDS)\n'
    assert_no_staging 'timed-out clone left a staging directory'
    pass_case
}

test_unreachable_remote_exits_three_without_timeout()
{
    local old_path
    begin_case unreachable_remote_exits_three_without_timeout
    mkdir -p "$case_root/bin"
    cat >"$case_root/bin/git" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "--version" ]]; then
    exec "$YEW_TEST_REAL_GIT" "$@"
fi
printf 'fixture network unavailable\n' >&2
exit 128
EOF
    chmod +x "$case_root/bin/git"
    old_path=$PATH
    export YEW_TEST_REAL_GIT=$real_git
    PATH="$case_root/bin:$old_path"
    run_pkg install https://invalid.example/unreachable.git --timeout 5
    PATH=$old_path
    unset YEW_TEST_REAL_GIT
    assert_status 3
    assert_stdout_exact ''
    assert_stderr_exact $'yew pkg: error: git clone failed (exit 128)\ngit said:\n  fixture network unavailable\n'
    pass_case
}

test_install_executes_no_repository_code()
{
    local sentinel
    begin_case install_executes_no_repository_code
    make_source inert-plugin
    sentinel=$case_root/repository-code-ran
    cat >"$source_repo/Makefile" <<EOF
all:
\t@touch '$sentinel'
EOF
    cat >"$source_repo/install.sh" <<EOF
#!/bin/sh
touch '$sentinel'
EOF
    chmod +x "$source_repo/install.sh"
    git -C "$source_repo" add -- Makefile install.sh
    git -C "$source_repo" commit -q -m 'inert install-shaped files'
    run_pkg install "$(source_url)"
    assert_status 0
    [[ ! -e "$sentinel" ]] || fail 'install executed repository code'
    pass_case
}

test_install_warns_once_for_uninitialized_submodules()
{
    begin_case install_warns_once_for_uninitialized_submodules
    make_source submodule-plugin
    printf '[submodule "nested"]\n\tpath = nested\n\turl = https://invalid.example/nested.git\n' \
        >"$source_repo/.gitmodules"
    git -C "$source_repo" add -- .gitmodules
    git -C "$source_repo" commit -q -m 'declare inert submodule'
    run_pkg install "$(source_url)"
    assert_status 0
    assert_output_contains 'repository contains .gitmodules; submodules are not initialized'
    [[ "$(grep -Fc 'repository contains .gitmodules' "$stderr_file")" -eq 1 ]] ||
        fail '.gitmodules warning was not emitted exactly once'
    pass_case
}

test_postinstall_key_is_rejected_without_execution()
{
    local sentinel
    begin_case postinstall_key_is_rejected_without_execution
    make_source postinstall-plugin
    sentinel=$case_root/postinstall-ran
    cat >"$source_repo/install.sh" <<EOF
#!/bin/sh
touch '$sentinel'
EOF
    chmod +x "$source_repo/install.sh"
    sed_rewrite '/^}/i\
  postinstall: "install.sh",
' \
        "$source_repo/plugin.fl"
    git -C "$source_repo" add -- install.sh plugin.fl
    git -C "$source_repo" commit -q -m 'rejected postinstall key'
    run_pkg install "$(source_url)"
    assert_status 1
    assert_output_contains 'unknown manifest key: postinstall'
    [[ ! -e "$sentinel" ]] || fail 'rejected postinstall key executed code'
    [[ ! -e "$(plugin_dir)" ]] || fail 'invalid manifest was installed'
    pass_case
}

test_manifest_diagnostic_sanitizes_terminal_controls()
{
    begin_case manifest_diagnostic_sanitizes_terminal_controls
    make_source diagnostic-plugin
    printf '{\n  name: "diagnostic-plugin",\n  version: "1.0.0",\n  api: 1,\n  entry: "src/main.fl",\n  capabilities: [],\n  events: [],\n  description: "fixture",\n  "bad\033]8;;https://invalid.example\a": true,\n}\n' \
        >"$source_repo/plugin.fl"
    git -C "$source_repo" add -- plugin.fl
    git -C "$source_repo" commit -q -m 'terminal-control manifest key'
    run_pkg install "$(source_url)"
    assert_status 1
    if LC_ALL=C grep $'\033' "$stdout_file" "$stderr_file" >/dev/null; then
        fail 'manifest diagnostic emitted a raw escape byte'
    fi
    assert_output_contains '\x1b'
    pass_case
}

test_offline_list_without_git()
{
    begin_case offline_list_without_git
    make_source offline-list-plugin
    install_head
    run_pkg_without_git list
    assert_status 0
    assert_output_contains offline-list-plugin
    assert_output_contains ok
    assert_output_lacks 'git not found'
    pass_case
}

test_offline_doctor_hash_without_git()
{
    begin_case offline_doctor_hash_without_git
    make_source offline-doctor-plugin
    install_head
    printf '# offline drift\n' >>"$(plugin_dir)/src/main.fl"
    run_pkg_without_git doctor offline-doctor-plugin
    assert_not_status 3
    assert_output_contains offline-doctor-plugin
    assert_output_contains drift
    assert_output_lacks 'git not found'
    pass_case
}

test_offline_install_exits_three()
{
    begin_case offline_install_exits_three
    make_source offline-install-plugin
    run_pkg_without_git install "$(source_url)"
    assert_status 3
    assert_stdout_exact ''
    assert_stderr_exact $'yew pkg: error: git not found in PATH\nyew pkg installs plugins over git; you can also copy a plugin directory into <plugins> by hand\n'
    pass_case
}

test_offline_update_exits_three()
{
    begin_case offline_update_exits_three
    make_source offline-update-plugin
    install_head
    advance_source offline-update-target
    run_pkg_without_git update offline-update-plugin
    assert_status 3
    assert_stdout_exact ''
    assert_stderr_exact $'yew pkg: error: git not found in PATH\nyew pkg installs plugins over git; you can also copy a plugin directory into <plugins> by hand\n'
    pass_case
}

test_deterministic_lock_and_reports()
{
    local first_list first_doctor first_lock
    begin_case deterministic_lock_and_reports
    make_source deterministic-plugin
    install_head
    first_lock=$case_root/plugins.lock.first
    first_list=$case_root/list.first
    first_doctor=$case_root/doctor.first
    cp "$(lock_path)" "$first_lock"
    run_pkg list
    assert_status 0
    cp "$stdout_file" "$first_list"
    run_pkg list
    assert_status 0
    cmp -s "$first_list" "$stdout_file" || fail 'list output is not deterministic'
    run_pkg doctor
    assert_status 0
    cp "$stdout_file" "$first_doctor"
    run_pkg doctor
    assert_status 0
    cmp -s "$first_doctor" "$stdout_file" || fail 'doctor output is not deterministic'
    cmp -s "$first_lock" "$(lock_path)" ||
        fail 'read-only list/doctor rewrote the lockfile'
    pass_case
}

test_three_install_lock_is_deterministic_across_clean_xdg_trees()
{
    local first name repo side
    local -a names repos
    begin_case three_install_lock_is_deterministic_across_clean_xdg_trees
    names=(det-alpha det-beta det-gamma)
    repos=()
    for name in "${names[@]}"; do
        make_source "$name"
        repos+=("$source_repo")
    done
    export YEW_TEST_PKG_NOW=1785600000
    for side in first second; do
        mkdir -p "$case_root/$side/data" "$case_root/$side/state" \
                 "$case_root/$side/config" "$case_root/$side/cache"
        export XDG_DATA_HOME=$case_root/$side/data
        export XDG_STATE_HOME=$case_root/$side/state
        export XDG_CONFIG_HOME=$case_root/$side/config
        export XDG_CACHE_HOME=$case_root/$side/cache
        for repo in "${repos[@]}"; do
            plugin_name=$(basename "$repo")
            plugin_name=${plugin_name#source-}
            source_repo=$repo
            install_head
        done
        if [[ $side == first ]]; then
            first=$(lock_path)
        fi
    done
    unset YEW_TEST_PKG_NOW
    cmp -s "$first" "$(lock_path)" ||
        fail 'three-plugin lock differs across clean XDG trees'
    pass_case
}

test_unmanaged_rows_are_sorted()
{
    local hostile=$'evil\tline\nesc\033]8;;invalid.example\a'

    begin_case unmanaged_rows_are_sorted
    mkdir -p "$XDG_DATA_HOME/yew/plugins/zeta" \
             "$XDG_DATA_HOME/yew/plugins/alpha" \
             "$XDG_DATA_HOME/yew/plugins/$hostile"
    run_pkg list
    assert_status 0
    assert_output_contains $'alpha\t-\t-\tunmanaged\t-'
    assert_output_contains $'zeta\t-\t-\tunmanaged\t-'
    [[ "$(sed -n '1p' "$stdout_file")" == alpha$'\t-\t-\tunmanaged\t-' ]] ||
        fail 'unmanaged rows are not byte-sorted'
    assert_output_contains 'evil\x09line\x0aesc\x1b]8;;invalid.example\x07'
    if LC_ALL=C grep -q $'\033' "$stdout_file"; then
        fail 'unmanaged row emitted a raw terminal escape'
    fi
    awk -F '\t' 'NF != 5 { exit 1 }' "$stdout_file" ||
        fail 'unmanaged row broke the fixed five-column format'
    pass_case
}

test_install_kill_boundaries_recover_fail_closed()
{
    local shim wrapper attempt at status
    local saw_disabled=0 saw_published=0 saw_locked=0

    begin_case install_kill_boundaries_recover_fail_closed
    make_source kill-install-plugin
    shim=$case_root/faultshim.so
    build_fault_shim "$shim"
    wrapper=$case_root/bin
    mkdir -p "$wrapper"
    printf '#!/bin/sh\nunset LD_PRELOAD DYLD_INSERT_LIBRARIES DYLD_FORCE_FLAT_NAMESPACE YEW_FAULT_AT YEW_FAULT_STORAGE_ONLY YEW_FAULT_STORAGE_ROOT\nexec %q "$@"\n' \
        "$real_git" >"$wrapper/git"
    chmod +x "$wrapper/git"
    for at in $(seq 0 160); do
        attempt=$case_root/attempt-$at
        mkdir -p "$attempt/data" "$attempt/state" "$attempt/config" \
                 "$attempt/cache"
        export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
        export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
        set +e
        PATH="$wrapper:$PATH" YEW_FAULT_AT=$at YEW_FAULT_STORAGE_ONLY=1 \
            YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" "$yew_bin" pkg install \
            "$(source_url)" --enable >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        if [[ $status -ne 137 || ! -f "$XDG_STATE_HOME/yew/pkg.intent" ]]; then
            continue
        fi
        if [[ -f "$XDG_STATE_HOME/yew/trust.fl" &&
              ! -d "$(plugin_dir)" ]]; then
            saw_disabled=1
        fi
        if [[ -d "$(plugin_dir)" &&
              ! -f "$(lock_path)" ]]; then
            saw_published=1
        fi
        if [[ -d "$(plugin_dir)" && -f "$(lock_path)" ]]; then
            saw_locked=1
        fi
        "$yew_bin" pkg list >"$stdout_file" 2>"$stderr_file" ||
            fail "install recovery failed at fault boundary $at"
        if [[ -d "$(plugin_dir)" ]]; then
            assert_file_contains "$(lock_path)" '"kill-install-plugin"'
            assert_file_contains "$XDG_STATE_HOME/yew/trust.fl" 'enabled: true'
        else
            [[ ! -f "$(lock_path)" ]] ||
                assert_file_lacks "$(lock_path)" '"kill-install-plugin"'
            [[ ! -e "$XDG_STATE_HOME/yew/trust.fl" ]] ||
                fail 'rolled-back install did not restore absent trust db'
        fi
        [[ ! -e "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
            fail 'install recovery left its durable intent'
        assert_no_staging \
            'install recovery left its recorded staging directory'
        if [[ $saw_disabled -eq 1 && $saw_published -eq 1 &&
              $saw_locked -eq 1 ]]; then
            break
        fi
    done
    [[ $saw_disabled -eq 1 ]] || fail 'did not kill after disabled trust commit'
    [[ $saw_published -eq 1 ]] || fail 'did not kill after plugin publication'
    [[ $saw_locked -eq 1 ]] || fail 'did not kill after lock publication'
    pass_case
}

test_install_commit_recovery_syncs_lock_parent()
{
    local shim wrapper attempt at status
    local hit=0

    begin_case install_commit_recovery_syncs_lock_parent
    make_source recover-lock-sync-plugin
    shim=$case_root/faultshim.so
    build_fault_shim "$shim"
    wrapper=$case_root/bin
    mkdir -p "$wrapper"
    printf '#!/bin/sh\nunset LD_PRELOAD DYLD_INSERT_LIBRARIES DYLD_FORCE_FLAT_NAMESPACE YEW_FAULT_AT YEW_FAULT_SAVE_META_EIO_AT YEW_FAULT_STORAGE_ONLY YEW_FAULT_STORAGE_ROOT\nexec %q "$@"\n' \
        "$real_git" >"$wrapper/git"
    chmod +x "$wrapper/git"
    for at in $(seq 0 160); do
        attempt=$case_root/attempt-$at
        mkdir -p "$attempt/data" "$attempt/state" "$attempt/config" \
                 "$attempt/cache"
        export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
        export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
        set +e
        PATH="$wrapper:$PATH" YEW_FAULT_AT=$at YEW_FAULT_STORAGE_ONLY=1 \
            YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" "$yew_bin" pkg install \
            "$(source_url)" >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        if [[ $status -ne 137 ||
              ! -f "$XDG_STATE_HOME/yew/pkg.intent" ||
              ! -d "$(plugin_dir)" || ! -f "$(lock_path)" ]] ||
           ! grep -F '"recover-lock-sync-plugin"' "$(lock_path)" \
                >/dev/null; then
            continue
        fi
        set +e
        YEW_FAULT_SAVE_META_EIO_AT=0 YEW_FAULT_STORAGE_ONLY=1 \
            YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" "$yew_bin" pkg list \
            >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        [[ $status -eq 3 ]] ||
            fail "recovery lock parent fsync fault exited $status, expected 3"
        [[ -f "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
            fail 'recovery cleared intent before the lock parent was durable'
        hit=1
        run_pkg list
        assert_status 0
        [[ ! -e "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
            fail 'retry left install intent after durable lock parent sync'
        assert_file_contains "$(lock_path)" '"recover-lock-sync-plugin"'
        [[ -d "$(plugin_dir)" ]] ||
            fail 'retry rolled back a committed install'
        assert_no_staging 'retry left install staging'
        break
    done
    [[ $hit -eq 1 ]] ||
        fail 'did not reach committed install recovery with a pending intent'
    pass_case
}

test_install_lock_parent_fsync_failure_recovers()
{
    local shim wrapper attempt at status
    local hit=0

    begin_case install_lock_parent_fsync_failure_recovers
    make_source lock-fsync-plugin
    shim=$case_root/faultshim.so
    build_fault_shim "$shim"
    wrapper=$case_root/bin
    mkdir -p "$wrapper"
    printf '#!/bin/sh\nunset LD_PRELOAD DYLD_INSERT_LIBRARIES DYLD_FORCE_FLAT_NAMESPACE YEW_FAULT_SAVE_META_EIO_AT YEW_FAULT_STORAGE_ONLY YEW_FAULT_STORAGE_ROOT\nexec %q "$@"\n' \
        "$real_git" >"$wrapper/git"
    chmod +x "$wrapper/git"
    for at in $(seq 0 80); do
        attempt=$case_root/attempt-$at
        mkdir -p "$attempt/data" "$attempt/state" "$attempt/config" \
                 "$attempt/cache"
        export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
        export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
        set +e
        PATH="$wrapper:$PATH" YEW_FAULT_SAVE_META_EIO_AT=$at \
            YEW_FAULT_STORAGE_ONLY=1 YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" \
            "$yew_bin" pkg install "$(source_url)" --enable \
            >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        if ! grep -F 'plugins.lock was replaced but its directory could not be synchronized' \
                "$stderr_file" >/dev/null; then
            continue
        fi
        hit=1
        [[ $status -eq 3 ]] ||
            fail "lock parent fsync fault exited $status, expected 3"
        [[ ! -d "$(plugin_dir)" ]] ||
            fail 'lock parent fsync fault retained an unlocked plugin tree'
        assert_file_lacks "$(lock_path)" '"lock-fsync-plugin"'
        [[ ! -e "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
            fail 'lock parent fsync recovery left its durable intent'
        assert_no_staging 'lock parent fsync recovery left staging'
        break
    done
    [[ $hit -eq 1 ]] || fail 'did not reach the plugins.lock parent fsync'
    pass_case
}

test_remove_kill_boundaries_restore_exact_policy()
{
    local shim wrapper baseline attempt at status before
    local saw_disabled=0 saw_quarantined=0 saw_committed=0

    begin_case remove_kill_boundaries_restore_exact_policy
    make_source kill-remove-plugin
    run_pkg install "$(source_url)" --enable
    assert_status 0
    baseline=$case_root/baseline
    cp -a "$case_root/xdg" "$baseline"
    before=$baseline/state/yew/trust.fl
    shim=$case_root/faultshim.so
    build_fault_shim "$shim"
    wrapper=$case_root/bin
    mkdir -p "$wrapper"
    printf '#!/bin/sh\nunset LD_PRELOAD DYLD_INSERT_LIBRARIES DYLD_FORCE_FLAT_NAMESPACE YEW_FAULT_AT YEW_FAULT_STORAGE_ONLY YEW_FAULT_STORAGE_ROOT\nexec %q "$@"\n' \
        "$real_git" >"$wrapper/git"
    chmod +x "$wrapper/git"
    for at in $(seq 0 120); do
        attempt=$case_root/remove-$at
        cp -a "$baseline" "$attempt"
        export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
        export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
        set +e
        PATH="$wrapper:$PATH" YEW_FAULT_AT=$at YEW_FAULT_STORAGE_ONLY=1 \
            YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" "$yew_bin" pkg remove \
            kill-remove-plugin >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        if [[ $status -ne 137 || ! -f "$XDG_STATE_HOME/yew/pkg.intent" ]]; then
            continue
        fi
        if [[ -d "$(plugin_dir)" ]] &&
           grep -F 'enabled: false' "$XDG_STATE_HOME/yew/trust.fl" >/dev/null; then
            saw_disabled=1
        fi
        if [[ ! -d "$(plugin_dir)" && -f "$(lock_path)" ]]; then
            saw_quarantined=1
        fi
        if [[ ! -d "$(plugin_dir)" ]] &&
           ! grep -F '"kill-remove-plugin"' "$(lock_path)" >/dev/null; then
            saw_committed=1
        fi
        "$yew_bin" pkg list >"$stdout_file" 2>"$stderr_file" ||
            fail "remove recovery failed at fault boundary $at"
        if [[ -d "$(plugin_dir)" ]]; then
            cmp -s "$before" "$XDG_STATE_HOME/yew/trust.fl" ||
                fail 'remove rollback did not restore exact trust bytes'
            assert_file_contains "$(lock_path)" '"kill-remove-plugin"'
        else
            assert_file_lacks "$(lock_path)" '"kill-remove-plugin"'
            assert_file_lacks "$XDG_STATE_HOME/yew/trust.fl" \
                '"kill-remove-plugin"'
        fi
        [[ ! -e "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
            fail 'remove recovery left its durable intent'
        if [[ $saw_disabled -eq 1 && $saw_quarantined -eq 1 &&
              $saw_committed -eq 1 ]]; then
            break
        fi
    done
    [[ $saw_disabled -eq 1 ]] || fail 'did not kill after remove trust staging'
    [[ $saw_quarantined -eq 1 ]] || fail 'did not kill after quarantine rename'
    [[ $saw_committed -eq 1 ]] || fail 'did not kill after remove lock commit'
    pass_case
}

test_remove_trash_parent_fsync_recovery_retries()
{
    local shim baseline attempt at status boundary=-1

    begin_case remove_trash_parent_fsync_recovery_retries
    make_source trash-fsync-plugin
    run_pkg install "$(source_url)" --enable
    assert_status 0
    baseline=$case_root/baseline
    cp -a "$case_root/xdg" "$baseline"
    shim=$case_root/faultshim.so
    build_fault_shim "$shim"
    for at in $(seq 0 100); do
        attempt=$case_root/probe-$at
        cp -a "$baseline" "$attempt"
        export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
        export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
        set +e
        YEW_FAULT_SAVE_META_EIO_AT=$at YEW_FAULT_STORAGE_ONLY=1 \
            YEW_FAULT_STORAGE_ROOT="$attempt" \
            run_with_fault_shim "$shim" "$yew_bin" pkg remove \
            trash-fsync-plugin >"$stdout_file" 2>"$stderr_file"
        status=$?
        set -e
        if grep -F 'could not durably remove residual trash' \
                "$stderr_file" >/dev/null; then
            boundary=$at
            [[ $status -eq 3 ]] ||
                fail "trash parent fsync fault exited $status, expected 3"
            break
        fi
    done
    [[ $boundary -ge 0 ]] || fail 'did not reach the trash parent fsync'

    attempt=$case_root/double-fault
    cp -a "$baseline" "$attempt"
    export XDG_DATA_HOME=$attempt/data XDG_STATE_HOME=$attempt/state
    export XDG_CONFIG_HOME=$attempt/config XDG_CACHE_HOME=$attempt/cache
    set +e
    YEW_FAULT_SAVE_META_EIO_AT=$boundary \
        YEW_FAULT_SAVE_META_EIO_AT2=$((boundary + 1)) \
        YEW_FAULT_STORAGE_ONLY=1 YEW_FAULT_STORAGE_ROOT="$attempt" \
        run_with_fault_shim "$shim" \
        "$yew_bin" pkg remove trash-fsync-plugin \
        >"$stdout_file" 2>"$stderr_file"
    status=$?
    set -e
    [[ $status -eq 3 ]] ||
        fail "double trash parent fsync fault exited $status, expected 3"
    [[ -f "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
        fail 'recovery cleared intent after its absent-trash fsync failed'
    run_pkg list
    assert_status 0
    [[ ! -e "$XDG_STATE_HOME/yew/pkg.intent" ]] ||
        fail 'retry left remove intent after a durable parent sync'
    assert_file_lacks "$(lock_path)" '"trash-fsync-plugin"'
    [[ ! -d "$(plugin_dir)" ]] ||
        fail 'retry restored a removal that was already committed'
    if compgen -G "$XDG_DATA_HOME/yew/.pkg-trash-*" >/dev/null; then
        fail 'retry left residual removal trash'
    fi
    pass_case
}

test_install_head
test_shipped_examples_install_as_local_repositories
test_install_tag
test_historical_pin_selects_historical_manifest
test_install_rev
test_install_branch
test_list_porcelain
test_doctor_clean
test_update_noop
test_update_fast_forward
test_update_dry_run
test_update_changelog_reports_remainder
test_update_changelog_failure_aborts_unchanged
test_update_force_push_refused
test_update_moved_tag_refused
test_update_dirty_refused
test_update_untracked_is_not_deleted
test_doctor_accepts_drift
test_doctor_fixes_drift
test_doctor_fix_preserves_added_paths
test_doctor_accepts_tree_and_head_together
test_doctor_reports_paths_and_capabilities
test_remove_managed_plugin
test_remove_trust_failure_rolls_back
test_invalid_bare_spec
test_cli_diagnostic_sanitizes_terminal_controls
test_invalid_ref_rejected
test_malicious_lock_name_cannot_escape_plugins
test_pin_options_are_mutually_exclusive
test_dash_prefixed_name_uses_option_terminator
test_corrupt_lock_preserved
test_force_relock_discards_corrupt_lock
test_timeout_argument_validation
test_network_timeout_kills_stalled_git
test_unreachable_remote_exits_three_without_timeout
test_install_executes_no_repository_code
test_install_warns_once_for_uninitialized_submodules
test_postinstall_key_is_rejected_without_execution
test_manifest_diagnostic_sanitizes_terminal_controls
test_offline_list_without_git
test_offline_doctor_hash_without_git
test_offline_install_exits_three
test_offline_update_exits_three
test_deterministic_lock_and_reports
test_three_install_lock_is_deterministic_across_clean_xdg_trees
test_unmanaged_rows_are_sorted
if [[ $preload_faults == true ]]; then
    test_install_kill_boundaries_recover_fail_closed
    test_install_commit_recovery_syncs_lock_parent
    test_install_lock_parent_fsync_failure_recovers
    test_remove_kill_boundaries_restore_exact_policy
    test_remove_trash_parent_fsync_recovery_retries
else
    for name in install_kill_boundaries_recover_fail_closed \
                install_commit_recovery_syncs_lock_parent \
                install_lock_parent_fsync_failure_recovers \
                remove_kill_boundaries_restore_exact_policy \
                remove_trash_parent_fsync_recovery_retries; do
        printf 'SKIP %s: static PIE cannot load the LD_PRELOAD fault shim\n' \
            "$name"
    done
fi

printf 'pkg integration: %d passed, 0 failed\n' "$passed"
