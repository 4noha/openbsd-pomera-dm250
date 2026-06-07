# Changelog

All notable changes to this project are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project tries to honour [Semantic
Versioning](https://semver.org/spec/v2.0.0.html) for tagged releases of
the project tooling.

Kernel and firmware artifacts have their own version cadence and are
tracked separately under GitHub Releases (see `prebuilt-info/`); this
file records changes to the *repository contents* (scripts, patches,
docs).

## [Unreleased]

### Added

- Initial public layout of the repository:
  - Top-level project files: `README.md`, `LICENSE`, `LICENSE-ISC`,
    `NOTICE`, `CONTRIBUTING.md`, this `CHANGELOG.md`, `.gitignore`,
    `.github/workflows/release.yml`.
  - `install/` — OpenBSD installation and post-install steps for the
    DM250 family (Wi-Fi, BT, keymap, eMMC backup pointers).
  - `kernel-patches/` — patch series layered on top of the
    `jcs/openbsd-src` rk3128 branch, including the bwfm SDIO resume
    fix, ieee80211 BA cleanup on suspend, and the dwmmc / sdmmc
    removable handling adjustment.
  - `panctl/` — Bluetooth PAN tether daemon and netwatchd glue (default
    route fail-over to BT when Wi-Fi is gone).
  - `harness/` — suspend / resume debug harness (persistent log across
    crashes, scripted exercise of the WiFi-resume path).
  - `logo/` — boot logo extractor and replacement helper, sourcing the
    factory logo from the user's own eMMC backup.
  - `battery/` — OCV-corrected PS1 helper for the RK818 fuel gauge.
  - `tailscale-optional/` — thin-client wiring for reaching a home box
    via Tailscale over the BT tether.
  - `docs/` — cross-build VM bring-up, kernel cross-build instructions,
    artifact manifest pointers.
  - `prebuilt-info/` — index of GitHub Release artifacts (custom
    kernel, BT firmware, mozc-server, mlterm-fb DM250 build), with
    SHA256 hashes.

### Notes

- This is a publishing snapshot. The original private working tree
  lives elsewhere; this repository is the result of a sanitisation
  pass that replaces personal identifiers, hostnames, IP addresses,
  and similar values with `<placeholders>` (see `CONTRIBUTING.md`).
- No tagged release yet. The first tag (`v0.1.0`) will be cut once
  every subtree has at least a `README.md` and the
  `.github/workflows/release.yml` pipeline has been exercised end to
  end against a throwaway release.

[Unreleased]: https://github.com/<your-name>/openbsd-pomera-dm250/commits/main
