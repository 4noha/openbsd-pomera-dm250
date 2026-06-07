#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# build-deploy-logo.sh — Build a DM250 boot logo (and optionally deploy to the device).
#
# What it does
#   1. Extract the factory "pomera" wordmark (logo.bmp) from the user's own
#      eMMC backup — specifically the Rockchip RSCE resource partition image
#      (mmcblk0p3.img, offset 76288, length 44072).
#   2. Compose it with the OpenBSD puffy logo distributed under install/
#      (1024x600 black background, pomera wordmark centred).
#   3. Optionally place a user-supplied mascot image (--mascot) next to puffy.
#   4. Convert the result to a 24-bit Windows 3.x BMP that the u-boot logo
#      loader can consume.
#   5. Optionally scp the BMP to the device's EFI partition (/dev/sd1i) at
#      /logo.bmp.
#
# Licensing stance
#   - The "pomera" wordmark (KING JIM Co., Ltd.) is **re-extracted from the
#     user's own DM250 eMMC backup on every run**. It is NOT bundled in this
#     repo and is NOT redistributed over the network. Each individual user
#     reads their own bytes from their own hardware.
#   - The puffy logo originates from jcs.org/dm250 (Joshua Stein's DM250
#     project) and is committed under install/files/dm250/logo.bmp; its
#     provenance is documented alongside that file.
#   - The mascot image is optional. The user supplies an image they have the
#     rights to (or one with a compatible licence). This script only takes a
#     path — the image is scp'd once and is never committed.
#   - The composed logo-deploy-1024x600.bmp is a derivative work. Keep it on
#     your personal device for personal use; do not publish.
#
# Usage
#   build-deploy-logo.sh                                  # puffy only, no deploy
#   build-deploy-logo.sh --mascot /path/to/mascot.png     # with mascot, no deploy
#   build-deploy-logo.sh --mascot ... --deploy-host <pomera-host>             # via Tailscale magic DNS (recommended)
#   build-deploy-logo.sh --mascot ... --deploy-host <your-pomera-user>@<dm250-lan-ip>  # same LAN subnet only
#
# Prerequisites
#   - macOS + ImageMagick (`brew install imagemagick`)
#   - At least one eMMC backup directory of the form
#     ~/Backups/pomera-dm250-emmc-YYYYMMDD/ containing mmcblk0p3.img.
#     Override with --backup-dir.
#   - For --deploy-host: an ssh key that already authenticates to the target,
#     and on the remote side: doas (wheel group) plus /dev/sd1i (EFI msdosfs).
set -eu

# Project root layout (this script lives in <repo>/logo/). The puffy logo is
# shipped under install/files/dm250/logo.bmp; adjust if your checkout layout
# differs.
PROJ=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PROJ/.." && pwd)
OUT=$PROJ/out
PUFFY=$REPO/install/files/dm250/logo.bmp

BACKUP_DIR=""
MASCOT=""
DEPLOY=""

usage() {
    cat <<EOF
usage: $0 [--backup-dir DIR] [--mascot IMAGE] [--deploy-host USER@HOST]

  --backup-dir DIR     eMMC backup directory containing mmcblk0p3.img.
                       Default: latest ~/Backups/pomera-dm250-emmc-* match.
  --mascot IMAGE       Optional mascot image (png/jpg, any resolution; scaled
                       to 142px). Default: puffy only, bottom-right centred.
  --deploy-host U@H    ssh target to write the BMP onto the device
                       (e.g. <your-pomera-user>@<dm250-lan-ip>).
                       Default: build the BMP locally, no deploy.
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --backup-dir)  BACKUP_DIR="$2"; shift 2 ;;
        --mascot)      MASCOT="$2"; shift 2 ;;
        --deploy-host) DEPLOY="$2"; shift 2 ;;
        -h|--help)     usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

# --- Prerequisite checks --------------------------------------------------
command -v magick >/dev/null || { echo "ERR: imagemagick missing (brew install imagemagick)" >&2; exit 1; }
[ -f "$PUFFY" ] || { echo "ERR: puffy missing at $PUFFY (fetch install/files/dm250/ first)" >&2; exit 1; }

# Auto-detect a backup directory.
if [ -z "$BACKUP_DIR" ]; then
    BACKUP_DIR=$(ls -td "$HOME/Backups/pomera-dm250-emmc-"* 2>/dev/null | head -1)
    if [ -z "$BACKUP_DIR" ]; then
        echo "ERR: no ~/Backups/pomera-dm250-emmc-* directory found." >&2
        echo "     Run the backup procedure from install/ first, or pass" >&2
        echo "     --backup-dir explicitly." >&2
        exit 1
    fi
