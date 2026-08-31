#!/bin/sh

# PID 1 for Sprint 57's constrained-target image.  Keep this BusyBox-only:
# the image deliberately carries no distro userspace beyond the static
# applets, yew, and its purpose-built test binaries.

set -u
export PATH=/bin
export HOME=/work/home
export TMPDIR=/tmp
export XDG_STATE_HOME=/work/state
export XDG_CONFIG_HOME=/work/config
export XDG_CACHE_HOME=/work/cache
export LC_ALL=C

mkdir -p "$HOME" "$XDG_STATE_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" \
    /work/build
mount -t proc proc /proc >/dev/null 2>&1 || true
mount -t sysfs sysfs /sys >/dev/null 2>&1 || true
mount -t devtmpfs devtmpfs /dev >/dev/null 2>&1 || true
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts >/dev/null 2>&1 || true

mode=full
case " $(cat /proc/cmdline 2>/dev/null) " in
    *' yew.embed.mode=lowmem '*) mode=lowmem ;;
esac

prepare_external_storage()
{
    for module in virtio_blk mbcache ext2; do
        if ! insmod "/modules/$module.ko"; then
            echo "YEW_EMBED_STORAGE status=fail detail=insmod-$module"
            return 1
        fi
    done
    if ! mount -t ext2 -o noatime /dev/vda /work/build; then
        echo 'YEW_EMBED_STORAGE status=fail detail=mount-work'
        return 1
    fi
    rm -f /modules/virtio_blk.ko /modules/mbcache.ko /modules/ext2.ko
    echo 'YEW_EMBED_STORAGE status=pass detail=virtio-ext2'
    generate_fixture
}

failures=0
peak_bytes=0

report_memory()
{
    awk -v tag="$1" '
        /^MemTotal:/ { total = $2 }
        /^MemFree:/ { free = $2 }
        /^MemAvailable:/ { available = $2 }
        /^Cached:/ { cached = $2 }
        /^Slab:/ { slab = $2 }
        END {
            printf "YEW_EMBED_MEMORY stage=%s total_kib=%s free_kib=%s available_kib=%s cached_kib=%s slab_kib=%s\n", tag, total, free, available, cached, slab
        }
    ' /proc/meminfo
}

row_pass()
{
    echo "YEW_EMBED_ROW row=$1 status=pass detail=$2"
}

row_fail()
{
    failures=$((failures + 1))
    echo "YEW_EMBED_ROW row=$1 status=fail detail=$2"
}

generate_fixture()
{
    if /bin/gen-embedded-fixture /bin/gen-bigfile \
        /work/build/pty-s57-embedded-4m.c; then
        echo 'YEW_EMBED_FIXTURE status=pass bytes=4194304'
        return 0
    fi
    failures=$((failures + 1))
    echo 'YEW_EMBED_FIXTURE status=fail'
    return 1
}

update_peak_bytes()
{
    candidate=$1
    case $candidate in ''|*[!0-9]*) return ;; esac
    if [ "$candidate" -gt "$peak_bytes" ]; then
        peak_bytes=$candidate
    fi
}

update_yew_peaks()
{
    log=$1
    tag=$2
    [ -f "$log" ] || return
    value=$(sed -n \
        's/.*peak_bytes=\([0-9][0-9]*\).*/\1/p' "$log" |
        sort -n | tail -n 1)
    update_peak_bytes "$value"
    case $value in
        ''|*[!0-9]*) ;;
        *) echo "YEW_EMBED_CASE_RSS case=$tag peak_bytes=$value" ;;
    esac
}

run_pty()
{
    filter=$1
    out="/work/pty-$filter.out"
    log="/work/yew-rss-$filter.log"
    rm -f "$log"

    YEW_PROF=1 YEW_LOG="$log" \
    YEW_PTY_S57_REUSE=1 \
    YEW_PTY_FILTER="$filter" YEW_PTY_BUDGET_MS=600000 \
    YEW_PTY_CASE_BUDGET_MS=120000 \
        /bin/pty_runner --demo /bin/yew --yew /bin/yew-embedded \
        >"$out" 2>&1
    rc=$?
    update_yew_peaks "$log" "$filter"
    if [ "$rc" -ne 0 ]; then
        echo "YEW_EMBED_DIAG case=$filter"
        sed -n '1,120p' "$out"
    fi
    return "$rc"
}

oom_free()
{
    dmesg > /work/dmesg.txt 2>/dev/null || true
    ! grep -E -i \
        'Out of memory.*yew|Killed process [0-9]+ \(yew\)' \
        /work/dmesg.txt >/dev/null 2>&1
}

