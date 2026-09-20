#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
make_bin=${1:-make}
uname_bin=${YEW_PERF_UNAME:-uname}
git_bin=${YEW_PERF_GIT:-git}
sha256_bin=${YEW_PERF_SHA256:-sha256sum}
build=${BUILD:-build}
runner_id=${PERF_RUNNER_ID:-}
reference=${CALIB_REFERENCE:-}
baseline=${PERF_BASELINE:-}
budgets=${PERF_BUDGETS:-tests/perf/budgets.txt}
resume=${PERF_NOISE_RESUME:-}
runs=30

die()
{
    echo "perf-noise: $*" >&2
    exit 2
}

field()
{
    awk -v key="$2" '$1 == key { print $2; exit }' "$1"
}

measure()
{
    output=$1
    "$make_bin" --no-print-directory calib BUILD="$build" \
        PERF_RUNNER_ID="$runner_id" CALIB_REFERENCE="$reference" \
        CALIB_OUTPUT="$output"
}

check_calibration()
{
    file=$1
    [ "$(field "$file" mode)" = GATING ] ||
        die "calibration did not enter GATING mode: $file"
    scale=$(field "$file" scale_permille)
    case $scale in ''|*[!0-9]*|0) die "invalid calibration scale: $file" ;; esac
}

require_manifest_field()
{
    actual=$(field "$manifest" "$1")
    [ "$actual" = "$2" ] ||
        die "resume manifest $1 mismatch: expected $2, got ${actual:-<missing>}"
}

cd "$repo"
[ "$($uname_bin -s)" = Linux ] || die 'noise evidence requires Linux'
arch=$($uname_bin -m)
case $runner_id in
    perf-x86_64-linux-gnu)
        [ "$arch" = x86_64 ] ||
            die "runner $runner_id requires x86_64, got $arch"
        [ -n "$reference" ] || reference=tests/perf/calib-reference.txt
        [ -n "$baseline" ] ||
            baseline=tests/perf/baselines/perf-x86_64-linux-gnu.txt
        ;;
    perf-arm64-linux)
        case $arch in aarch64|arm64) ;; *)
            die "runner $runner_id requires arm64, got $arch" ;;
        esac
        [ -n "$reference" ] ||
            reference=tests/perf/calib-reference-arm64.txt
        [ -n "$baseline" ] ||
            baseline=tests/perf/baselines/perf-arm64-linux.txt
        ;;
    *) die 'PERF_RUNNER_ID must name a designated runner' ;;
esac
[ -f "$reference" ] || die "missing designated reference $reference"
[ -f "$baseline" ] || die "missing designated baseline $baseline"
[ -f "$budgets" ] || die "missing budgets $budgets"
source_commit=$($git_bin rev-parse HEAD 2>/dev/null) ||
    die 'noise evidence requires a Git checkout'
$git_bin diff --quiet --ignore-submodules -- ||
    die 'noise evidence requires a clean tracked worktree'
$git_bin diff --cached --quiet --ignore-submodules -- ||
    die 'noise evidence requires a clean index'
reference_sha=$($sha256_bin "$reference" | awk '{ print $1 }')
baseline_sha=$($sha256_bin "$baseline" | awk '{ print $1 }')
[ -n "$reference_sha" ] && [ -n "$baseline_sha" ] ||
    die 'cannot hash designated inputs'

kernel_release=$($uname_bin -r)
if [ -n "$resume" ]; then
    campaign=$resume
    [ "$(dirname -- "$campaign")" = "$build/perf-noise" ] &&
    case $(basename -- "$campaign") in campaign-*) true ;; *) false ;; esac ||
        die 'PERF_NOISE_RESUME must name a generated campaign in BUILD/perf-noise'
    manifest=$campaign/manifest.txt
    [ -f "$manifest" ] || die "resume manifest is missing: $manifest"
    # YEW-F-072: an interrupted designated campaign may resume only when
    # every input that gives its observations meaning is byte-identical.
    require_manifest_field source_commit "$source_commit"
    require_manifest_field runner_id "$runner_id"
    require_manifest_field arch "$arch"
    require_manifest_field kernel_release "$kernel_release"
    require_manifest_field reference "$reference"
    require_manifest_field reference_sha256 "$reference_sha"
    require_manifest_field baseline "$baseline"
    require_manifest_field baseline_sha256 "$baseline_sha"
    require_manifest_field runs "$runs"
    require_manifest_field relative_threshold_permille 100
    before=$campaign/calib-before.txt
    [ -f "$before" ] || die "resume calibration is missing: $before"
    check_calibration "$before"
    before_scale=$scale
