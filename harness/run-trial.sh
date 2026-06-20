#!/bin/sh
# run-trial.sh — single labeled trial of the wifi-suspend-test harness.
#
# Wraps /root/wifi-suspend-test.sh so the operator can fire each trial in the
# 6-trial paired experiment (3x v3-pure, 3x v3-diag) without retyping the
# pre/post boilerplate.
#
# Usage (on pomera, after deploy):
#   doas sh /root/run-trial.sh <label>
#
# <label> is a short tag for the trial, e.g. v3-pure-1, v3-pure-2, v3-pure-3,
# v3-diag-1, v3-diag-2, v3-diag-3. The label is logged in /var/log/wifi-suspend-test.log
# alongside the kernel sha256 and ddb.panic state so the post-experiment
# harvest can attribute every line to a specific (kernel, trial) pair.
#
# Effect:
#   1. ensure ddb.panic = 1 so any kernel panic during suspend stops at ddb
#      (so the operator can photograph the trace) rather than auto-rebooting
#      and destroying evidence.
#   2. write a TRIAL-START marker into the harness log.
#   3. exec the regular wifi-suspend-test.sh with the label and --go.
#
# This script is intentionally NOT nohup'd here — wrap with `nohup ... &`
# from the calling ssh session so the kernel suspend doesn't kill the test.

set -e

LABEL="${1:?usage: run-trial.sh <label>}"
LOG=/var/log/wifi-suspend-test.log
HARNESS=/root/wifi-suspend-test.sh

doas sysctl ddb.panic=1 >/dev/null

KSHA=$(doas sha256 -q /bsd 2>/dev/null || echo unknown)
DDP=$(sysctl -n ddb.panic 2>/dev/null || echo unknown)

printf '\nTRIAL-START %s %s ddb.panic=%s kernel_sha=%s\n' \
	"$LABEL" "$(date '+%Y-%m-%d %H:%M:%S')" "$DDP" "$KSHA" \
	| doas tee -a "$LOG" >/dev/null
sync

# 本体: ラベル付きで --go 起動。harness 側は label を pre-suspend capture に
# 載せる (--label "$LABEL" 対応版が deploy 済みであること)。
doas sh "$HARNESS" --label "$LABEL" --go
