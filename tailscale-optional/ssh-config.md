<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# ssh-config.md

Templates for the two `~/.ssh/config` files involved (one on each side).
Both assume Tailscale MagicDNS is reaching `<your-tailnet>.ts.net`.

Substitute the placeholders before use:

| Placeholder | What it is |
|---|---|
| `<your-tailnet>` | Your Tailscale tailnet name (the part before `.ts.net`) |
| `<pomera-host>` | The pomera's Tailscale hostname (see `nodes.md`) |
| `<home-mac>` | Your home Mac's Tailscale hostname |
| `<your-pomera-user>` | The pomera shell user (the account you log into on the DM250) |
| `4noha` | Your shell user on the home Mac |

## Mac -> pomera (deploy and maintenance)

`~/.ssh/config` on your Mac:

```ssh-config
Host pomera
    HostName <pomera-host>.<your-tailnet>.ts.net
    User <your-pomera-user>
```

Then `ssh pomera`, `scp <file> pomera:/tmp/`, etc. all work. The default
identity (`~/.ssh/id_ed25519` or whatever you have) is used.

### Prerequisites

- The pomera's `~/.ssh/authorized_keys` contains your Mac's public key.
- Tailscale is running on the Mac and the pomera is visible as a peer
  (`/Applications/Tailscale.app/Contents/MacOS/Tailscale status`).

## pomera -> Mac (`Host claude`, the thin-client path)

`~/.ssh/config` on the pomera:

```ssh-config
# Roam-friendly defaults (works when BT-tether is the underlying network).
# Put keepalive in a leading block so it applies to both ssh aliases below;
# ssh first-match merges the rest from the more specific blocks.
Host claude mac
    ServerAliveInterval 15
    ServerAliveCountMax 6        # 15s x 6 = 90s of silence tolerated before declaring dead
    TCPKeepAlive no              # avoid NAT mis-eviction; rely on app-level keepalive
    Compression yes              # cheap on a thin (BT-tether) link

# Reach the home Mac over Tailscale
Host mac
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    # DERP round-trip is slow-ish; share the control channel so subsequent
    # ssh/scp calls are instant.
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m

# claude: alias for the same Mac (where the claude-master tmux lives)
Host claude
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m
```

`chmod 600 ~/.ssh/config` afterwards.

### Why ControlMaster

- Tailscale DERP-relayed connections eat 200-300 ms per TCP handshake.
- `tmux attach`, batches of `scp`, or "ssh then ssh again 3 s later"
  feel sluggish with a fresh handshake every time.
- `ControlMaster auto` + `ControlPersist 10m` makes the first connection
  expensive and the next 10 minutes cost the same as a local UNIX socket.
- Side effect: a stale control socket survives network changes and gets
  in the way. If something looks wrong, `rm ~/.ssh/cm-*` and reconnect.

### Roam-friendly tuning

- **Keepalive (the leading block)**: short blackouts stay in the same
  ssh session for up to 90 s. `TCPKeepAlive no` puts the burden on
  app-level `ServerAlive*` so NAT-eviction does not look like a hang.
- **Auto-reconnect lives in the `claude` wrapper** (see
  `pomera-tmux-status.md`). When the ssh session dies, the wrapper
  retries with backoff and re-`tmux attach`es. Since tmux is on the
  Mac side, your work state is preserved.
- For a more robust transport, **mosh** is the obvious next step (UDP,
  survives IP changes and sleep). It needs to be installed on both
  ends (the pomera is armv7), so it's not the default here.

### Prerequisites

- The pomera's `~/.ssh/id_ed25519.pub` is in the Mac's
  `~/.ssh/authorized_keys`.
- The Mac is accepting ssh (System Settings -> General -> Sharing ->
  Remote Login).
- Your Tailscale ACL permits pomera -> Mac on port 22. The default
  Personal-tier policy already does this for same-owner devices.

## Thin-client usage pattern

```bash
# on pomera
ssh claude                          # lands on <home-mac>.<your-tailnet>.ts.net
tmux attach -t claude-master        # join the persistent session
# (work)
# Ctrl-b d to detach; ssh session also closes.
```

Or use the `claude` wrapper (one shot: status push + font shrink +
attach + auto-reconnect):

```bash
# on pomera
claude
```

Connection resilience: the tmux session lives on the Mac. The pomera
can drop ssh (lid close, move between networks, BT-tether reconnect)
and come back later with `ssh claude && tmux attach` to pick up
exactly where it was.

## Triage

| Symptom | Where to look |
|---|---|
| `ssh claude` times out | On pomera: `doas tailscale status`. Is the Mac listed as online? If not, wake your Mac. |
| `Permission denied (publickey)` | The pomera pubkey is not in the Mac's `authorized_keys`. `cat ~/.ssh/id_ed25519.pub` on pomera, append on Mac. |
| Mac -> pomera: `Host key verification failed` | Pomera was reinstalled and the host key changed. `ssh-keygen -R <pomera-host>.<your-tailnet>.ts.net` on Mac, retry. |
| ssh works but `tmux: no sessions` on Mac | No `claude-master` session yet. On the Mac: `tmux new -s claude-master`. |
| ssh hangs at random times | Stale ControlMaster socket. On pomera: `rm ~/.ssh/cm-*`. |
