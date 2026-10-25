#!/bin/bash
set -u

LOG=$(mktemp /tmp/kumos-smoke-XXXXXX.log)
DISK=$(mktemp /tmp/kumos-smoke-disk-XXXXXX.img)
EXT2=$(mktemp /tmp/kumos-smoke-ext2-XXXXXX.img)
cp disk.img "$DISK"
cp ext2.img "$EXT2"

timeout 20s qemu-system-x86_64 \
    $([ -r /dev/kvm ] && echo "-enable-kvm") \
    -boot order=d -cdrom kumos.iso \
    -drive file="$DISK",format=raw,if=ide -drive file="$EXT2",format=raw,if=ide \
    -m 128M -vga std -display none -serial file:"$LOG" -no-reboot >/dev/null 2>&1

rm -f "$DISK" "$EXT2"

if grep -q "\[boot\] GUI started" "$LOG"; then
    echo "PASS: kernel booted to the desktop"
    if grep -qi "panic\|triple fault\|\[fault\]" "$LOG"; then
        echo "WARN: boot log also contains a fault/panic line, check $LOG"
        exit 1
    fi
    rm -f "$LOG"
    exit 0
else
    echo "FAIL: kernel did not reach the desktop within 20s"
    echo "--- boot log ($LOG) ---"
    cat "$LOG"
    exit 1
fi
