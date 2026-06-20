#!/bin/sh
# qemu-prep-sd.sh — drive qemu-system-aarch64 to prep the DM250 install SD.
#
# Boots install79.img (OpenBSD/arm64 installer), drops to shell, fetches
# prep-sd.sh from a host-side HTTP server, and runs it against the SD card
# passed through as /dev/disk<N>. Requires sudo (qemu raw disk access).
#
# Usage:
#   sudo SD_DEV=/dev/disk4 ./qemu-prep-sd.sh
#
set -eu

WORK=${WORK:-"$(cd "$(dirname "$0")" && pwd)"}
SD_DEV="${SD_DEV:-/dev/disk4}"
EDK=/opt/homebrew/share/qemu/edk2-aarch64-code.fd
INSTALL_IMG="$WORK/arm64-snapshot/install79.img"

LOG=/tmp/dm250-qemu.log
SERIAL_SOCK=/tmp/dm250-qemu-serial.sock
MONITOR_SOCK=/tmp/dm250-qemu-monitor.sock
HTTP_PORT=8000

die() { printf 'ERR: %s\n' "$*" >&2; exit 1; }

[ -e "$SD_DEV" ] || die "SD_DEV=$SD_DEV not present"
[ -f "$INSTALL_IMG" ] || die "install image missing: $INSTALL_IMG"
[ -f "$EDK" ] || die "EDK2 firmware missing: $EDK"
command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 not on PATH"
command -v python3 >/dev/null || die "python3 missing"

# Resolve the real user for HTTP server (so the file server isn't running as root).
REAL_USER="${SUDO_USER:-$USER}"

rm -f "$SERIAL_SOCK" "$MONITOR_SOCK" "$LOG"

# Make sure the SD is fully unmounted (macOS auto-mounts FAT volumes).
diskutil unmountDisk "$SD_DEV" || true

cleanup() {
    echo
    echo "[cleanup] killing children"
    [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "${HTTP_PID:-}" ] && kill "$HTTP_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$SERIAL_SOCK" "$MONITOR_SOCK"
}
trap cleanup EXIT INT TERM

# HTTP server (loopback only; qemu user-mode networking reaches 10.0.2.2=host loopback).
echo "[runner] HTTP server on 127.0.0.1:$HTTP_PORT serving $WORK"
sudo -u "$REAL_USER" python3 -m http.server "$HTTP_PORT" \
    --bind 127.0.0.1 \
    --directory "$WORK" \
    >/tmp/dm250-http.log 2>&1 &
HTTP_PID=$!

# qemu runs as root because /dev/disk* needs it.
echo "[runner] starting qemu (passthrough SD=$SD_DEV)"
qemu-system-aarch64 \
    -machine virt,accel=hvf \
    -cpu host \
    -m 2G -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$EDK" \
    -drive file="$INSTALL_IMG",format=raw,if=virtio,readonly=on \
    -drive file="$SD_DEV",format=raw,if=virtio,cache=none \
    -netdev user,id=net0 \
    -device virtio-net,netdev=net0 \
    -display none \
    -serial unix:"$SERIAL_SOCK",server,nowait \
    -monitor unix:"$MONITOR_SOCK",server,nowait \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!
echo "[runner] qemu PID=$QEMU_PID, HTTP PID=$HTTP_PID"

# Wait for serial socket
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    [ -S "$SERIAL_SOCK" ] && break
    sleep 1
done
[ -S "$SERIAL_SOCK" ] || die "qemu serial socket never appeared; see $LOG"

# Drive
echo "[runner] running driver.py"
python3 "$WORK/driver.py"

# qemu should halt itself via halt -p; wait briefly.
echo "[runner] waiting for qemu to exit..."
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 1
done

echo "[runner] done."
