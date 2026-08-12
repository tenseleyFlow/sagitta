#!/bin/sh

# Run the authoritative syntax benchmark only after the host has remained
# quiet and cool for a sustained window.  Cooperating jobs should use the
# same lock path (shared for background work, exclusive for this runner).
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
workdir=${YEW_PERF_WORKDIR:-$repo}
lock_path=${YEW_PERF_LOCK_PATH:-${TMPDIR:-/tmp}/yew-perf-syn.lock}
lock_timeout=${YEW_PERF_LOCK_TIMEOUT:-1800}
quiet_timeout=${YEW_PERF_QUIET_TIMEOUT:-1800}
quiet_window=${YEW_PERF_QUIET_WINDOW:-30}
poll_seconds=${YEW_PERF_POLL_SECONDS:-1}
status_interval=${YEW_PERF_STATUS_INTERVAL:-30}
idle_min=${YEW_PERF_MIN_IDLE_PERCENT:-95}
run_sibling_idle_min=${YEW_PERF_MIN_RUN_SIBLING_IDLE_PERCENT:-$idle_min}
temp_max=${YEW_PERF_MAX_TEMP_C:-75}
run_temp_max=${YEW_PERF_MAX_RUN_TEMP_C:-100}
run_timeout=${YEW_PERF_RUN_TIMEOUT:-1800}
busy_pattern=${YEW_PERF_BUSY_PATTERN:-'(^|[[:space:]/])(cgfried|cc1|cc1plus|gcc|g\+\+|clang|clang\+\+|cargo|rustc|torture-run|import-c-testsuite(\.sh)?|s35_loop_driver|ctfe_diagnostic|release_native)([[:space:]/]|$)'}
stat_file=${YEW_PERF_STAT_FILE:-/proc/stat}
topology_root=${YEW_PERF_TOPOLOGY_ROOT:-/sys/devices/system/cpu}

die()
{
    printf 'perf-syn-quiet: %s\n' "$*" >&2
    exit 2
}

check_uint()
{
    check_name=$1
    check_value=$2
    case $check_value in
        ''|*[!0-9]*) die "$check_name must be an unsigned integer" ;;
    esac
}

check_uint YEW_PERF_LOCK_TIMEOUT "$lock_timeout"
check_uint YEW_PERF_QUIET_TIMEOUT "$quiet_timeout"
check_uint YEW_PERF_QUIET_WINDOW "$quiet_window"
check_uint YEW_PERF_POLL_SECONDS "$poll_seconds"
check_uint YEW_PERF_STATUS_INTERVAL "$status_interval"
check_uint YEW_PERF_MIN_IDLE_PERCENT "$idle_min"
check_uint YEW_PERF_MIN_RUN_SIBLING_IDLE_PERCENT "$run_sibling_idle_min"
check_uint YEW_PERF_MAX_TEMP_C "$temp_max"
check_uint YEW_PERF_MAX_RUN_TEMP_C "$run_temp_max"
check_uint YEW_PERF_RUN_TIMEOUT "$run_timeout"
[ "$lock_timeout" -gt 0 ] || die 'YEW_PERF_LOCK_TIMEOUT must be greater than zero'
[ "$quiet_timeout" -gt 0 ] || die 'YEW_PERF_QUIET_TIMEOUT must be greater than zero'
[ "$poll_seconds" -gt 0 ] || die 'YEW_PERF_POLL_SECONDS must be greater than zero'
[ "$status_interval" -gt 0 ] || die 'YEW_PERF_STATUS_INTERVAL must be greater than zero'
[ "$idle_min" -le 100 ] || die 'YEW_PERF_MIN_IDLE_PERCENT must not exceed 100'
[ "$run_sibling_idle_min" -le 100 ] ||
    die 'YEW_PERF_MIN_RUN_SIBLING_IDLE_PERCENT must not exceed 100'
[ "$run_temp_max" -ge "$temp_max" ] ||
    die 'YEW_PERF_MAX_RUN_TEMP_C must be at least YEW_PERF_MAX_TEMP_C'

if printf '%s\n' probe | grep -E "$busy_pattern" >/dev/null 2>&1; then
    :
else
    grep_status=$?
    [ "$grep_status" -eq 1 ] || die 'YEW_PERF_BUSY_PATTERN is not a valid extended regular expression'
fi

[ -d "$workdir" ] || die "benchmark workdir is not a directory: $workdir"
cd "$workdir"
if [ "$#" -eq 0 ] && [ -n "${YEW_PERF_SYN_COMMAND:-}" ]; then
    set -f
    # The Make target supplies a whitespace-separated executable and flags.
    # shellcheck disable=SC2086
    set -- $YEW_PERF_SYN_COMMAND
    set +f
