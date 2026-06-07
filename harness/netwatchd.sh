#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# /usr/local/sbin/netwatchd.sh — pomera DM250 ネット監視 + LED 制御常駐
#
# やること:
#   1. resume 検出 (sleep が想定より長かった = 蓋閉じスリープから復帰)
#      → bwfm0 を down/up + panctl を restart
#   2. 定期的に bwfm0 link 確認、 落ちてたら down/up で再 join
#   3. panctl が落ちてたら自動再起動
#   4. panctl の log を見て BT が疎通中か判定、 /tmp/.bt-state に "up"/"down" を書く
#   5. パイロット LED (gpio1: red_led / green_led) を電源状態で制御:
#        charging  & SOC<99% → orange (red ON + green ON)
#        charging  & SOC≥99% → green
#        full              → green
#        discharging & SOC≤LOW_BAT → red
#        それ以外            → off
#
# 設定は /etc/netwatchd.conf があれば source される。
# LED 制御のため /etc/rc.securelevel で gpio1 pin 8/12 を open しておく必要あり。

set -u
trap '' HUP

WIFI_IF=bwfm0
POLL=5
RESUME_THRESHOLD=10
WIFI_DOWN_RETRY=15
PANCTL_ENABLED=1
BT_STATE_FILE=/tmp/.bt-state
LED_ENABLED=1
LOW_BAT=15           # %; これ以下で 要充電 (red)
FULL_BAT=99          # %; これ以上で 充電完了 (green)

[ -r /etc/netwatchd.conf ] && . /etc/netwatchd.conf

log() { logger -t netwatchd -p daemon.info "$*"; }

wifi_up() {
    ifconfig "$WIFI_IF" down 2>/dev/null
    sleep 1
    ifconfig "$WIFI_IF" up 2>/dev/null
    for _try in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        ifconfig "$WIFI_IF" 2>/dev/null | grep -q "status: active" && return 0
    done
    log "wifi_up: still no link after 10s, kick dhclient"
    pkill -f "dhclient.*$WIFI_IF" 2>/dev/null
    sleep 1
    dhclient "$WIFI_IF" >/dev/null 2>&1 &
}

is_wifi_active() {
    ifconfig "$WIFI_IF" 2>/dev/null | grep -q "status: active"
}

is_panctl_running() {
    # パス非依存で検知する。 panctl のバイナリは cold-boot 対策で
    # /usr/local.boot/ (eMMC) に置かれるため、 旧 /usr/local/sbin パスを
    # 決め打ちすると稼働中の panctl を見失い重複起動 storm を起こす。
    pgrep -x panctl >/dev/null 2>&1
}

bt_state() {
    if ! is_panctl_running; then
        echo down; return
    fi
    last=$(grep "panctl\[" /var/log/daemon 2>/dev/null | tail -1)
    case "$last" in
        ""|*"no matching RFCOMM"*|*"reconnect: re-issuing"*|*"Authentication"*|*"denied"*|*"Disconnect"*)
            echo down ;;
        *) echo up ;;
    esac
}

update_bt_state() {
    new=$(bt_state)
    cur=$(cat "$BT_STATE_FILE" 2>/dev/null)
    if [ "$new" != "$cur" ]; then
        echo "$new" > "$BT_STATE_FILE"
        log "BT state: $cur -> $new"
    fi
}

# LED 制御 (gpioctl は /etc/rc.securelevel で red_led/green_led を open 済前提)
led_set() {
    # $1=red(0|1)  $2=green(0|1)
    gpioctl gpio1 red_led   "$1" >/dev/null 2>&1
    gpioctl gpio1 green_led "$2" >/dev/null 2>&1
}

