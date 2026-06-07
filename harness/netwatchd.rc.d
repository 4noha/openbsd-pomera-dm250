#!/bin/ksh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# /etc/rc.d/netwatchd — wifi/BT/resume を一括監視する常駐
#
# cold-boot 対策: boot critical バイナリは eMMC /usr/local.boot/ に置く
# (SD=/mnt/sd は noauto で pkg_scripts 時点では未マウントのため)。
daemon="/usr/local.boot/netwatchd.sh"

. /etc/rc.d/rc.subr

pexp="/bin/sh ${daemon}"
rc_reload=NO
rc_bg=YES

rc_cmd $1