elif [ "$#" -eq 0 ]; then
    if [ "${YEW_PERF_SKIP_BUILD:-0}" != 1 ]; then
        printf 'perf-syn-quiet: building benchmark\n' >&2
        make -C "$repo" build/perf_syn build/yew >/dev/null
    fi
    set -- "$repo/build/perf_syn"
fi
[ -n "${1:-}" ] || die 'benchmark command is empty'

cleanup_lock_kind=none
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-syn-quiet.XXXXXX") ||
    die 'cannot create scratch directory'
cleanup()
{
    if [ "$cleanup_lock_kind" = mkdir ]; then
        rm -f "$lock_path.d/pid"
        rmdir "$lock_path.d" 2>/dev/null || :
    fi
    rm -f "$scratch/output"
    rm -f "$scratch"/idle-*
    rmdir "$scratch" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

sleep_for_poll()
{
    if [ -n "${YEW_PERF_SLEEP_COMMAND:-}" ]; then
        "$YEW_PERF_SLEEP_COMMAND" "$poll_seconds"
    else
        sleep "$poll_seconds"
    fi
}

flock_tool=
if [ "${YEW_PERF_FLOCK+x}" = x ]; then
    [ "$YEW_PERF_FLOCK" = none ] || flock_tool=$YEW_PERF_FLOCK
elif command -v flock >/dev/null 2>&1; then
    flock_tool=$(command -v flock)
fi

lock_elapsed=0
if [ -n "$flock_tool" ]; then
    exec 9>"$lock_path" || die "cannot open lock: $lock_path"
    while ! "$flock_tool" -n 9; do
        if [ "$lock_elapsed" -ge "$lock_timeout" ]; then
            printf 'perf-syn-quiet: lock timeout after %ss\n' "$lock_elapsed" >&2
            exit 75
        fi
        sleep_for_poll
        lock_elapsed=$((lock_elapsed + poll_seconds))
    done
    cleanup_lock_kind=flock
else
    while ! mkdir "$lock_path.d" 2>/dev/null; do
        holder=
        if [ -r "$lock_path.d/pid" ]; then
            IFS= read -r holder <"$lock_path.d/pid" || holder=
        fi
        case $holder in
            ''|*[!0-9]*) ;;
            *)
                if ! kill -0 "$holder" 2>/dev/null; then
                    rm -f "$lock_path.d/pid"
                    rmdir "$lock_path.d" 2>/dev/null || :
                    continue
                fi
                ;;
        esac
        if [ "$lock_elapsed" -ge "$lock_timeout" ]; then
            printf 'perf-syn-quiet: lock timeout after %ss\n' "$lock_elapsed" >&2
            exit 75
        fi
        sleep_for_poll
        lock_elapsed=$((lock_elapsed + poll_seconds))
    done
    cleanup_lock_kind='mkdir'
    printf '%s\n' "$$" >"$lock_path.d/pid"
fi
printf 'perf-syn-quiet: exclusive lock acquired\n' >&2

has_competitor()
{
    if [ -n "${YEW_PERF_PS_COMMAND:-}" ]; then
        ps_output=$("$YEW_PERF_PS_COMMAND" 2>/dev/null || :)
    else
        ps_output=$(ps -eo pid=,comm=,args= 2>/dev/null || :)
    fi
    printf '%s\n' "$ps_output" | grep -E "$busy_pattern" >/dev/null 2>&1
}

cpu=${YEW_PERF_CPU:-}
if [ -z "$cpu" ]; then
    cpu=$(awk '/^Cpus_allowed_list:/ {
        split($2, groups, ","); split(groups[1], span, "-"); print span[1]
    }' /proc/self/status 2>/dev/null || :)
    cpu=${cpu:-0}
