# openbsd-pomera-dm250

Turn a King Jim **Pomera DM250** into a usable OpenBSD machine, and
(optionally) into a Bluetooth-tethered thin client for a remote shell.

This repository collects everything that was needed to make that work
end-to-end on real hardware: an installation procedure, a kernel patch
series layered on top of [`jcs/openbsd-src`][jcs-src], userland helpers
for the bits that the stock OpenBSD tree does not yet handle cleanly
(Bluetooth PAN tether, suspend / resume, RK818 fuel-gauge prompt, boot
logo), and a cross-build setup that does the heavy lifting on a Mac so
the DM250 itself never has to compile anything.

## Why this exists

The DM250 is a small, fan-less, all-day-battery Japanese-keyboard
typewriter built on a Rockchip RK3128. It is wonderful hardware to type
into and miserable hardware to compile on. Joshua Stein's January 2024
write-up, *[Installing OpenBSD on the Pomera DM250][jcs-post]*, showed
that OpenBSD/armv7 can be coaxed onto it, and his
[`jcs/openbsd-src`][jcs-src] branch carries the rk3128 bring-up
patches.

This repository builds on top of that work with the patches and tooling
that were still missing after a stock install — most importantly the
**Bluetooth-tether path** (so the DM250 can reach the internet through
an Android phone when Wi-Fi is unreliable), a **suspend / resume fix**
for the BCM4343A1 SDIO Wi-Fi, an **OCV-corrected battery prompt** that
matches the RK818's real curve, and a **boot-logo replacement** that
keeps the stock factory look.

## Target hardware

- **Pomera DM250** (the original 2023 model)
- **Pomera DM250X**
- **Pomera DM250XY**

These three SKUs share the same SoC, eMMC layout, RK818 PMIC, AP6212
Wi-Fi / BT combo module, and U-Boot. They are treated as one platform
throughout this repo.

> The **DM250US** is *not* supported here. Its U-Boot and LED control
> path are different from the JP-region models, and none of the
> patches in `kernel-patches/` have been tested against it.

## Architecture overview

```
   +-----------------+        Bluetooth PAN tether
   |  Pomera DM250   | ----------------------------------+
   |  (OpenBSD/armv7)|                                   |
   |                 |  fallback: Wi-Fi (bwfm sdio)      v
   +--------+--------+                              +---------+
            |                                       | Android |
            | tmux/ssh over Tailscale               | (phone) |
            |                                       +----+----+
            v                                            |
   +-----------------+                                   | 4G/5G
   | Home box (Mac / |     Tailnet (WireGuard)           v
   |  Linux server)  | <----------------------------- Internet
   | tmux: dev shell |
   +-----------------+
```

The DM250 is intentionally kept minimal: it runs OpenBSD, a console
multiplexer, `ssh`, `tailscale`, and the BT tether daemon. Everything
heavy — language servers, AI tooling, large editors — runs on the home
box and is reached through `ssh` + `tmux attach`.

## Two paths to a working DM250

There are two supported ways to use this repository. They share the
exact same end state.

### 1. Prebuilt path (fast, recommended)

If you trust the maintainer's binaries (they are signed-by-tag on
GitHub and have SHA256 hashes recorded under `prebuilt-info/`), you can
skip the cross-build step entirely and pull artifacts straight from
GitHub Releases:

```sh
# on mac / linux, with `gh` authenticated
REL=armv7-artifacts-v1
REPO=<your-name>/openbsd-pomera-dm250
mkdir -p ~/dm250-artifacts && cd ~/dm250-artifacts

gh release download "$REL" -R "$REPO" -p 'bsd.armv7.delay-2s'
gh release download "$REL" -R "$REPO" -p 'BCM4343A1.hcd.armbian-ap6212'
gh release download "$REL" -R "$REPO" -p 'ja-mozc-server-*.tgz'
gh release download "$REL" -R "$REPO" -p 'mlterm-fb.armv7'

# verify hashes
sha256sum -c <(gh release view "$REL" -R "$REPO" --json assets -q \
  '.assets[] | "\(.digest)  \(.name)"')

# copy onto the device
scp ./* <your-pomera-user>@<dm250-lan-ip>:/tmp/
```

You then drop the kernel into `/bsd`, the firmware blob into
`/etc/firmware/`, install the mozc tarball with `tar -C / -xzf`, and
reboot. The full step-by-step is in `install/README.md`.

### 2. Build-from-source path

If you do not trust the prebuilts, or if you want to modify a patch,
the cross-build flow lives in `docs/build-vm.md` and
`docs/kernel-cross-build.md`. The short version:

```sh
# on mac (Apple Silicon recommended)
qemu-system-aarch64 -accel hvf ...   # see docs/build-vm.md
# inside the VM:
ssh builder@localhost -p 2222
git clone <jcs-src> && cd src
git apply <path-to>/kernel-patches/*.patch
cd sys/arch/armv7/compile/GENERIC && make obj && make config && make
```

The result is a `bsd` you can `scp` to the DM250 exactly the same way
the prebuilt is consumed. The VM takes around 5 minutes per kernel
build on M-series silicon; the DM250 itself takes about 73 minutes
for the same kernel and crashes under sustained build load, which is
why all instructions assume **the DM250 is never the build host**.

## Subtree map

The repository is divided into self-contained subtrees. Each one has
its own README and is meant to be readable in isolation.

