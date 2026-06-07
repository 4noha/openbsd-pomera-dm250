#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# /mnt/sd (SD) を冪等にマウントし /usr/local/lib の ld.so hints を回復する。
# /etc/rc.local (boot) と /etc/hotplug/attach (hot-insert) の両方から呼ぶ。
# 旧構成では rc.local と hotplug が同じ sd0a を同時 fsck して mount 失敗していた。
# mkdir ロックで直列化し、fsck/mount を 1 回だけ走らせる。
SD_DEV=/dev/sd0a
LOCK=/var/run/mount-sd.lock

already() { mount | grep -q " /mnt/sd "; }

if already; then ldconfig -m /usr/local/lib >/dev/null 2>&1; exit 0; fi

# mkdir は atomic。他の呼び出しが処理中なら待つ間に mounted になり次第抜ける。
i=0
while ! mkdir "$LOCK" 2>/dev/null; do
    already && { ldconfig -m /usr/local/lib >/dev/null 2>&1; exit 0; }
    i=$((i+1)); [ $i -ge 90 ] && break; sleep 1
done
trap "rmdir \"$LOCK\" 2>/dev/null" EXIT

if ! already; then
    # preen で dirty FS を修復。preen が無理な不整合は -y で強制。
    fsck -p "$SD_DEV" >/dev/null 2>&1 || fsck -y "$SD_DEV" >/dev/null 2>&1
    i=0
    until mount /mnt/sd >/dev/null 2>&1; do
        i=$((i+1)); [ $i -ge 15 ] && break; sleep 1
    done
fi

if already; then
    ldconfig -m /usr/local/lib >/dev/null 2>&1
    logger -t mount-sd "mounted /mnt/sd + ldconfig /usr/local/lib"
else
    logger -t mount-sd "FAILED to mount /mnt/sd"
fi
