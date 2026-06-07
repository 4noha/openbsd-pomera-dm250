#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
set -e
BT=/home/<your-pomera-user>/btstack
INCS="-I$BT/port/posix-h4 -I$BT/src -I$BT/src/ble -I$BT/src/classic -I$BT/platform/posix -I$BT/platform/embedded -I$BT/chipset/bcm -I$BT/3rd-party/rijndael -I$BT/3rd-party/tinydir -I$BT/3rd-party/micro-ecc"
CFLAGS="-O -Wall -Werror=implicit-function-declaration"

# minimum BTstack core (filter out audio/xml/mesh + the file that pulls in btstack.h)
CORE=""
for f in $BT/src/*.c; do
    case $f in
    *sample_rate_compensation*|*lc3_google*) ;;  # extra exclusions
    *sample_rate_compensation*) ;;  # pulls in btstack.h → sbc/lc3
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
      $BT/platform/posix/hci_dump_posix_fs.c"

CHIP="$BT/chipset/bcm/btstack_chipset_bcm.c"
THIRD="$BT/3rd-party/rijndael/rijndael.c $BT/3rd-party/micro-ecc/uECC.c"

NTOT=$(echo $CORE $PLAT $CHIP $THIRD /tmp/btstack_h4_smoke.c | wc -w)
echo "=== compiling $NTOT files ==="
cc $CFLAGS $INCS \
    $CORE $PLAT $CHIP $THIRD /tmp/btstack_h4_smoke.c \
    -lpthread \
    -o /tmp/btstack_h4_smoke 2>&1 | grep -E "error|^/[a-z]" | head -30
ls -la /tmp/btstack_h4_smoke 2>&1
