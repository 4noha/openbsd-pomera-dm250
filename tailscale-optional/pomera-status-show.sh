#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
#
# pomera-status-show.sh — run on the Mac, called from tmux status-right.
# Emits the pomera-pushed status (/tmp/pomera-status) only when the file
# exists AND is fresh (mtime <=20 s). Older / missing (= pomera detached,
# roaming, suspended, or never connected) -> emit empty -> the black box
# disappears from the bar entirely.
#
# Styling (black background + padding) lives here, not in the tmux format,
# so that an empty result really means empty (no phantom box).
#
# The pomera-side `claude` wrapper deletes the file on detach via EXIT
# trap, so usually the box goes away instantly. The mtime check is the
# backstop for the case where ssh died hard and the trap never fired.
# The pushed content already includes tmux #[fg=...] markup per glyph
# (battery / wifi / bt color), which tmux honors at status paint time.
f=/tmp/pomera-status
[ -f "$f" ] || exit 0
now=$(date +%s)
m=$(stat -f %m "$f" 2>/dev/null || stat -c %Y "$f" 2>/dev/null)   # macOS / Linux both
[ -n "$m" ] && [ $((now - m)) -le 20 ] && printf ' #[bg=black,fg=cyan] %s #[default]' "$(cat "$f")"
exit 0
