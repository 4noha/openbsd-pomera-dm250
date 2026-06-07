# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# ~/.kshrc — sourced by interactive ksh (via $ENV)
#
# v3.9 (2026-06-01): netwatchd backend が /tmp/.prompt-{bat,net} に書き出すので
# PS1 側は cat するだけ。 fork 数 7 → 2 で大幅高速化。
#
# 計算ロジック (OCV 補正・色・glyph) は /usr/local/sbin/netwatchd.sh の
# update_prompt() を見る。 netwatchd 停止中は PS1 が「[]$」 で出るが、
# それは netwatchd 自体の異常検知のサインになる (敢えて fallback しない)。

# --- command history persistence -----------------------------------------
HISTFILE=$HOME/.ksh_history
HISTSIZE=10000
export HISTFILE HISTSIZE

# --- PS1 (cat-only) -------------------------------------------------------
_bat() { cat /tmp/.prompt-bat 2>/dev/null; }
_net() { cat /tmp/.prompt-net 2>/dev/null; }

PS1='[$(_bat)$(_net)]\$ '

# --- debug helpers (重い計算は残す、 普段使わない) -------------------------
# kernel raw percent (粗い voltage 推定)
_bat_raw() { sysctl -n hw.sensors.simplebat0.percent0 2>/dev/null | awk '{print int($1)}'; }

# V / I / state / OCV(mV) / corrected SOC / raw SOC を出力
_bat_debug() {
    local v i s R
    R=0.12
    v=$(sysctl -n hw.sensors.simplebat0.volt0    | awk '{print $1}')
    i=$(sysctl -n hw.sensors.simplebat0.current0 | awk '{print $1}')
    s=$(sysctl -n hw.sensors.simplebat0.raw0     | awk -F'[()]' '{print $2}')
    printf 'V=%s I=%s state=%s R=%s ocv=' "$v" "$i" "$s" "$R"
    awk -v v="$v" -v i="$i" -v s="$s" -v R="$R" '
        BEGIN {
            if      (s == "charging")    ocv = (v - i * R) * 1000
            else if (s == "discharging") ocv = (v + i * R) * 1000
            else                         ocv = v * 1000
            printf "%d", ocv
        }'
    printf ' soc(corrected)=%s%% soc(raw)=%s%%\n' \
        "$(_bat | awk '{ gsub(/\033\[[0-9;]*m/, ""); gsub(/%/, ""); print }')" \
        "$(_bat_raw)"
}

# --- aliases -------------------------------------------------------------
alias wifi-rescan='doas ifconfig bwfm0 down; sleep 1; doas sh /etc/netstart bwfm0; sleep 2; ifconfig bwfm0 | grep -E ssid\\\|inet'

# ssh で <home-mac> (Host claude) にログイン → 既存 tmux session "claude-master" に attach
# tmux は絶対パス指定 — ssh non-interactive shell では /opt/homebrew/bin が PATH に
# 入らず "command not found: tmux" になるため
alias claude-master='ssh -t claude /opt/homebrew/bin/tmux attach -t claude-master'
