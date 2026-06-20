#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# wifi-suspend-test.sh — pomera DM250 (OpenBSD/armv7) の「WiFi up のまま suspend
# すると resume でカーネル crash (bwfm net80211 stale 参照)」を、クラッシュや
# reboot を生き残る永続ログで切り分ける harness。
#
# ログは eMMC root fs 上の /var/log/wifi-suspend-test.log。各書込み後に sync(8)
# で flush するので、suspend 中に kernel が panic/reboot しても直前の状態が残る。
#
# !!! このタスクでは絶対に実 suspend を起こさない !!!
#   apmsuspend を呼ぶ行は用意してあるが、--go を付けない限り呼ばない。
#   既定 (引数なし or "dry") は「状態採取 -> sync -> ARMED マーカ -> sync」までで
#   止まり、suspend ステップは echo するだけ。
#
# 使い方 (on pomera, installed):
#   doas sh /root/wifi-suspend-test.sh dry      # ドライラン (suspend しない / 既定)
#   doas sh /root/wifi-suspend-test.sh --go      # 本番 (実 suspend。今回は使わない)
#   doas sh /root/wifi-suspend-test.sh boot-hook # boot 時フック (rc.local から)
#
# 主要パス:
LOG=/var/log/wifi-suspend-test.log
STATE=/var/db/wifi-suspend-test.state   # boot 間で持ち越す ARMED/boottime 記録
APMSUSPEND=/usr/local.boot/apmsuspend   # 自前 suspend トリガ (別ビルド)

now_human() { date '+%Y-%m-%d %H:%M:%S'; }

# boottime を epoch 整数で。これが前回と変われば「reboot 発生」。
boottime_epoch() { sysctl -n kern.boottime 2>/dev/null; }

log() { printf '%s\n' "$*" >> "$LOG"; }

# 書いて即 flush。crash を生き残らせる肝。
flush() { sync; }

# ---- 状態採取: SEQ / uptime / boottime / ifconfig bwfm0 / dmesg tail ----
capture() {
	_tag=$1
	log "----- [$_tag] $(now_human) -----"
	log "  seq=${SEQ:-?}  pid=$$"
	log "  uptime: $(uptime 2>/dev/null)"
	log "  boottime_epoch=$(boottime_epoch)  ($(sysctl -n kern.boottime 2>/dev/null))"
	log "  ifconfig bwfm0:"
	ifconfig bwfm0 2>&1 | sed 's/^/    | /' >> "$LOG"
	log "  dmesg tail:"
	dmesg | tail -6 | sed 's/^/    | /' >> "$LOG"
	capture_firmware_reachability
	flush
}

# firmware load fail (bwfm0: failed loadfirmware of file ...) の真因切り分け用。
# bwfm の resume kthread が動く瞬間に /etc/firmware/* が VFS から読めるか確認する。
# eMMC sd1 (rootfs) が resume で force-detach されて再 attach 待ちなら、ここで
# 1) /etc/firmware ディレクトリの readdir が EIO/ENOENT
# 2) brcmfmac*.bin の dd 読み出しが失敗 (短時間 read)
# 3) sd1 が disklabel で見えるが mount/df は古いマウント情報のまま
# のいずれかが観測されるはず。各サンプル時点で fsync 込みで残す。
capture_firmware_reachability() {
	_fwbin=/etc/firmware/brcmfmac43430-sdio.rockchip,pomera-dm250.bin
	_fwfallback=/etc/firmware/brcmfmac43430-sdio.bin
	log "  firmware reachability:"
	# mount 状態 (sd1a=rootfs / sd0a=SDcard)
	log "    mount sd0/sd1   :"
	mount 2>/dev/null | grep -E '/dev/sd[01]' | sed 's/^/      | /' >> "$LOG"
	# /etc/firmware/ readdir (root fs 上のディレクトリエントリ)
	log "    /etc/firmware ls :"
	ls -la /etc/firmware/ 2>&1 | head -8 | sed 's/^/      | /' >> "$LOG"
	# 実際のファイル read: 先頭 64B を dd で読む。これが ENOENT/EIO なら鍵。
	for _f in "$_fwbin" "$_fwfallback"; do
		if [ -e "$_f" ]; then
			_first=$(dd if="$_f" bs=64 count=1 2>/dev/null | wc -c 2>/dev/null || echo X)
			_size=$(ls -l "$_f" 2>/dev/null | awk '{print $5}')
			log "    read $_f : first_64B_bytes=${_first} size=${_size:-?}"
		else
			log "    read $_f : MISSING"
		fi
	done
	# bwfm0 ifnet 状態と sd0/sd1 disk node 存在
	log "    /dev/sd0c    : $([ -c /dev/rsd0c ] && echo present || echo absent)"
	log "    /dev/sd1c    : $([ -c /dev/rsd1c ] && echo present || echo absent)"
	# 直近 dmesg から firmware/init bus エラーを抽出 (一発で見える)
	_fwerr=$(dmesg 2>/dev/null | grep -E 'failed loadfirmware|could not init bus|cannot enable function|sdmmc[01]:|bwfm0:' | tail -8)
	if [ -n "$_fwerr" ]; then
		log "    dmesg fw/bus signals:"
		printf '%s\n' "$_fwerr" | sed 's/^/      | /' >> "$LOG"
	fi
}