led_last=""
update_led() {
    [ "$LED_ENABLED" = "1" ] || return
    p=$(sysctl -n hw.sensors.simplebat0.percent0 2>/dev/null | awk '{print int($1)}')
    st=$(sysctl -n hw.sensors.simplebat0.raw0    2>/dev/null | awk -F'[()]' '{print $2}')
    [ -z "$p" ] && return

    case "$st" in
        charging)
            if [ "$p" -ge "$FULL_BAT" ]; then
                want="green"
            else
                want="orange"
            fi ;;
        full)
            want="green" ;;
        discharging|*)
            if [ "$p" -le "$LOW_BAT" ]; then
                want="red"
            else
                want="off"
            fi ;;
    esac

    if [ "$want" != "$led_last" ]; then
        case "$want" in
            orange) led_set 1 1 ;;
            green)  led_set 0 1 ;;
            red)    led_set 1 0 ;;
            off)    led_set 0 0 ;;
        esac
        log "LED: $led_last -> $want (raw0=$st soc=${p}%)"
        led_last="$want"
    fi
}

# PS1 cache 出力 (2026-06-01 v3.9):
# OCV 補正済 _bat と Nerd Font 化 _net を計算して /tmp/.prompt-{bat,net} に書く。
# ~/.kshrc は cat だけで PS1 を組めるので fork 数が劇的に減る (7 → 2)。
# OCV table / R の根拠は ../battery/ README 参照。
PROMPT_BAT_FILE=/tmp/.prompt-bat
PROMPT_NET_FILE=/tmp/.prompt-net
PROMPT_BAT_META=/tmp/.bat-meta    # 行頭: prev_soc prev_state hold_until
_BAT_R=0.12
_BAT_HOLD_SEC=15                  # state 変化後の transient 制限期間 (秒)
_BAT_RAMP=1                       # transient 中の最大変化量 (pt/poll)

update_prompt() {
    # battery: OCV 補正 → 21-point table → SOC
    v=$(sysctl -n hw.sensors.simplebat0.volt0    2>/dev/null | awk '{print $1}')
    i=$(sysctl -n hw.sensors.simplebat0.current0 2>/dev/null | awk '{print $1}')
    s=$(sysctl -n hw.sensors.simplebat0.raw0     2>/dev/null | awk -F'[()]' '{print $2}')
    if [ -n "$v" ]; then
        pp_raw=$(awk -v v="$v" -v i="$i" -v s="$s" -v R="$_BAT_R" '
            BEGIN {
                if      (s == "charging")    ocv = (v - i * R) * 1000
                else if (s == "discharging") ocv = (v + i * R) * 1000
                else                         ocv = v * 1000
                # 21-point OCV table (mV, ascending).  This MUST be replaced
                # with the values extracted from YOUR factory dtb — they
                # are a hardware fingerprint and differ per device.
                # See ../battery/README.md "factory dts から OCV table を抜く"
                # for the extraction procedure.  Leave 3400 and 4200 as the
                # endpoints; fill V2..V20 from your own dtb.
                n = split("3400 PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV 4200", \
                          t, " ")
                if      (ocv <= t[1]) p = 0
                else if (ocv >= t[n]) p = 100
                else {
                    for (k = 2; k <= n; k++) {
                        if (ocv < t[k]) {
                            p = (k - 2) * 5 + (ocv - t[k-1]) / (t[k] - t[k-1]) * 5
                            break
                        }
                    }
                }
                if (p > 100) p = 100
                if (p <   0) p = 0
                printf "%d", p + 0.5
            }')

        # transient suppression: plug-in/out で V/I/state の整合が崩れる瞬間に
        # OCV 計算が ±10pt 飛ぶ問題対策。 state 変化を検知したら _BAT_HOLD_SEC
        # 秒間 ramp 制限 (±_BAT_RAMP pt/poll で raw 値に近づく)。
        meta=$(cat "$PROMPT_BAT_META" 2>/dev/null || echo "")
        prev_soc=$(echo "$meta"   | awk '{print ($1=="")?-1:$1+0}')
        prev_state=$(echo "$meta" | awk '{print $2}')
        hold_until=$(echo "$meta" | awk '{print ($3=="")?0:$3+0}')
        now=$(date +%s)

        if [ -n "$prev_state" ] && [ "$s" != "$prev_state" ]; then
            hold_until=$((now + _BAT_HOLD_SEC))
            log "prompt: state $prev_state -> $s, hold ramp ${_BAT_HOLD_SEC}s (raw=${pp_raw}%)"
        fi

        pp=$pp_raw
        if [ "$now" -lt "$hold_until" ] && [ "$prev_soc" -ge 0 ]; then
            delta=$((pp_raw - prev_soc))
            if   [ "$delta" -gt "$_BAT_RAMP" ]; then pp=$((prev_soc + _BAT_RAMP))
            elif [ "$delta" -lt "-$_BAT_RAMP" ]; then pp=$((prev_soc - _BAT_RAMP))
            fi
        fi

        echo "$pp $s $hold_until" > "$PROMPT_BAT_META"

        case "$s" in
            charging)    bcol=$(printf '\033[38;5;208m') ;;
            discharging) [ "$pp" -le 15 ] && bcol=$(printf '\033[31m') || bcol=$(printf '\033[37m') ;;
            *)           bcol=$(printf '\033[32m') ;;
        esac
        rst=$(printf '\033[0m')
        printf '%s%d%%%s' "$bcol" "$pp" "$rst" > "$PROMPT_BAT_FILE.tmp"
        mv "$PROMPT_BAT_FILE.tmp" "$PROMPT_BAT_FILE"
    fi

    # net: WiFi green / BT blue / down gray + Nerd Font glyph
    wf=""; bv=""
    is_wifi_active && wf=1
    [ "$(cat "$BT_STATE_FILE" 2>/dev/null)" = "up" ] && bv=1
    wifi_g=$(printf '\xef\x87\xab')
    bt_g=$(printf '\xef\x8a\x93')
    rst=$(printf '\033[0m')
    if [ -n "$wf" ]; then wcol=$(printf '\033[32m'); else wcol=$(printf '\033[90m'); fi
    if [ -n "$bv" ]; then bcol2=$(printf '\033[34m'); else bcol2=$(printf '\033[90m'); fi
    printf '%s%s%s%s%s%s' "$wcol" "$wifi_g" "$rst" "$bcol2" "$bt_g" "$rst" > "$PROMPT_NET_FILE.tmp"
    mv "$PROMPT_NET_FILE.tmp" "$PROMPT_NET_FILE"
}

