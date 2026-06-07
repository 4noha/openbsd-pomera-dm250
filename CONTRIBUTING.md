# Contributing

Thanks for taking an interest in this project. This file describes how
patches, scripts, and documentation contributions are accepted into the
`openbsd-pomera-dm250` repository.

## Project scope

This repository contains everything needed to turn a King Jim Pomera
DM250 / DM250X / DM250XY into a usable OpenBSD device, and (optionally)
into a thin client that reaches a remote shell over Bluetooth tethering
and Tailscale.

In scope:

- OpenBSD installation and post-install configuration for the DM250
  family
- Kernel patches against the OpenBSD source tree (layered on top of the
  jcs/openbsd-src rk3128 branch)
- Userland tooling that runs on the DM250 itself (`panctl/`, the battery
  PS1 helper, the suspend/resume harness, the logo extractor)
- Cross-build documentation and helper scripts that run on a developer
  Mac or Linux box
- Optional Tailscale integration

Out of scope:

- Changes that target other Pomera hardware (DM30, DM200, DM250US, …)
  unless they are trivial side-effects of DM250 work
- Anything that depends on King Jim's signed firmware blobs being
  redistributed
- Running large AI / LLM stacks directly on the DM250 (the device is
  positioned as a thin client; heavy work belongs upstream of the SSH
  link)

## Before you start

1. Read the top-level `README.md`. It links to the per-subtree READMEs.
2. Decide which subtree your change belongs in (see "Subtree map" in the
   README). If it doesn't fit any subtree, open an issue first so we can
   agree on a home for it before you write code.
3. Check `prebuilt-info/` to see whether your change requires a new
   prebuilt artifact (kernel image, mozc tarball, BT firmware blob).
   If yes, you'll also need to update the release manifest.

## Style guide

- **Languages**: shell (POSIX `sh` or `ksh`, not bash extensions), C
  (OpenBSD style — `KNF`, see `style(9)`), Python 3 (PEP 8 with 4-space
  indent).
- **Filenames**: lowercase with hyphens (`do-thing.sh`), not snake_case,
  not CamelCase.
- **SPDX headers**: every new C / sh / py file in `install/`, `panctl/`,
  `harness/`, `battery/`, `logo/`, `tailscale-optional/`, and `docs/`
  must start with:

  ```
  # SPDX-License-Identifier: MIT
  # Copyright (c) 2026 4noha
  ```

  (or the C-comment equivalent). New files under `kernel-patches/`
  inherit ISC from OpenBSD; their patch headers should carry
  `License: ISC (inherits from OpenBSD upstream)`.

- **Commit messages**: imperative mood, ≤72 chars on the subject line,
  followed by a body that explains *why*. If a commit touches an
  upstream patch, name the patched file in the subject (e.g.
  `kernel-patches: bwfm_sdio: re-arm CMD5 fn_count on resume`).

- **No private placeholders**: the repository must never reference real
  Tailnet names, hostnames, IP addresses, user names, email addresses,
  MAC addresses, or SSIDs. Use the public placeholders defined in the
  sanitize design (`<your-tailnet>`, `<your-pomera-ts-ip>`,
  `<pomera-host>`, `4noha`, `<your-email>`,
  `<dm250-lan-ip>`, `<your-lan-gw>`, `XX:XX:XX:XX:XX:XX`,
  `<your-wifi-ssid>`, …) instead. CI will reject PRs that reintroduce
  the redacted strings.

## Patch format for `kernel-patches/`

- One logical change per patch file.
- File name: `NNN-shortname.patch` where `NNN` is a zero-padded index
  that determines apply order.
- First line of the patch body, before the `diff --git` block, is a
  comment block of the form:

  ```
  # Subject: <one-line summary>
  # Upstream-base: openbsd-src @ <commit hash>
  # Layered-on: jcs/openbsd-src rk3128 @ <commit hash>
  # License: ISC (inherits from OpenBSD upstream)
  # SPDX-FileContributor: 4noha
  ```

- The patch must apply cleanly with `patch -p1 -F0` from the OpenBSD
  source tree root.

## Build / test expectations

For changes under `install/`, `panctl/`, `harness/`, `battery/`,
`logo/`, `tailscale-optional/`:

- New shell scripts should pass `sh -n` and (where practical)
  `shellcheck`.
- New Python scripts should pass `python3 -m py_compile` and `ruff` (or
  `flake8`).
- New C should compile with `cc -Wall -Werror` against the OpenBSD
  base toolchain.

For kernel patch changes:

- The cross-build VM described in `docs/` must produce a `bsd.armv7`
  that boots on a DM250 (or an equivalent armv7 board if you don't
  have hardware). If you cannot test on hardware, say so in the PR
  description and tag a maintainer who can.
- For suspend/resume work, attach a log captured by the harness under
  `harness/` so reviewers can compare before/after.

## Reporting issues

Open a GitHub issue. Useful information to include:

- DM250 SKU (DM250 / DM250X / DM250XY) and firmware version
- OpenBSD release (`uname -a`)
- Whether you are running the prebuilt kernel from the Releases page,
  or a kernel you built yourself
- Steps to reproduce, including which commands you ran on the Mac side
  vs. on the pomera side (label them clearly — host confusion is the
  most common source of bug reports)
- Relevant excerpts from `/var/log/messages` and (for suspend/resume
  issues) the harness log

## Security

If you find a security issue (privilege escalation, leaked credentials
in a commit, etc.), please **do not** open a public issue. Instead,
contact the maintainer via the address in `README.md` and include a
proof of concept if you have one.

## Sign-off

By submitting a contribution you certify that you have the right to
license it under the terms of `LICENSE` (MIT) for project files, or the
terms of `LICENSE-ISC` for changes under `kernel-patches/`. Add a
`Signed-off-by:` line to your commits to make that explicit.