fi
check_uint YEW_PERF_CPU "$cpu"
cpu_threads=$cpu
topology_file=$topology_root/cpu$cpu/topology/thread_siblings_list
if [ -r "$topology_file" ]; then
    sibling_spec=$(awk 'NR == 1 { print; exit }' "$topology_file" 2>/dev/null || :)
    expanded=$(printf '%s\n' "$sibling_spec" | awk -F, '
        {
            for (i = 1; i <= NF; i++) {
                n = split($i, span, "-")
                if (n == 1 && span[1] ~ /^[0-9]+$/) {
                    print span[1]
                } else if (n == 2 && span[1] ~ /^[0-9]+$/ &&
                           span[2] ~ /^[0-9]+$/ && span[1] <= span[2]) {
                    for (cpu = span[1]; cpu <= span[2]; cpu++)
                        print cpu
                } else {
                    exit 2
                }
            }
        }
    ') || die "invalid thread sibling list: $sibling_spec"
    [ -z "$expanded" ] || cpu_threads=$(printf '%s\n' "$expanded" |
        awk '!seen[$1]++ { if (out != "") out = out " "; out = out $1 }
             END { print out }')
fi
sibling_threads=
for thread_cpu in $cpu_threads; do
    check_uint thread_sibling "$thread_cpu"
    if [ "$thread_cpu" != "$cpu" ]; then
        sibling_threads=${sibling_threads:+$sibling_threads }$thread_cpu
    fi
done

read_idle_set()
{
    idle_set=$1
    idle_result=
    [ -n "$idle_set" ] || return
    if [ -n "${YEW_PERF_IDLE_COMMAND:-}" ]; then
        idle_result=$("$YEW_PERF_IDLE_COMMAND" 2>/dev/null || :)
        case $idle_result in
            ''|*[!0-9]*) idle_result= ;;
            *) [ "$idle_result" -le 100 ] || idle_result= ;;
        esac
        return
    fi
    idle_known=1
    idle_minimum=100
    for idle_cpu in $idle_set; do
        cpu_stat_name=cpu$idle_cpu
        cpu_line=$(awk -v wanted="$cpu_stat_name" \
            '$1 == wanted { print; exit }' "$stat_file" 2>/dev/null || :)
        set -f
        # Splitting the /proc/stat fields is intentional; globbing is disabled.
        # shellcheck disable=SC2086
        set -- $cpu_line
        set +f
        if [ "${1:-}" != "$cpu_stat_name" ] || [ "$#" -lt 5 ]; then
            idle_known=0
            continue
        fi
        shift
        cpu_total=0
        cpu_field_count=0
        cpu_idle_now=0
        for cpu_field in "$@"; do
            case $cpu_field in
                ''|*[!0-9]*) idle_known=0; break ;;
            esac
            cpu_field_count=$((cpu_field_count + 1))
            cpu_total=$((cpu_total + cpu_field))
            if [ "$cpu_field_count" -eq 4 ] ||
               [ "$cpu_field_count" -eq 5 ]; then
                cpu_idle_now=$((cpu_idle_now + cpu_field))
            fi
        done
        idle_state=$scratch/idle-$idle_cpu
        previous_total=
        previous_idle=
        if [ -r "$idle_state" ]; then
            IFS=' ' read -r previous_total previous_idle <"$idle_state" || :
        fi
        printf '%s %s\n' "$cpu_total" "$cpu_idle_now" >"$idle_state"
        if [ -z "$previous_total" ] || [ "$cpu_total" -le "$previous_total" ]; then
            idle_known=0
            continue
        fi
        cpu_delta=$((cpu_total - previous_total))
        idle_delta=$((cpu_idle_now - previous_idle))
        current_idle=$((idle_delta * 100 / cpu_delta))
        if [ "$current_idle" -lt "$idle_minimum" ]; then
            idle_minimum=$current_idle
        fi
    done
    if [ "$idle_known" -eq 1 ]; then
        idle_result=$idle_minimum
    fi
}

read_package_temp()
{
    package_temp=
    if [ -n "${YEW_PERF_TEMP_COMMAND:-}" ]; then
        package_temp=$("$YEW_PERF_TEMP_COMMAND" 2>/dev/null || :)
    elif command -v sensors >/dev/null 2>&1; then
        package_temp=$(sensors 2>/dev/null | awk '
            /Package id|Tctl|Tdie/ {
                package = 1
                for (i = 1; i <= NF; i++) {
                    if ($i !~ /^\+/)
                        continue
                    value = $i
                    gsub(/[^0-9.]/, "", value)
                    if (value != "" && (!found || value > maximum)) {
                        maximum = value
                        found = 1
                    }
                    package = 0
                }
                next
            }
            package && /_input:/ {
                value = $2 + 0
                if (!found || value > maximum) {
                    maximum = value
                    found = 1
                }
                package = 0
            }
            END { if (found) printf "%.0f\n", maximum }
        ' || :)
    fi
    case $package_temp in
        ''|*[!0-9]*) package_temp= ;;
    esac
}

