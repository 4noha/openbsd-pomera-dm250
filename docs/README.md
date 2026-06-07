<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# docs/

Design notes and environment guides for the build / cross-build side of this project. The user-facing install procedure lives under `install/`; the kernel patches themselves live under `kernel-patches/`. This directory holds the **how / why** that backs both.

## Contents

| File | What it covers |
|---|---|
| [`build-vm.md`](build-vm.md) | Host-Mac (Apple Silicon) environments — the throwaway qemu installer used for SD prep, the cross-build sysroot for user-space, and the long-lived arm64 OpenBSD VM used for kernel cross-builds. |
| [`cross-build-kernel.md`](cross-build-kernel.md) | armv7 GENERIC kernel cross-build under the arm64 OpenBSD VM with clang-22 — VM setup, wrapper, prereq patches, feature patches, deploy, rollback. |
| [`bcmbt-patch-design.md`](bcmbt-patch-design.md) | Design note for the `bcmbt-delay-2s` patch (post-firmware HCI Reset delay 250 ms → 2 s on BCM43430A1). Also captures the legacy native-build procedure as a fallback. |

## How these documents relate to the rest of the repo

- **`install/`** holds the SD-prep scripts and the install procedure for the DM250. `build-vm.md` §1 describes the qemu VM those scripts run under.
- **`kernel-patches/`** holds the actual `.patch` / `.diff` files. `cross-build-kernel.md` walks through applying them; `bcmbt-patch-design.md` documents the design behind `bcmbt-delay-2s.patch`.
- **`panctl/`** uses the cross-build sysroot described in `build-vm.md` §2.
- **`harness/`** has the suspend/resume debug harness used when iterating on the Wi-Fi-resume patches mentioned in `cross-build-kernel.md` §3.
- **`prebuilt-info/`** describes the prebuilt-kernel route (route A), as an alternative to the cross-build route (route C) covered here.

## Licensing notes

- Documents in this directory are MIT (same as the rest of the project documentation).
- Patches under `kernel-patches/` inherit ISC from OpenBSD. See top-level `LICENSE-ISC` and `NOTICE`.
- New shell / C / Python files created as part of the install or harness flows carry SPDX `MIT` headers; see top-level `LICENSE`.

## Sanitization placeholders used in this directory

Some commands reference site-specific values. Where those appear, the documents use the following placeholders. Substitute your own values when running:

| Placeholder | Meaning |
|---|---|
| `4noha` | your macOS username |
| `<your-pomera-user>` | the username you created on the DM250 during install |
| `<pomera-host>` | the ssh alias / Tailscale name for the DM250 |
| `<your-tailnet>.ts.net` | your Tailscale tailnet DNS suffix (if you use Tailscale) |
| `<dm250-lan-ip>` | the DM250's LAN IPv4 address |
| `<your-lan-gw>` | your LAN gateway / AP IPv4 |
| `<your-email>` | your email (only appears in patch headers / commit metadata) |

If you intend to publish your own fork, keep these as placeholders in the documents and supply the real values out-of-band (sidecar `.env`, private notes, etc.).