wifi_down_since=0
log "started (poll=${POLL}s, resume_threshold=${RESUME_THRESHOLD}s, pid=$$)"
update_bt_state
update_led
update_prompt

while :; do
    t0=$(date +%s)
    sleep "$POLL"
    t1=$(date +%s)
    elapsed=$((t1 - t0))

    if [ "$elapsed" -gt "$RESUME_THRESHOLD" ]; then
        # resume 直後に bwfm を触ると、 カーネル側 bwfm の resume 再init
        # (DVACT_WAKEUP -> bwfm_init) と競合し net80211 の NULL/stale 参照で
        # kernel data abort (ddb) になる。 対策: settle してから、 かつ
        # カーネルが自力で WiFi を戻せていない時だけ wifi_up する。
        log "resume detected (slept ${elapsed}s) -- settle ${RESUME_SETTLE:-8}s before touching wifi"
        sleep "${RESUME_SETTLE:-8}"
        if is_wifi_active; then
            log "wifi recovered by kernel WAKEUP -- skip wifi_up (avoid bwfm race)"
        else
            log "wifi still down after settle -- wifi_up"
            wifi_up
        fi
        if [ "$PANCTL_ENABLED" = "1" ]; then
            /etc/rc.d/panctl restart >/dev/null 2>&1 || true
        fi
        wifi_down_since=0
        update_bt_state
        update_led
        update_prompt
        continue
    fi

    if is_wifi_active; then
        wifi_down_since=0
    else
        if [ "$wifi_down_since" = "0" ]; then
            wifi_down_since=$t1
            log "wifi link down, will retry in ${WIFI_DOWN_RETRY}s"
        elif [ $((t1 - wifi_down_since)) -ge "$WIFI_DOWN_RETRY" ]; then
            log "wifi still down -- bring up + re-issue joins + dhcp"
            wifi_up
            wifi_down_since=$t1
        fi
    fi

    if [ "$PANCTL_ENABLED" = "1" ]; then
        if ! is_panctl_running; then
            log "panctl not running -- start"
            /etc/rc.d/panctl start >/dev/null 2>&1 || true
        fi
    fi

    update_bt_state
    update_led
    update_prompt
done