quiet_elapsed=0
quiet_streak=0
temp_notice=0
while [ "$quiet_streak" -lt "$quiet_window" ]; do
    competing=0
    has_competitor && competing=1
    read_idle_set "$cpu_threads"
    cpu_idle=$idle_result
    read_package_temp
    if [ -z "$package_temp" ] && [ "$temp_notice" -eq 0 ]; then
        printf 'perf-syn-quiet: package temperature unavailable; continuing\n' >&2
        temp_notice=1
    fi
    if [ "$competing" -eq 0 ] && [ -n "$cpu_idle" ] &&
       [ "$cpu_idle" -ge "$idle_min" ] &&
       { [ -z "$package_temp" ] || [ "$package_temp" -le "$temp_max" ]; }; then
        quiet_streak=$((quiet_streak + poll_seconds))
    else
        quiet_streak=0
    fi
    if [ "$quiet_streak" -ge "$quiet_window" ]; then
        break
    fi
    if [ $((quiet_elapsed % status_interval)) -eq 0 ]; then
        printf 'perf-syn-quiet: waiting; cpu=%s threads=%s idle=%s%% temp=%sC competitor=%s streak=%ss\n' \
            "$cpu" "$cpu_threads" "${cpu_idle:-unknown}" \
            "${package_temp:-unknown}" "$competing" "$quiet_streak" >&2
    fi
    if [ "$quiet_elapsed" -ge "$quiet_timeout" ]; then
        printf 'perf-syn-quiet: quiet-window timeout after %ss\n' "$quiet_elapsed" >&2
        exit 75
    fi
    sleep_for_poll
    quiet_elapsed=$((quiet_elapsed + poll_seconds))
done

taskset_tool=
if [ "${YEW_PERF_TASKSET+x}" = x ]; then
    [ "$YEW_PERF_TASKSET" = none ] || taskset_tool=$YEW_PERF_TASKSET
elif command -v taskset >/dev/null 2>&1; then
    taskset_tool=$(command -v taskset)
fi

printf 'perf-syn-quiet: quiet for %ss; cpu=%s samples=1001\n' \
    "$quiet_streak" "$cpu" >&2
if [ -n "$taskset_tool" ]; then
    YEW_SYN_PERF_SAMPLES=1001 "$taskset_tool" -c "$cpu" "$@" \
        >"$scratch/output" 2>&1 &
else
    YEW_SYN_PERF_SAMPLES=1001 "$@" >"$scratch/output" 2>&1 &
fi
benchmark_pid=$!
run_elapsed=0
contaminated=0
sibling_contaminated=0
sibling_idle_low=unknown
timed_out=0
while kill -0 "$benchmark_pid" 2>/dev/null; do
    has_competitor && contaminated=1
    if [ -n "$sibling_threads" ]; then
        read_idle_set "$sibling_threads"
        sibling_idle=$idle_result
        if [ -z "$sibling_idle" ] ||
           [ "$sibling_idle" -lt "$run_sibling_idle_min" ]; then
            contaminated=1
            sibling_contaminated=1
            if [ -n "$sibling_idle" ]; then
                sibling_idle_low=$sibling_idle
            fi
        fi
    fi
    read_package_temp
    if [ -n "$package_temp" ] && [ "$package_temp" -ge "$run_temp_max" ]; then
        contaminated=1
    fi
    if [ "$run_timeout" -gt 0 ] && [ "$run_elapsed" -ge "$run_timeout" ]; then
        timed_out=1
        kill -TERM "$benchmark_pid" 2>/dev/null || :
        break
    fi
    sleep_for_poll
    run_elapsed=$((run_elapsed + poll_seconds))
done
if wait "$benchmark_pid"; then
    benchmark_status=0
else
    benchmark_status=$?
fi

# Catch a competitor that appeared between the final monitor poll and wait.
has_competitor && contaminated=1
if [ -n "$sibling_threads" ]; then
    read_idle_set "$sibling_threads"
    sibling_idle=$idle_result
    if [ -z "$sibling_idle" ] ||
       [ "$sibling_idle" -lt "$run_sibling_idle_min" ]; then
        contaminated=1
        sibling_contaminated=1
        if [ -n "$sibling_idle" ]; then
            sibling_idle_low=$sibling_idle
        fi
    fi
fi
read_package_temp
if [ -n "$package_temp" ] && [ "$package_temp" -ge "$run_temp_max" ]; then
    contaminated=1
fi

if [ "$timed_out" -eq 1 ]; then
    printf 'perf-syn-quiet: benchmark timeout after %ss; results discarded\n' \
        "$run_elapsed" >&2
    exit 75
fi
if [ "$contaminated" -eq 1 ]; then
    if [ "$sibling_contaminated" -eq 1 ]; then
        printf 'perf-syn-quiet: run contaminated; sibling idle %s%% below %s%%; results discarded\n' \
            "$sibling_idle_low" "$run_sibling_idle_min" >&2
    else
        printf 'perf-syn-quiet: run contaminated; results discarded\n' >&2
    fi
    exit 75
fi

cat "$scratch/output"
if [ "$benchmark_status" -ne 0 ]; then
    printf 'perf-syn-quiet: benchmark failed with status %s\n' \
        "$benchmark_status" >&2
    exit "$benchmark_status"
fi
printf 'perf-syn-quiet: clean run completed\n' >&2