else
    campaign=$build/perf-noise/campaign-$$
    mkdir -p "$campaign"
    manifest=$campaign/manifest.txt
    {
        echo '# yew perf noise evidence v1'
        echo "source_commit $source_commit"
        echo "runner_id $runner_id"
        echo "arch $arch"
        echo "kernel_release $kernel_release"
        echo "reference $reference"
        echo "reference_sha256 $reference_sha"
        echo "baseline $baseline"
        echo "baseline_sha256 $baseline_sha"
        echo "runs $runs"
        echo 'relative_threshold_permille 100'
        echo "started_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$manifest"
    before=$campaign/calib-before.txt
    measure "$before"
    check_calibration "$before"
    before_scale=$scale
fi
after=$campaign/calib-after.txt

run=1
while [ "$run" -le "$runs" ] && [ -f "$campaign/run-$run.log" ]; do
    [ -s "$campaign/run-$run.log" ] ||
        die "completed run is empty: $campaign/run-$run.log"
    run=$((run + 1))
done
probe=$((run + 1))
while [ "$probe" -le "$runs" ]; do
    [ ! -e "$campaign/run-$probe.log" ] ||
        die "resume campaign has a gap before run $probe"
    probe=$((probe + 1))
done
if [ -n "$resume" ]; then
    echo "resumed_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$manifest"
fi
while [ "$run" -le "$runs" ]; do
    log=$campaign/run-$run.log
    temporary=$log.tmp
    attempt=1
    while [ -e "$temporary" ]; do
        temporary=$log.tmp.$attempt
        attempt=$((attempt + 1))
    done
    echo "perf-noise: run $run/$runs"
    # YEW-F-072: the resume selector belongs only to this driver.  Clear it
    # for nested makes so their policy self-tests do not mistake the real
    # campaign path for one of their isolated synthetic campaigns.
    if PERF_GATE=0 PERF_S56_EVALUATE=1 \
       "$make_bin" --no-print-directory perf BUILD="$build" \
       PERF_RUNNER_ID="$runner_id" PERF_BASELINE="$baseline" \
       CALIB_REFERENCE="$reference" PERF_BUDGETS="$budgets" \
       PERF_NOISE_RESUME= \
       >"$temporary" 2>&1 && \
       PERF_GATE=0 PERF_S56_EVALUATE=1 \
       "$make_bin" --no-print-directory perf-huge BUILD="$build" \
       PERF_RUNNER_ID="$runner_id" PERF_BASELINE="$baseline" \
       CALIB_REFERENCE="$reference" PERF_BUDGETS="$budgets" \
       PERF_NOISE_RESUME= \
       >>"$temporary" 2>&1; then
        mv "$temporary" "$log"
    else
        status=$?
        cat "$temporary"
        echo "perf-noise: run $run failed; evidence kept at $temporary" >&2
        exit "$status"
    fi
    run=$((run + 1))
done

measure "$after"
check_calibration "$after"
after_scale=$scale
drift=$((after_scale > before_scale ? after_scale - before_scale : before_scale - after_scale))
if [ "$drift" -gt $((before_scale * 15 / 100)) ]; then
    die "campaign calibration drifted $before_scale -> $after_scale"
fi

# The campaign directory is generated without whitespace by the supported
# BUILD interface; the analyzer independently verifies all 30 files.
# shellcheck disable=SC2086
if "$repo/scripts/perf-noise-floor.sh" "$budgets" $campaign/run-*.log \
    >"$campaign/noise-floor-body.txt"; then
    status=0
else
    status=$?
fi
{
    cat "$manifest"
    echo "finished_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    cat "$campaign/noise-floor-body.txt"
} >"$campaign/noise-floor.txt"
cat "$campaign/noise-floor.txt"
if [ "$status" -ne 0 ]; then
    echo "perf-noise: threshold audit failed; evidence retained" >&2
    exit "$status"
fi
echo "perf-noise: evidence $campaign"