# ---- resume / reboot 後の判定 + 採取 ----
# boottime が ARM 時と同じ  -> 箱は生存 (clean resume の可能性)
# boottime が ARM 時と違う  -> reboot した (crash-reboot の可能性大)
# dmesg に "error 60" があれば bwfm の resume コマンド timeout の痕跡。
#
# bwfm 再 attach は sdmmc task thread が detach -> sdmmc_needs_discover ->
# bwfm_sdio_attach -> 別 kthread で bwfm_sdio_attachhook (firmware load) と
# 非同期に進む。resume 直後だけ採取すると「まだ if_attach に到達してない」
# だけかもしれないので T+0/3/10/30s で連続キャプチャして時間軸を作る。
post_resume_capture() {
	_prev_boot=$1
	_label=$2
	_cur_boot=$(boottime_epoch)
	log "===== POST-RESUME CHECK ($_label) $(now_human) ====="
	if [ "$_prev_boot" = "$_cur_boot" ]; then
		log "  VERDICT: boottime 不変 ($_cur_boot) -> 箱は生存 = RESUME (no reboot)"
	else
		log "  VERDICT: boottime 変化 ($_prev_boot -> $_cur_boot) -> REBOOT が発生した"
	fi
	if dmesg | grep -q 'error 60'; then
		log "  bwfm: dmesg に 'error 60' あり -> resume コマンド timeout の痕跡"
	else
		log "  bwfm: dmesg に 'error 60' なし"
	fi
	# bwfm0 ifnet 存在 / 状態のフル採取 (grep でなく全文)
	log "  ifconfig bwfm0 (full):"
	ifconfig bwfm0 2>&1 | sed 's/^/    | /' >> "$LOG"
	# resume 後に何が wedge するかの診断 (SD/usr_local detach? netwatchd? wsdisplay?)
	# ※ mount/pgrep/dmesg は SD の I/O を触らないので、SD detach 中でも hang しない
	log "  /mnt/sd mounted : $(mount 2>/dev/null | grep -q ' /mnt/sd ' && echo yes || echo NO)"
	log "  default route   : $(route -n get default 2>/dev/null | awk '/interface:/{print $2}')"
	log "  netwatchd       : $(pgrep -f netwatchd.sh >/dev/null 2>&1 && echo alive || echo dead)"
	log "  panctl          : $(pgrep -x panctl >/dev/null 2>&1 && echo alive || echo dead)"
	log "  resume dmesg tail (sd0/scsibus/bwfm/rkdrm/wskbd 再attach を見る):"
	dmesg | tail -30 | sed 's/^/    | /' >> "$LOG"
	# BWFMD = bwfm-attach-diag printfs (v3-diag kernel のみ存在)。
	# bwfm_sdio_attach / preinit がどの番号まで進んだかが直接見える。
	log "  BWFMD trace (bwfm-attach-diag printfs since boot):"
	dmesg | grep '^BWFMD ' | tail -40 | sed 's/^/    | /' >> "$LOG"
	# post-wake region (rk3128_suspend: woke from irq の後の dmesg)。
	# detach/re-attach の前後のキー行を取りこぼさないように 60 行まで広げる。
	log "  post-wake dmesg region:"
	dmesg | awk '/rk3128_suspend: woke from irq/{p=1} p' | tail -60 \
		| sed 's/^/    | /' >> "$LOG"
	flush
}

# resume 後の bwfm 再 attach は非同期 kthread 経路。
# T+0..300s で 7 回採取して時系列で「いつ ifnet が現れるか / 現れないか」を見る。
# 5 分待っても来ないなら本当に止まってる; どこかで来るなら遅いだけ。
post_resume_capture_series() {
	_prev_boot=$1
	post_resume_capture "$_prev_boot" "T+0s"
	sleep 3
	post_resume_capture "$_prev_boot" "T+3s"
	sleep 7
	post_resume_capture "$_prev_boot" "T+10s"
	sleep 20
	post_resume_capture "$_prev_boot" "T+30s"
	sleep 30
	post_resume_capture "$_prev_boot" "T+60s"
	sleep 120
	post_resume_capture "$_prev_boot" "T+180s"
	sleep 120
	post_resume_capture "$_prev_boot" "T+300s"
}

