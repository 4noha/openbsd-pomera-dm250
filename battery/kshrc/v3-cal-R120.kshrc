# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# ~/.kshrc — sourced by interactive ksh (via $ENV)

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
    local v i s sym sign p
    v=$(sysctl -n hw.sensors.simplebat0.volt0    2>/dev/null | awk '{print $1}')
    i=$(sysctl -n hw.sensors.simplebat0.current0 2>/dev/null | awk '{print $1}')
    s=$(sysctl -n hw.sensors.simplebat0.raw0     2>/dev/null | awk -F'[()]' '{print $2}')
    [ -z "$v" ] && return

    case "$s" in
        charging)    sign='-'; sym='+' ;;
        discharging) sign='+'; sym='-' ;;
        *)           sign='0'; sym='=' ;;
    esac

    # OCV correction → look up against factory 21-point OCV table
    p=$(awk -v v="$v" -v i="$i" -v sg="$sign" -v R="$_BAT_R" '
        BEGIN {
            if      (sg == "+") ocv = (v + i * R) * 1000   # → mV
            else if (sg == "-") ocv = (v - i * R) * 1000
            else                ocv = v * 1000

            n = split("PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV", \
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

    printf '[%s%d%%]' "$sym" "$p"
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
    local wifi bt i
    ifconfig bwfm0 2>/dev/null | grep -q 'status: active' && wifi=1
    for i in bnep0 pan0 ubt0; do
        ifconfig "$i" 2>/dev/null | grep -q 'status: active' && { bt=1; break; }
    done
    if   [ -n "$wifi" ] && [ -n "$bt" ]; then printf '[wifi+bt]'
    elif [ -n "$wifi" ];                 then printf '[wifi]'
    elif [ -n "$bt" ];                   then printf '[bt]'
    else                                      printf '[off]'
    fi
}

PS1='$(_bat)$(_net) \$ '