fi
[ -f "$BACKUP_DIR/mmcblk0p3.img" ] || {
    echo "ERR: mmcblk0p3.img not found in $BACKUP_DIR" >&2
    exit 1
}

mkdir -p "$OUT"

# --- 1. Extract logo.bmp from the RSCE partition --------------------------
echo "[1/5] extract pomera logo from $BACKUP_DIR/mmcblk0p3.img"
dd if="$BACKUP_DIR/mmcblk0p3.img" of="$OUT/logo.bmp" bs=1 skip=76288 count=44072 2>/dev/null
SIZE1=$(wc -c < "$OUT/logo.bmp" | tr -d ' ')
[ "$SIZE1" = 44072 ] || { echo "ERR: extracted size $SIZE1 != 44072 (RSCE offset may be wrong)" >&2; exit 1; }
echo "      -> $OUT/logo.bmp ($SIZE1 bytes)"

# --- 2. 500x44 PNG (for HTML preview / composition; drop alpha) -----------
echo "[2/5] convert logo.bmp to 500x44 PNG for compose"
magick "$OUT/logo.bmp" -alpha off "$OUT/logo-500x44.png" 2>/dev/null

# --- 3. 1024x600 composition ----------------------------------------------
echo "[3/5] compose 1024x600 mock"
if [ -n "$MASCOT" ]; then
    [ -f "$MASCOT" ] || { echo "ERR: mascot file not found: $MASCOT" >&2; exit 1; }
    echo "      mascot: $MASCOT"
    magick "$MASCOT" -resize x142 "$OUT/mascot-142x142.png" 2>/dev/null

    # Bottom-right: [puffy 166x142] [16px gap] [mascot 142x142] [32px from edge]
    magick -size 1024x600 xc:black \
        \( "$OUT/logo.bmp" -alpha off \) -gravity center -composite \
        \( "$PUFFY" -alpha off \) -gravity SouthEast -geometry +190+32 -composite \
        \( "$OUT/mascot-142x142.png" \) -gravity SouthEast -geometry +32+32 -composite \
        "$OUT/boot-screen-mock-1024x600.png" 2>/dev/null
else
    echo "      no mascot (puffy only)"
    magick -size 1024x600 xc:black \
        \( "$OUT/logo.bmp" -alpha off \) -gravity center -composite \
        \( "$PUFFY" -alpha off \) -gravity SouthEast -geometry +32+32 -composite \
        "$OUT/boot-screen-mock-1024x600.png" 2>/dev/null
fi

# --- 4. Convert to 24-bit Windows 3.x BMP (u-boot logo loader format) -----
echo "[4/5] convert to 24-bit Win 3.x BMP"
magick "$OUT/boot-screen-mock-1024x600.png" -define bmp:format=bmp3 -depth 8 "$OUT/logo-deploy-1024x600.bmp" 2>/dev/null
SIZE2=$(wc -c < "$OUT/logo-deploy-1024x600.bmp" | tr -d ' ')
MD5=$(md5 -q "$OUT/logo-deploy-1024x600.bmp")
echo "      -> $OUT/logo-deploy-1024x600.bmp ($SIZE2 bytes, md5=$MD5)"

# --- 5. Optional deploy ---------------------------------------------------
if [ -n "$DEPLOY" ]; then
    echo "[5/5] deploy to $DEPLOY (scp + EFI mount)"
    scp -q "$OUT/logo-deploy-1024x600.bmp" "$DEPLOY:/tmp/logo.bmp"
    ssh "$DEPLOY" 'set -e
        doas mount -t msdos /dev/sd1i /mnt
        echo "    before: $(doas md5 /mnt/logo.bmp 2>/dev/null || echo missing)"
        doas cp /tmp/logo.bmp /mnt/logo.bmp
        echo "    after:  $(doas md5 /mnt/logo.bmp)"
        doas umount /mnt' || {
        echo "ERR: remote deploy failed; check ssh / doas / /dev/sd1i" >&2
        exit 1
    }
    echo "      deployed. reboot to see the new boot screen."
else
    echo "[5/5] no --deploy-host; BMP available at $OUT/logo-deploy-1024x600.bmp"
fi

echo "done."
