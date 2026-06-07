<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# Pushing pomera battery/net status into Mac tmux `status-right`

While the pomera is attached to the remote tmux on your Mac, push the
pomera's own battery + net state (the same `/tmp/.prompt-{bat,net}` the
PS1 reads — see `battery/`) up to the Mac so tmux can render it in
`status-right`.

```
pomera -> (ssh claude, ControlMaster reused) -> <home-mac>:/tmp/pomera-status -> tmux status-right
```

**Direction: pomera-push** (data lives on the pomera, display lives on
the Mac, so something has to cross the link). Outbound from pomera is
the reliable direction in this setup — inbound (Mac -> pomera, through
a phone-NAT BT-tether) is fragile. No active detection is needed
either: the push loop has the exact same lifetime as the attached ssh
session, so detach automatically stops the push.

## Files involved

| File | Lives on | Role |
|---|---|---|
| `claude` | **pomera** `~/bin/claude` | Wrapper. Open ssh ControlMaster, run a 5 s push loop, shrink mlterm-fb font, `tmux attach`. Auto-reconnects if ssh dies. |
| `pomera-status-show.sh` | **Mac** `~/bin/pomera-status-show.sh` | Called from `status-right`. Emits a black-boxed snippet **only when** `/tmp/pomera-status` is fresh (mtime <=20 s). Otherwise emits empty (the box disappears entirely). |

The wrapper command is named `claude` because the pomera is a
thin-client — no Claude Code runs locally, no name conflict, and the
internal `ssh claude` (Host alias) is unrelated.

## Deploy

### On the pomera

```sh
# on mac — copy the wrapper to ~/bin/claude on the pomera
scp tailscale-optional/claude pomera:bin/claude
# on pomera
chmod +x ~/bin/claude
# Now instead of `ssh claude && tmux attach`, just:
claude                      # attach to claude-master + push status + shrink font
```

Prerequisites:

- `~/.ssh/config` has `Host claude` with `ControlMaster auto`
  (see `ssh-config.md`).
- The pomera key is passphrase-less, or `ssh-agent` is already holding
  it — so the push loop can authenticate non-interactively.
- The pomera is updating `/tmp/.prompt-{bat,net}` (the netwatchd/PS1
  cache — see `battery/`).

### On the Mac

```sh
# on mac
cp tailscale-optional/pomera-status-show.sh ~/bin/
chmod +x ~/bin/pomera-status-show.sh

# Append to ~/.tmux.conf
cat >> ~/.tmux.conf <<'EOF'

# pomera (thin-client) battery/net status in status-right
set -g status-interval 5
set -g status-right-length 80   # default 40 fills with title+clock and clips the pomera bit
# Styling (black box + padding) is added by the helper. When the helper
# returns empty (pomera not attached) the box disappears entirely — keep
# styling out of the tmux format so it does not leave a phantom box.
# WARNING: `set -ag status-right "..."` is non-idempotent. If you
# source-file this config N times into a long-lived tmux server, you get
# N copies of the helper invocation pinned to status-right and the
# pomera status renders N times. Guard with if-shell + a flag.
if-shell -F '#{!=:#{@pomera_status_added},1}' {
  set -ag status-right "#(~/bin/pomera-status-show.sh)"
  set -g @pomera_status_added 1
}
EOF
tmux source-file ~/.tmux.conf      # apply to running sessions
```

> [!NOTE]
> The default status bar uses `status-style bg=green,fg=black`. Painting
> the pomera bit with `#[fg=cyan]` only would leave cyan-on-green
> (unreadable). The helper switches `bg=black` for its segment, then
> tints the foreground per-glyph. Set `status-right-length 80` or the
> default 40-column right-clip will hide the appended segment entirely.

Result: a small black box with `87% [wifi] [bt]` — wifi/bt are Nerd
Font icons, colored green/blue when on, gray when off — updating every
5 s. While the pomera is **not** attached (detached, roaming, or you
are working locally on the Mac), the helper returns empty and the
black box disappears from the bar. No `pomera` prefix label.

## How it works / pitfalls

- **Push loop rides ControlMaster**: each 5 s `ssh claude "cat > ..."`
  reuses the multiplexed control channel, so it costs milliseconds.
- **Open the master before pushing**: if the pomera ssh key is
  passphrase-protected, a non-interactive push would fail with
  `Permission denied (publickey)`. The wrapper runs
  `ssh -O check || ssh -fN claude` once at start so the passphrase
  prompt happens exactly once. After that, `push` and `attach` share
  the channel. Don't `-O exit` on cleanup — other sessions may be
  riding the same master.