run_lowmem()
{
    mem_kib=$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)
    case $mem_kib in ''|*[!0-9]*) mem_kib=0 ;; esac
    if [ "$mem_kib" -lt 49152 ]; then
        echo "yew: error: embedded 4 MiB workload requires at least 48 MiB; MemTotal=${mem_kib}KiB"
        echo 'YEW_EMBED_ROW row=12 status=refused detail=memory-preflight'
    elif ! generate_fixture; then
        row_fail 12 fixture-generation
    else
        bytes=$(wc -c </work/build/pty-s57-embedded-4m.c 2>/dev/null |
            tr -d ' ')
        if [ "$bytes" != 4194304 ]; then
            row_fail 12 fixture-not-4m
        else
            before=$(sha256sum /work/build/pty-s57-embedded-4m.c |
                awk '{print $1}')
            log=/work/yew-rss-lowmem.log
            rm -f "$log"
            if YEW_PROF=1 YEW_LOG="$log" \
               /bin/yew --batch --test --clean \
                /work/tests/script/57_embedded_gate.fl \
                /work/build/pty-s57-embedded-4m.c \
                >/work/lowmem.out 2>/work/lowmem.err &&
               cmp -s /work/lowmem.out \
                /work/tests/script/batch.golden; then
                after=$(sha256sum /work/build/pty-s57-embedded-4m.c |
                    awk '{print $1}')
                if [ "$before" = "$after" ]; then
                    row_pass 12 completed
                else
                    row_fail 12 changed-file
                fi
            else
                row_fail 12 unnamed-refusal
                sed -n '1,80p' /work/lowmem.err
            fi
            update_yew_peaks "$log" lowmem
        fi
    fi
    if oom_free; then
        echo 'YEW_EMBED_OOM status=pass'
    else
        failures=$((failures + 1))
        echo 'YEW_EMBED_OOM status=fail process=yew'
    fi
}

run_full()
{
    if /bin/yew --version >/work/version.out 2>/work/version.err &&
       grep -Fqx 'modules: lsp ai fuss plugins' /work/version.out; then
        row_pass 1 version-modules
    else
        row_fail 1 version-modules
    fi

    if run_pty notepad_open; then
        row_pass 2 first-paint
    else
        row_fail 2 first-paint
    fi

    report_memory before-4m-pty
    if run_pty s57_embedded_4m_roundtrip; then
        row_pass 3 4m-roundtrip
        row_pass 5 syntax-host-golden
    else
        row_fail 3 4m-roundtrip
        row_fail 5 syntax-host-golden
    fi

    if run_pty notepad_burst_keys; then
        row_pass 4 typing-4096-keys
    else
        row_fail 4 typing-4096-keys
    fi

    generate_fixture || true
    before=$(sha256sum /work/build/pty-s57-embedded-4m.c |
        awk '{print $1}')
    log=/work/yew-rss-batch.log
    rm -f "$log"
    sync
    printf '%s\n' 3 >/proc/sys/vm/drop_caches 2>/dev/null || true
    if YEW_PROF=1 YEW_LOG="$log" \
       /bin/yew --batch --test --clean \
        /work/tests/script/57_embedded_gate.fl \
        /work/build/pty-s57-embedded-4m.c \
        >/work/batch.out 2>/work/batch.err &&
       cmp -s /work/batch.out /work/tests/script/batch.golden; then
        after=$(sha256sum /work/build/pty-s57-embedded-4m.c |
            awk '{print $1}')
        if [ "$before" = "$after" ]; then
            row_pass 6 regex-host-golden
            row_pass 7 undo-redo-500
        else
            row_fail 6 changed-file
            row_fail 7 changed-file
        fi
    else
        row_fail 6 regex-host-golden
        row_fail 7 undo-redo-500
        sed -n '1,120p' /work/batch.err
    fi
    update_yew_peaks "$log" batch

    if run_pty s25_resume_exact; then
        row_pass 8 workspace-resume
    else
        row_fail 8 workspace-resume
    fi

    if run_pty s35_macro_record_replay_from_e_mode; then
        row_pass 9 macro-roundtrip
    else
        row_fail 9 macro-roundtrip
    fi

    /bin/yew --selftest-bug >/work/bug.out 2>/work/bug.err
    rc=$?
    esc=$(printf '\033')
    if [ "$rc" -eq 4 ] && [ ! -s /work/bug.out ] &&
       grep -E '^yew: internal error at .*: selftest$' /work/bug.err \
           >/dev/null &&
       grep -Fqx 'yew: please report this internal error' /work/bug.err &&
       ! grep -F "$esc" /work/bug.err >/dev/null; then
        row_pass 10 structured-bug-terminal-clean
    else
        row_fail 10 structured-bug-terminal-clean
    fi

    echo "YEW_EMBED_RSS peak_bytes=$peak_bytes limit_bytes=25165824"
    if [ "$peak_bytes" -le 25165824 ]; then
        row_pass 11 peak-rss
    else
        row_fail 11 peak-rss
    fi
    if oom_free; then
        echo 'YEW_EMBED_OOM status=pass'
    else
        failures=$((failures + 1))
        echo 'YEW_EMBED_OOM status=fail process=yew'
    fi
}

echo "YEW_EMBED_BEGIN mode=$mode"
cd /work || failures=$((failures + 1))
if [ "$mode" = lowmem ]; then
    run_lowmem
elif prepare_external_storage; then
    run_full
else
    failures=$((failures + 1))
fi

if [ "$failures" -eq 0 ]; then
    echo "YEW_EMBED_RESULT mode=$mode status=pass failures=0"
else
    echo "YEW_EMBED_RESULT mode=$mode status=fail failures=$failures"
fi
sync
poweroff -f
