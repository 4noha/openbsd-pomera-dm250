#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
# build_panctl.sh — pomera 上で panctl-h4 をネイティブビルド。
#
# 前提:
#   - BT=$HOME/btstack に BTstack v1.6.2 が展開済み (fetch-btstack.sh 経由 or
#     `git clone --depth 1 -b v1.6.2 https://github.com/bluekitchen/btstack`)
#   - patches/openbsd-rk3128/btstack-v1.6.2-openbsd-compat.patch 適用済み
#   - panctl/ 配下のソース (frame.c mux.c ipv4.c divert.c main.c) が居る
#     (このリポの wake_android_tether/btstack/panctl/ から rsync 等で持ってくる)
#
# 出力:
#   /tmp/panctl  (約 600KB の単一バイナリ)
set -e

BT=${BT:-$HOME/btstack}
PANCTL=${PANCTL:-$HOME/panctl}

INCS="-I$PANCTL \
  -I$BT/port/posix-h4 -I$BT/src -I$BT/src/ble -I$BT/src/classic \
  -I$BT/platform/posix -I$BT/platform/embedded \
  -I$BT/chipset/bcm \
  -I$BT/3rd-party/rijndael -I$BT/3rd-party/tinydir -I$BT/3rd-party/micro-ecc"

CFLAGS="-O -Wall -Werror=implicit-function-declaration \
  -DHAVE_BTSTACK=1 \
  -DBTSTACK_FILE__=__FILE__"

# BTstack core (filter audio/mesh/不要 profile)
CORE=""
for f in $BT/src/*.c; do
    case $f in
    *sample_rate_compensation*|*lc3_google*) ;;
    *) CORE="$CORE $f" ;;
    esac
done
for f in $BT/src/ble/*.c; do CORE="$CORE $f"; done
for f in $BT/src/classic/*.c; do
    case $f in
    *sbc*|*pbap*|*hsp*|*hfp*|*hid_*|*goep*|*a2dp*|*avdtp*|*avrcp*|*obex*|*opp*|*map*|*bnep*) ;;
    *) CORE="$CORE $f" ;;
    esac
done

PLAT="$BT/platform/posix/btstack_run_loop_posix.c \
      $BT/platform/posix/btstack_uart_posix.c \
      $BT/platform/posix/btstack_signal.c \
      $BT/platform/posix/hci_dump_posix_fs.c \
      $BT/platform/posix/btstack_tlv_posix.c"

CHIP="$BT/chipset/bcm/btstack_chipset_bcm.c"
THIRD="$BT/3rd-party/rijndael/rijndael.c $BT/3rd-party/micro-ecc/uECC.c"

PANCTL_SRC="$PANCTL/frame.c $PANCTL/mux.c $PANCTL/ipv4.c $PANCTL/divert.c $PANCTL/ctl.c $PANCTL/tun_tcp.c $PANCTL/main.c"

NTOT=$(echo $CORE $PLAT $CHIP $THIRD $PANCTL_SRC | wc -w)
echo "=== compiling $NTOT files (panctl-h4) ==="
cc $CFLAGS $INCS \
    $CORE $PLAT $CHIP $THIRD $PANCTL_SRC \
    -lpthread \
    -o /tmp/panctl 2>&1 | grep -E "error:|warning:.*declaration|warning:.*incompatible" | head -30
echo "panctl exit=$?"
ls -la /tmp/panctl 2>&1

# panctlctl is small and standalone (no BTstack deps).
TOOLS=${TOOLS:-$HOME/tools}
if [ -f "$TOOLS/panctlctl.c" ]; then
    echo "=== compiling panctlctl ==="
    cc -O -Wall -o /tmp/panctlctl "$TOOLS/panctlctl.c" 2>&1 | head -10
    echo "panctlctl exit=$?"
    ls -la /tmp/panctlctl 2>&1
fi