# ---- boot フック: rc.local から呼ぶ。前回 ARMED があったか & 今回 boot が
#      新しいか(=crash-reboot か)を STATE と boottime で判定して追記する ----
boot_hook() {
	_cur_boot=$(boottime_epoch)
	log "===== BOOT $(now_human) (boottime_epoch=$_cur_boot) ====="
	if [ -f "$STATE" ]; then
		# STATE 形式: "ARMED <armed_boottime> <armed_human>"
		read _kw _armed_boot _rest < "$STATE"
		if [ "$_kw" = "ARMED" ]; then
			if [ "$_armed_boot" = "$_cur_boot" ]; then
				log "  RESUME: 前回 ARMED の boottime と一致 -> 箱は再起動せず resume した (clean resume)"
			else
				log "  CRASH-REBOOT: 前回 ARMED (boottime=$_armed_boot, $_rest) のあと boottime が変わった -> suspend/resume で crash して再起動した疑い"
			fi
		else
			log "  BOOT: state ファイルはあるが ARMED ではない ($_kw)"
		fi
		# 判定済みなので ARMED マーカは解除 (次の重複判定を防ぐ)
		rm -f "$STATE"
	else
		log "  BOOT: 直前に ARMED 記録なし -> 通常 boot"
	fi
	flush
}

# 6-trial paired experiment用の追加状態採取 (workflow 推奨)
# - sysctl ddb.panic — panic 時に ddb 行きか自動 reboot か
# - default route iface — bwfm0 / tun0 / bcmbt のどれが default か (環境差仮説 #3)
# - ifconfig bcmbt — BT 状態 (QUIESCE 時 com0 contention 仮説)
# - sha256 /bsd — どのカーネルバイナリで走ってるかの裏取り
capture_extra_for_trial() {
	log "  TRIAL state:"
	log "    label              : ${LABEL:-(none)}"
	log "    sysctl ddb.panic   : $(sysctl -n ddb.panic 2>/dev/null)"
	log "    default route      : $(route -n get default 2>/dev/null \
		| awk '/interface:/{print $2}')"
	log "    ifconfig bcmbt     :"
	ifconfig bcmbt 2>&1 | sed 's/^/      | /' >> "$LOG"
	log "    kernel sha256 /bsd : $(doas sha256 -q /bsd 2>/dev/null)"
	flush
}

# =====================================================================
case "${1:-dry}" in
boot-hook)
	boot_hook
	exit 0
	;;
esac

# 互換: 第1引数が --label LABEL なら label を取って残りをずらす。
LABEL=""
if [ "$1" = "--label" ] && [ -n "$2" ]; then
	LABEL="$2"
	shift 2
fi

MODE="${1:-dry}"   # dry (既定) | --go
SEQ=$(date '+%s')

log ""
log "##### wifi-suspend-test RUN start $(now_human) mode=$MODE label=${LABEL:-none} #####"
flush

# (a-pre) trial 用の追加状態採取 (label が付いてるときだけ)
[ -n "$LABEL" ] && capture_extra_for_trial

# (a) suspend 前の状態採取 + sync
capture "PRE-SUSPEND"

# (b) ARMED マーカを log と STATE 両方に書いて sync。
#     STATE は boot フックが「直前に suspend を仕掛けたか」を知るために使う。
_armed_boot=$(boottime_epoch)
log "ARMED $(now_human) (boottime_epoch=$_armed_boot)  <- ここで suspend を仕掛けた印"
printf 'ARMED %s %s\n' "$_armed_boot" "$(now_human)" > "$STATE"
flush

# (c) suspend ステップ。!!! 既定では絶対に呼ばない !!!
#     --go のときだけ apmsuspend --go を実行する。今回のタスクでは --go は使わない。
if [ "$MODE" = "--go" ]; then
	log "GOING-TO-SUSPEND $(now_human) -> exec $APMSUSPEND --go"
	flush
	# ↓ これが実 suspend を起こす唯一の行。今回は到達しない。
	doas "$APMSUSPEND" --go
	# resume したらここに戻る
	log "BACK-FROM-SUSPEND $(now_human)"
	flush
	post_resume_capture_series "$_armed_boot"
	# clean に戻れたので ARMED は解除 (boot フックに crash と誤判定させない)
	rm -f "$STATE"; flush
else
	# ---- ドライラン: suspend を起こさない ----
	log "DRY-RUN $(now_human): suspend はスキップ (apmsuspend --go を呼ばない)"
	log "  (本番なら: doas $APMSUSPEND --go  を実行する行に到達する)"
	flush
	# ドライランでは STATE を残すと次 boot で誤って CRASH-REBOOT 判定される。
	# 検証目的なので即解除しておく。
	rm -f "$STATE"; flush
	log "DRY-RUN done $(now_human): ARMED state cleared, no suspend issued"
	flush
fi

log "##### wifi-suspend-test RUN end $(now_human) #####"
flush
