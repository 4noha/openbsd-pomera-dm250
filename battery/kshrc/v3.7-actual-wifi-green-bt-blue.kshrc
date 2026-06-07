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

# --- Nerd Font BMP PUA glyphs (3-byte UTF-8) -------------------------------
# v3.6 (2026-06-01) — emoji を諦め、 Hack Nerd Font Mono の Font Awesome 4
# グリフ (BMP PUA U+F000-F8FF) に統一。 framebuffer mlterm-fb で確実に出る。
# Material Design Icons (nf-md-*, U+F0000-) は SMP PUA で mlterm-fb が引けず
# 空白になるので使わない。
#   nf-fa-battery_full       U+F240  EF 89 80
#   nf-fa-battery_3/4        U+F241  EF 89 81
#   nf-fa-battery_half       U+F242  EF 89 82
#   nf-fa-battery_1/4        U+F243  EF 89 83
#   nf-fa-battery_empty      U+F244  EF 89 84
#   nf-fa-bolt (charging)    U+F0E7  EF 83 A7
#   nf-fa-wifi               U+F1EB  EF 87 AB
#   nf-fa-bluetooth          U+F293  EF 8A 93

_bat() {
    local p st color reset
    p=$(sysctl -n hw.sensors.simplebat0.percent0 2>/dev/null | awk '{print int($1)}')
    st=$(sysctl -n hw.sensors.simplebat0.raw0    2>/dev/null | awk -F'[()]' '{print $2}')
    [ -z "$p" ] && return

    # wsvt25 colors#8: charging=orange(256色), discharging=white, full=green
    case "$st" in
        charging)    color=$(printf '\033[38;5;208m') ;;
        discharging)
            if [ "$p" -le 15 ]; then color=$(printf '\033[31m')   # 低残量赤
            else                     color=$(printf '\033[37m')   # 通常白
            fi
            ;;
        *) color=$(printf '\033[32m') ;;                          # full=緑
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

# --- physical link indicator: wifi + bt glyphs ------------------------------
# wifi up    = white  glyph
# wifi down  = gray   glyph (同じ glyph、色だけ変える)
# bt connected = cyan  glyph
# bt disconnected = gray glyph
_net() {
    local wifi bt w_col b_col reset wifi_g bt_g
    ifconfig bwfm0 2>/dev/null | grep -q 'status: active' && wifi=1
    [ "$(cat /tmp/.bt-state 2>/dev/null)" = "up" ] && bt=1

    wifi_g=$(printf '\xef\x87\xab')   # nf-fa-wifi      U+F1EB
    bt_g=$(printf '\xef\x8a\x93')     # nf-fa-bluetooth U+F293
    reset=$(printf '\033[0m')

    # WiFi up = green (32) / down = gray (90)
    # BT   up = blue  (34) / down = gray (90)
    if [ -n "$wifi" ]; then w_col=$(printf '\033[32m'); else w_col=$(printf '\033[90m'); fi
    if [ -n "$bt" ];   then b_col=$(printf '\033[34m'); else b_col=$(printf '\033[90m'); fi

    printf '%s%s%s%s%s%s' "$w_col" "$wifi_g" "$reset" "$b_col" "$bt_g" "$reset"
}

PS1='[$(_bat)$(_net)]\$ '

# WiFi 再 scan + 再 join (移動時に最強信号の AP に切替え)
alias wifi-rescan='doas ifconfig bwfm0 down; sleep 1; doas sh /etc/netstart bwfm0; sleep 2; ifconfig bwfm0 | grep -E ssid\\\|inet'
