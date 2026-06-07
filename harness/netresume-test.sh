#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# netresume-test.sh — 蓋閉じ(suspend)→開け(resume) でネットワークデバイスが
# どう死に・どう復帰するかを「ローカルファイル」に時系列記録する診断。
#
# ネット/ssh が切れても結果は /var/log に残る。suspend 中はプロセスごと凍結し
# resume で再開、時刻ジャンプ量から resume を自動検知してマーカを打つ。
#
#   # on pomera (installed) — root で nohup 起動
#   doas sh -c 'nohup sh /root/netresume-test.sh >/dev/null 2>&1 & echo PID=$!'
#   # → 蓋を閉じる → しばらく → 蓋を開ける → 1-2分待つ → 結果を読む:
#   doas cat /var/log/netresume-test.log
#   # 停止:
#   doas touch /root/.netresume-stop
#
# 環境変数:
#   NETRESUME_LAN_GW  — L2 疎通確認に ping する LAN GW IP (既定: <your-lan-gw>)
#   NETRESUME_MAX     — 自動終了秒。0 = 無制限
LOG=/var/log/netresume-test.log
INTERVAL=6                       # サンプル間隔(秒)
MAXSEC=${NETRESUME_MAX:-3600}    # 自動終了(秒)。0=無制限。boot 永続化時は大きい値で起動
GAP=20                           # この秒以上の時刻ジャンプを resume(suspend明け)とみなす
STOP=/root/.netresume-stop
LAN_GW=${NETRESUME_LAN_GW:-<your-lan-gw>}   # L2 疎通確認先。LAN router の IP。

ts() { date '+%H:%M:%S'; }

# 簡易タイムアウト実行 (OpenBSD base に timeout(1) が無いため自作)。
run_to() {
	_to=$1; shift
	"$@" & _cp=$!
	( sleep "$_to"; kill -9 "$_cp" 2>/dev/null ) & _wp=$!
	wait "$_cp" 2>/dev/null; _rc=$?
	kill -9 "$_wp" 2>/dev/null
	return $_rc
}

probe() {
	wifi=$(ifconfig bwfm0 2>/dev/null | grep -q 'status: active' && echo active || echo DOWN)
	t0=$(ifconfig tun0 2>/dev/null | grep -q 'status: active' && echo up || echo -)
	t1=$(ifconfig tun1 2>/dev/null | grep -q 'status: active' && echo up || echo -)
	def=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')
	gw=$(ping -c1 -w2 "$LAN_GW" >/dev/null 2>&1 && echo ok || echo NG)         # WiFi L2(GW)
	dns=$(run_to 3 nslookup one.one.one.one >/dev/null 2>&1 && echo ok || echo NG)  # MagicDNS含む
	tcp=$(nc -z -w3 1.1.1.1 443 >/dev/null 2>&1 && echo ok || echo NG)          # L4(IP直)
	tsd=$(run_to 3 tailscale status >/dev/null 2>&1 && echo up || echo DOWN)    # tailscaled
	bt=$(cat /tmp/.bt-state 2>/dev/null || echo '?')                            # netwatchd 判定
	el=$(( $(date +%s) - START ))
	printf '%s T+%ss wifi=%s tun0=%s tun1=%s def=%s | gwping=%s dns=%s tcp1111=%s ts=%s bt=%s\n' \
		"$(ts)" "$el" "$wifi" "$t0" "$t1" "${def:-none}" "$gw" "$dns" "$tcp" "$tsd" "$bt"
}

START=$(date +%s); last=$START
{
	echo "===== netresume-test start $(date '+%Y-%m-%d %H:%M:%S') (interval=${INTERVAL}s max=${MAXSEC}s) ====="
	echo "  boottime=$(sysctl -n kern.boottime 2>/dev/null)  ← 前のセッションと違えば=リブート発生 / 同じなら=resume(箱は生存)"
	echo "  lan_gw=$LAN_GW"
	echo "  読む: doas cat $LOG  /  停止: doas touch $STOP"
} >> "$LOG"

while :; do
	now=$(date +%s)
	gap=$((now - last))
	if [ "$gap" -ge "$GAP" ]; then
		echo "  ===== RESUME 検知: 約 ${gap}s の時刻ジャンプ(=suspend)後に再開 @ $(date '+%H:%M:%S') =====" >> "$LOG"
		dmesg | tail -6 | sed 's/^/    dmesg| /' >> "$LOG"
	fi
	probe >> "$LOG"
	last=$(date +%s)
	[ -f "$STOP" ] && { echo "===== stopped by stopfile $(date '+%H:%M:%S') =====" >> "$LOG"; rm -f "$STOP"; break; }
	[ $((now - START)) -ge "$MAXSEC" ] && { echo "===== max duration reached $(date '+%H:%M:%S') =====" >> "$LOG"; break; }
	sleep "$INTERVAL"
done
