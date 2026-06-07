# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# ~/.kshrc — sourced by interactive ksh (via $ENV)

# --- battery indicator (OCV-corrected, factory-tuned) -----------------------
# 工場 dtb から抽出した実測パラメータを使う:
#   - bat_res = 0x2d (= 45 mΩ)
#   - ocv_table = 21 点 5%-step (3400mV → 4200mV)
#   - design_capacity = 5800 mAh
# 一般 Li-ion カーブで近似していた初期版より精度が出るはず。
# 出典: logo/out/rk-kernel.dtb → factory-dtb/rk-kernel.dts
_BAT_R=0.045  # ohms; factory value from dtb

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
