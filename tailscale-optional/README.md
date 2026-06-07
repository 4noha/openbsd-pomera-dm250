<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# tailscale-optional/

Optional Tailscale layer for the pomera DM250 thin-client.

DM250 (OpenBSD/armv7) <-> home Mac, joined through Tailscale, so the pomera
can attach a remote `claude-master` tmux session from any network.

This subtree is **optional** — the install (see `install/`) brings the
device up far enough to ssh over LAN or BT-tether to your home Mac without
Tailscale. Add Tailscale only if you want the device to roam (cafe Wi-Fi,
LTE tether, friend's house) and still hit the same hostname.

## Goal

- From the pomera, attach `tmux` on your home Mac through `ssh claude`
  regardless of the underlying network (home LAN / public Wi-Fi / phone
  tether).
- Path: pomera -> Tailscale tailnet (`<your-tailnet>.ts.net`) -> Mac, with
  MagicDNS resolving `<home-mac>` so the ssh alias `claude` works
  everywhere.

## Contents

| File | Purpose |
|---|---|
| `README.md` | This file. Architecture, deploy outline. |
| `nodes.md` | Naming convention + role of each tailnet node. |
| `ssh-config.md` | `~/.ssh/config` templates for both directions. |
| `pomera-tmux-status.md` | How the pomera pushes battery/net status to Mac tmux `status-right` (mechanism B). |
| `claude` | pomera-side wrapper: open ssh, push status, attach tmux, shrink font, auto-reconnect. |
| `pomera-status-show.sh` | Mac-side helper called from `tmux` `status-right` to render the pomera-pushed status. |

## Architecture

```
                  Tailscale Tailnet
                  <your-tailnet>.ts.net

  pomera ----- ssh -----> <home-mac>     <- Host claude alias
  (<pomera-host>)         (your home Mac)
                          claude-master tmux session lives here

  Related nodes (optional, see nodes.md): NAS, phones, etc.
```

The pomera is **not** an exit node and **not** a subnet router. It only
makes one outbound ssh connection to your Mac and (while attached) one
push connection back to that same Mac for status display. Nothing else
in your tailnet needs to know about it.

## Deploy outline

(Full step-by-step lives in `install/` step 6 — Tailscale join and ssh
wiring. This file is the reference; install/ is the runbook.)

1. **Install Tailscale on the pomera** (already covered by
   `install/` — package: `tailscale` from OpenBSD ports, started as
   `tailscaled` via rc.d). First-time join is done by web auth: run
   `doas tailscale up --hostname=<pomera-host>` and visit the printed URL
   on your Mac to approve.
2. **Install Tailscale on your home Mac** if not already, give it a
   stable hostname (`<home-mac>`).
3. **Write `~/.ssh/config`** on both sides (see `ssh-config.md`).
4. **Drop `claude` wrapper on pomera** at `~/bin/claude`, drop
   `pomera-status-show.sh` on Mac at `~/bin/`, add the snippet to your
   `~/.tmux.conf`. See `pomera-tmux-status.md` for the snippet and the
   pitfalls (status-right length, non-idempotent `set -ag`, etc.).
5. **Use it**: from pomera, run `claude`. That's it — the wrapper opens
   the ssh ControlMaster, starts the status push loop, shrinks the
   mlterm-fb font for the small DM250 screen, and `tmux attach`es to the
   `claude-master` session on the Mac. Ctrl-b d to detach cleanly.

## Auth keys

- Default flow is **web-auth, no auth key**. `tailscale up` prints a URL,
  you approve once per device, that's it. Keys auto-renew while the
  device stays online.
- If you want unattended re-provision (CI, scripted reinstall) generate
  an auth key in the Tailscale admin console and keep it in a sidecar
  (e.g. `~/.tailscale-keys/<purpose>.key`, mode 600). **Do not commit it
  to this repo.** The repo only references the sidecar.

## What this is not

- **Not** a Tailscale tutorial — see Tailscale's own docs for the basics
  of `tailscale up`, ACL, MagicDNS.
- **Not** an exit-node or subnet-router setup — those routes are not
  needed for the thin-client use case and the DM250 is too small to run
  exit-node traffic for other devices anyway.
- **Not** required for the install — `install/` works without this whole
  subtree. Add it only if you want roaming.

## Cross-references

- `install/` — full DM250 install runbook (Tailscale join is one of its
  steps).
- `panctl/` — BT-tether daemon. If your roaming network is "phone over
  BT" rather than Wi-Fi, that's what's underneath the Tailscale layer
  most of the time.
- `battery/` — the `/tmp/.prompt-{bat,net}` files that `claude` reads to
  build the pushed status come from this layer (OCV-corrected battery,
  net state).
