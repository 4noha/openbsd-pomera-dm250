<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# nodes.md

Naming convention for nodes on your Tailscale tailnet (`<your-tailnet>.ts.net`)
when wiring the pomera thin-client.

## Naming convention

- **Devices with fixed roles** (your home Mac, the pomera) get a hand-picked
  name via `tailscale up --hostname=<name>`. Pick something stable; the ssh
  config and every deploy script will hardcode it.
- Names must be **lowercase + digits + hyphens** only — MagicDNS rejects
  uppercase.
- The pomera in these docs is referred to as `<pomera-host>`. Pick a short
  single-token name; you'll see it in `~/.ssh/config`, in `install/` deploy
  steps, and in tmux status output.
- Leave **auto-named devices** (`asus--ai2202`, `d24wt27c3j` shaped strings)
  alone unless you're sure they're not referenced anywhere. Renaming or
  deleting long-offline nodes in the admin console is a footgun — Tailscale
  re-issues numeric suffixes (`name-1`, `name-2`) if the same hostname comes
  back, which silently breaks any hardcoded references.

## Role assignments (workspace context)

In the rest of this repo, two names are load-bearing:

| Placeholder | Meaning | Where it's referenced |
|---|---|---|
| `<home-mac>` | Your home Mac's Tailscale hostname | `ssh-config.md` (`Host mac`, `Host claude`), `pomera-tmux-status.md`, `install/` Tailscale step |
| `<pomera-host>` | The pomera DM250's Tailscale hostname | `ssh-config.md` (Mac-side `Host pomera`), every deploy step in `install/`, `panctl/`, `battery/` |

If you rename either one, you need to update every place it appears. Do
`grep -rE '<home-mac>|<pomera-host>' .` (substituting whatever names you
picked) before assuming the rename is done.

## Tailnet boundaries

- Don't make the pomera a **subnet router**. If you want to reach your home
  LAN through the tailnet, make your home Mac the subnet router instead.
  Subnet routing is out of scope for this workspace.
- Don't make the pomera an **exit node**. The DM250 SoC (1 GHz Cortex-A7
  quad, 1 GB RAM) does not have the headroom to forward exit-node traffic
  for other devices, and you don't need it for the thin-client use case
  anyway.

## Online vs offline nodes

You'll usually see a mix of online and long-offline devices in your
admin console. Only the two fixed-role nodes (`<home-mac>` and
`<pomera-host>`) matter for this repo. Anything else (NAS, phones, old
laptops) is unrelated to the thin-client path.

If you want to clean up long-offline nodes, do it from the admin console,
but be aware: deleting a node and then reconnecting it under the same
hostname produces a numbered suffix (`oldname-1`) for at least a while.
