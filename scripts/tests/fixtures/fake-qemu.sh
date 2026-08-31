#!/bin/sh

set -eu

scenario=${FAKE_QEMU_SCENARIO:-full}
case $scenario in
    full|rss|missing)
        echo 'YEW_EMBED_BEGIN mode=full'
        row=1
        while [ "$row" -le 11 ]; do
            if [ "$scenario" != missing ] || [ "$row" -ne 5 ]; then
                echo "YEW_EMBED_ROW row=$row status=pass detail=fake"
            fi
            row=$((row + 1))
        done
        if [ "$scenario" = rss ]; then peak=26000000; else peak=20000000; fi
        echo "YEW_EMBED_RSS peak_bytes=$peak limit_bytes=25165824"
        echo 'YEW_EMBED_OOM status=pass'
        echo 'YEW_EMBED_RESULT mode=full status=pass failures=0'
        ;;
    lowmem)
        echo 'YEW_EMBED_BEGIN mode=lowmem'
        echo 'YEW_EMBED_ROW row=12 status=refused detail=memory-preflight'
        echo 'YEW_EMBED_OOM status=pass'
        echo 'YEW_EMBED_RESULT mode=lowmem status=pass failures=0'
        ;;
    *) exit 7 ;;
esac