| Directory                 | What is in it |
|---------------------------|---------------|
| `install/`                | The "from a brand-new DM250 to a usable OpenBSD shell" procedure: eMMC backup pointer, OpenBSD installer SD prep, Wi-Fi / BT firmware install, keymap, hostname, and the first reboot. |
| `kernel-patches/`         | A patch series layered on top of `jcs/openbsd-src` rk3128. Each `*.patch` file carries an upstream-base note and inherits OpenBSD's ISC license (see `LICENSE-ISC`). |
| `panctl/`                 | The Bluetooth PAN tether daemon and its supporting `netwatchd` glue: pair once, then claim the default route on demand when Wi-Fi disappears. |
| `harness/`                | A persistent-log suspend / resume debug harness. Survives a kernel crash, replays the same scenario on the next boot, and lets you bisect WiFi-resume regressions without sitting in front of the device. |
| `logo/`                   | A small extractor + replacement tool for the DM250 splash logo. Sources the factory image from *your own* eMMC backup; nothing copyrighted ships in this repo. |
| `battery/`                | An OCV-corrected PS1 prompt that calls into the RK818 fuel-gauge counters and corrects for the gauge's well-known idle drift. |
| `tailscale-optional/`     | The thin-client wiring: `tailscale up` arguments, ACL hints, an `~/.ssh/config` template that points `<pomera-host>` at the right tmux session. |
| `docs/`                   | Cross-build VM setup (qemu HVF arm64 on Apple Silicon, reusable for any armv7 cross-build), kernel cross-build steps, and the OCV / battery math derivation. |
| `prebuilt-info/`          | A manifest of the binary artifacts published to GitHub Releases, with names, SHA256 hashes, and a one-line description of what each artifact is. The artifacts themselves live in Releases, not in the git tree. |

If you only want the device working, read `install/` first and circle
back to the others as needed. If you want to understand *why* something
is the way it is, the relevant subtree's README and the matching memory
note in `docs/` are where to look.

## Status

The "OpenBSD on DM250" base from `jcs/openbsd-src` has been working for
the better part of a year. The work in this repository adds, on top of
that:

- A stable **BT-tether default route** that fails over from Wi-Fi to
  Bluetooth automatically (`panctl/`, `prio48 backup default` on
  `tun0`).
- A **BCM4343A1 resume fix** for the bwfm SDIO driver. The original
  symptom was a kernel data abort on lid-open; the v3 patch in
  `kernel-patches/` (`bwfm_sdio_activate` + ieee80211 BA cleanup +
  `media_status` NULL guard) makes lid close / open reliable.
- A **sdmmc / dwmmc removable-handling tweak** so that SD card and
  bwfm Wi-Fi do not vanish across resume.
- An **OCV-corrected PS1** for the RK818 fuel gauge, served from
  `/tmp/.prompt-bat` so `~/.kshrc` is just a `cat`. The earlier
  `rkfuelgauge` kernel-module path is documented but frozen.
- A **boot-logo replacement** that re-uses your own factory logo from
  your eMMC backup, so no copyrighted artwork is checked in.
- A **cross-build VM** at `builder@localhost:2222` (qemu HVF arm64),
  reusable for any armv7 cross-build that comes up later.

Open items, captured in `harness/` and the per-subtree READMEs:

- **`sdmmc` removable behaviour** under heavy I/O is still being
  ironed out; the current patch trusts the device-tree
  `non-removable` flag, which is correct for the eMMC but might be
  too aggressive for the SD slot. See `kernel-patches/`.
- **CMD5 / `fn_count` re-arm** on bwfm resume — root-caused, fix
  drafted, not yet in the patch series.

## License summary

- Top-level project files (everything that is *not* under
  `kernel-patches/`) are **MIT-licensed**. See [`LICENSE`](LICENSE).
- Patches under `kernel-patches/` modify the OpenBSD source tree and
  **inherit OpenBSD's ISC license**. See
  [`LICENSE-ISC`](LICENSE-ISC).
- Third-party works (OpenBSD itself, the `jcs/openbsd-src` branch,
  armbian's Broadcom firmware blobs, BlueKitchen BTstack, the Linux
  RK818 driver used as a reference for the OCV curve, ichinomoto's
  EKESETE eMMC backup tool) keep their own licenses; their
  attributions live in [`NOTICE`](NOTICE).

New shell, C, and Python files added by this repository carry an SPDX
header:

```sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 4noha
```

New patch files under `kernel-patches/` carry an explicit
`License: ISC (inherits from OpenBSD upstream)` line in their header
block.

## Acknowledgments

This project would not exist without:

- **The OpenBSD Project** — for an operating system that is small,
  honest, and a joy to read.
- **joshua stein (jcs)** — for the original DM250 install write-up
  and the `jcs/openbsd-src` rk3128 branch. The patches in
  `kernel-patches/` are layered on top of that branch; the install
  procedure in `install/` follows the same shape.
- **The armbian project** — for keeping Broadcom AP6212 / BCM4343A1
  firmware blobs in one place and at one version that actually works
  with `bwfm(4)`.
- **BlueKitchen GmbH (BTstack)** — for a Bluetooth stack that is
  embeddable, debuggable, and well-documented enough to write
  `panctl/` against in a few weeks instead of months.
- **ichinomoto** — for the EKESETE eMMC backup tool that made it
  safe to experiment on real DM250 hardware in the first place.
- The unnamed maintainers of the **Linux RK818** driver, whose OCV
  curves and calibration constants saved a great deal of guessing in
  `battery/`. No GPL source is bundled here; only the math was
  re-derived.

## Disclaimers

"Pomera" and "DM250" are product names of King Jim Co., Ltd. This
project is not affiliated with, endorsed by, or sponsored by King Jim.
Everything in this repository is provided **as-is**, with no warranty
of any kind. Installing a custom kernel and replacing boot artifacts
will almost certainly void your warranty, may brick your device, and
absolutely requires that you have a tested eMMC backup before you
start.

See [`install/README.md`](install/) for the backup procedure. Do not
skip it.

[jcs-post]: https://jcs.org/2024/01/15/dm250
[jcs-src]: https://github.com/jcs/openbsd-src
