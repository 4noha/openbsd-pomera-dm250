# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# ~/.kshrc — sourced by interactive ksh (via $ENV)

# --- command history persistence -----------------------------------------
HISTFILE=$HOME/.ksh_history
HISTSIZE=10000
export HISTFILE HISTSIZE

# --- battery indicator (OCV-corrected, measured-R) --------------------------
# OCV table は工場 dtb の 21 点 (5%-step, 3400mV→4200mV)、 design 5800mAh。
# 出典: logo/out/rk-kernel.dtb → factory-dtb/rk-kernel.dts
#
# 内部抵抗 R は工場値 45mΩ ではなく **実測 ~120mΩ** を採用 (2026-05-30):
#   - 負荷ステップ法 (ΔV/ΔI) と 充電/放電 2 点法 が一致して R≈0.12Ω
#   - 工場 45mΩ は新品セルの瞬時オーミック。 実測 120mΩ は経年+分極込みの
#     実効抵抗で、 OCV 復元 (= 完全弛緩電圧) に使うのはこちらが正しい
#   - R=45mΩ では plug/unplug jump が ±14pt 残るが、 0.12Ω だと充放電の
#     OCV 推定がほぼ一致し jump がほぼ消える (notes/observations.md 03:35-41)
#   - この R は _bat が読むのと同じ current0 で較正済 = 自己整合的
_BAT_R=0.12   # ohms; measured effective R (load-step + charge/discharge 2-point)

_bat() {
    local p st color reset
    p=$(sysctl -n hw.sensors.simplebat0.percent0 2>/dev/null | awk '{print int($1)}')
    st=$(sysctl -n hw.sensors.simplebat0.raw0    2>/dev/null | awk -F'[()]' '{print $2}')
    [ -z "$p" ] && return
    # wsvt25 colors#8: charging=orange(yellow), discharging=white, full=green
    case "$st" in
        charging)    color=$(printf '\033[38;5;208m') ;;   # 256-color orange (8色端末では fallback)
        discharging) color=$(printf '\033[37m') ;;
        *)           color=$(printf '\033[32m') ;;
    esac
    reset=$(printf '\033[0m')
    printf '%s%d%%%s' "$color" "$p" "$reset"
}

# raw kernel-reported percent — 比較用
_bat_raw() { sysctl -n hw.sensors.simplebat0.percent0 2>/dev/null | awk '{print int($1)}'; }

# debug dump — V / I / state / OCV(mV) / factory SOC / raw SOC
_bat_debug() {
    local v i s
    v=$(sysctl -n hw.sensors.simplebat0.volt0    | awk '{print $1}')
    i=$(sysctl -n hw.sensors.simplebat0.current0 | awk '{print $1}')
    s=$(sysctl -n hw.sensors.simplebat0.raw0     | awk -F'[()]' '{print $2}')
    printf 'V=%s I=%s state=%s R=%s ocv=' "$v" "$i" "$s" "$_BAT_R"
    awk -v v="$v" -v i="$i" -v s="$s" -v R="$_BAT_R" '
        BEGIN {
            if      (s == "charging")    ocv = (v - i * R) * 1000
            else if (s == "discharging") ocv = (v + i * R) * 1000
            else                         ocv = v * 1000
            printf "%d", ocv
        }'
    printf ' soc(factory)=%s%% soc(raw)=%s%%\n' \
        "$(_bat | sed 's/[^0-9]//g')" "$(_bat_raw)"
}

# --- physical link indicator: wifi / bt / wifi+bt / off ---------------------
_net() {
    local wifi bt cyan reset
    ifconfig bwfm0 2>/dev/null | grep -q 'status: active' && wifi=1
    [ "$(cat /tmp/.bt-state 2>/dev/null)" = "up" ] && bt=1
    cyan=$(printf '\033[36m')
    reset=$(printf '\033[0m')
    if   [ -n "$wifi" ] && [ -n "$bt" ]; then printf 'WiFi%sBT%s' "$cyan" "$reset"
    elif [ -n "$wifi" ];                 then printf 'WiFi'
    elif [ -n "$bt" ];                   then printf '%sBT%s' "$cyan" "$reset"
    else                                      printf 'off'
    fi
}

PS1='$(_bat)$(_net) \$ '

# WiFi 再 scan + 再 join (移動時に最強信号の AP に切替え)
alias wifi-rescan='doas ifconfig bwfm0 down; sleep 1; doas sh /etc/netstart bwfm0; sleep 2; ifconfig bwfm0 | grep -E ssid\\\|inet'