- **Detach cleans up the box**: the wrapper's `trap EXIT` kills the
  push loop and `rm -f /tmp/pomera-status` on the Mac. If something
  wedges (kill -9, panic) and the file is left stale, the helper's
  20 s mtime check still returns empty — so the box vanishes anyway.
  Styling is colocated with content in the helper for this reason: an
  empty helper means empty box, no phantom black rectangle.
- **Wrapper edits need a reconnect**: a running `claude` keeps the
  code it loaded at start, so `scp`-ing a new wrapper does nothing
  until you `detach` and run `claude` again. A push loop orphaned by a
  network drop (trap never fires) keeps writing the old format ->
  "sometimes the pomera box reappears even though I'm detached".
  Mitigation: the wrapper writes its loop PID to
  `/tmp/.pomera-push.pid` and kills any prior loop on next start.
- **"pomera box renders multiple times" is a different bug**: the push
  side writes a single file no matter how many loops are running, so
  duplication never comes from the push side. Duplicates come from
  tmux: multiple `set -ag status-right "#(...)"` lines appended to a
  long-lived server. The `if-shell + @pomera_status_added` guard
  prevents that. If you already accumulated N copies in a running
  server, strip the helper from `status-right` with `sed` and re-add
  one copy.
- **Font shrink on attach**: the wrapper sends mlterm-fb's OSC 5379 to
  set fontsize to `FONT_ON` (12) on attach and back to `FONT_OFF` (19)
  on detach. Sent only when stdout is a TTY (`[ -t 1 ]`). Terminals
  other than mlterm ignore OSC 5379 silently — harmless.
- **`#()` in tmux only fires while a client is attached.** Status-bar
  formats are evaluated for each attached client. If nobody's
  attached, the helper isn't called. So: verify the output by being
  attached, not by `tmux display-message -p`.
- **Watch `status-right-length`** (default: 40). The default
  status-right (`"#{=21:pane_title}" %H:%M %d-%b-%y`) already burns
  ~35-40 chars, and the appended pomera segment (~20 chars
  `87% wifi+ bt+`) gets clipped off the right. Bumping to 80 gives
  comfortable headroom.
- **Roam-resilient connection** (assumes most days the pomera is on
  BT-tether, not home Wi-Fi):
  - **App-level keepalive** (`ServerAliveInterval=15
    ServerAliveCountMax=6` = 90 s of silence tolerated) plus
    `TCPKeepAlive=no` (so NAT eviction does not look like death) and
    `Compression=yes` (cheap on slow links).
  - **Auto-reconnect**: if ssh exits non-zero, the wrapper retries
    `tmux attach` with backoff (2 -> 4 -> ... -> 10 s, ceiling). A
    clean detach (Ctrl-b d -> rc 0) exits the loop. Ctrl-C also exits.
  - Same keepalive lives in `~/.ssh/config` `Host claude mac` (see
    `ssh-config.md`) so bare `ssh claude` / `scp` survive the same
    blackouts.
  - Even more robust: **mosh**, but armv7 + Mac both need it; punted.
- **Battery is colored text, wifi/bt are Nerd Font glyphs** (all
  matching the PS1):
  - `build_status()` reads the ANSI color escape that netwatchd already
    wrote into `/tmp/.prompt-{bat,net}` and translates it to a tmux
    `#[fg=...]` marker. Translation table:
    - battery: `208m` = charging (orange), `31m` = <=15% (red),
      `32m` = full (green), `37m` = normal discharge (white).
    - wifi: `32m` = on (green) / gray = off.
    - bt: `34m` = on (blue) / gray = off.
  - tmux interprets `#[fg=...]` inside `#()` output at status-paint
    time (same mechanism tmux-cpu and friends use; not honored by
    `display-message -p` which is why you should verify in the live
    bar).
  - Background stays black for the segment; only foreground per-glyph
    changes.
  - Nerd Font is required at the rendering terminal (the pomera's
    mlterm-fb, your Mac terminal): without it the glyphs render as
    `[]`. mlterm-fb handles FA4 glyphs fine (wifi U+F1EB, bt U+F293
    are within that subset).
  - If you change netwatchd's color choices, update the `case`
    statement in `claude` to match.
- **Cost**: 5 s tick on the pomera = one `sed`/`cat` + one `ssh`. The
  netwatchd PS1 cache is unmodified — `claude` only reads it.

## Related

- `battery/` — origin of `/tmp/.prompt-{bat,net}` (the PS1/netwatchd
  cache the wrapper consumes here).
- `ssh-config.md` — the ControlMaster wiring this whole thing rides
  on.
