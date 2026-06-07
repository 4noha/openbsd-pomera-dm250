# kernel-patches/ — OpenBSD/armv7 (rk3128) kernel patches for pomera DM250

Source-level patches against the **jcs/openbsd-src `rk3128` branch**
(`https://github.com/jcs/openbsd-src`, branch `rk3128`) used to bring
the King Jim pomera DM250 (Rockchip RK3128, AP6212A / Broadcom
BCM43430) up far enough to run as an ssh thin-client.

> [!NOTE]
> **Base commit**: `<TBD: jcs/openbsd-src rk3128 HEAD SHA at clone time>`.
> The patches were captured against a `git clone --depth 1 -b rk3128`
> snapshot taken during the 2026-05 to 2026-06 work. Hunks use
> context-based addressing and small line-number drift (a few lines)
> is usually absorbed by `patch -l`; for non-trivial drift, re-roll
> hunks against the current `rk3128` HEAD.

> [!IMPORTANT]
> All patches in this directory modify OpenBSD source under `sys/` and
> therefore **inherit OpenBSD's ISC license**. See
> [`../LICENSE-ISC`](../LICENSE-ISC). Per-file ISC-inheritance notes
> are included in each patch header.

## License

- Patches under this directory: **ISC** (inherited from OpenBSD).
- The two BTstack patches (`btstack-*.patch`) modify
  BlueKitchen BTstack v1.6.2 and inherit its BSD-like license; see
  [`../NOTICE`](../NOTICE).
- The reference snapshot `bcmbt_fdt.c.patched` reproduces an OpenBSD
  source file modified by `bcmbt-delay-2s.patch` and retains its
  upstream copyright header (joshua stein / OpenBSD).

## File matrix

### Functional patches

| File | Target | Purpose | Status | Required |
|---|---|---|---|---|
| `bcmbt-delay-2s.patch` | `sys/dev/fdt/bcmbt_fdt.c` | Extend post-firmware HCI reset wait `250ms -> 2s` for AP6212A BT bring-up | Working but not sufficient on every bring-up; see notes below | Yes (BT) |
| `rkdrm-wsdisplay-smode.patch` | `sys/dev/fdt/rkdrm.c` | Accept `WSDISPLAYIO_SMODE` as a no-op so `mlterm-fb` can use `DUMBFB` and render CJK glyphs via cairo | Working | Optional (mlterm-fb / CJK) |
| `bus_space_notimpl-align-clang22.diff` | `sys/arch/arm/arm/bus_space_notimpl.S` | `.align 0` -> `.align 2` so the NOT_IMPL macro assembles cleanly under clang-22 | Working; build-prereq only | Required for cross-build VM (see [`../docs/`](../docs/)); not needed for native base-clang(19) build on the device |

### Suspend / resume work — WiFi / SDIO (frozen)

The full suspend / resume chain for the AP6212A SDIO WiFi was the
deepest rabbit hole in this project. The patches below build on each
other; the resulting v3 stack BUILDS clean (clang-22 cross-build,
armv7 GENERIC, ELF 32-bit ARM) but **was not validated end-to-end on
the device** — the workaround in production is "do not close the lid
while WiFi is up; turn WiFi off during BT-tether". Retained here so
the work can be picked up rather than redone.

| File | Target | Purpose | Status |
|---|---|---|---|
| `dwmmc-resume-pwrseq.diff` | `sys/dev/fdt/dwmmc.c` | Explicit `pwrseq_pre/post` on `DVACT_RESUME`. Resolves the original "error 60" (ETIMEDOUT). | Superseded by `dwmmc-resume-full-cycle.diff` |
| `dwmmc-resume-full-cycle.diff` | `sys/dev/fdt/dwmmc.c` | Drop `sc_vdd` to 0 + release `vmmc` so the next `bus_power()` runs the **full** cold-boot `regulator + pwrseq` sequence. Fixes "AP6212A reports only 1 SDIO function" after resume. | Built, partially validated |
| `bwfm-sdio-resume-guard.diff` | `sys/dev/sdmmc/if_bwfm_sdio.c` | NULL-guard `bwfm_sdio_task` against a half-initialised softc + `taskq_barrier(systq)` on detach. | Built, partial — eliminates one fault class only |
| `ieee80211-media-status-nullguard.diff` | `sys/net80211/ieee80211.c` | Guard `ieee80211_media_status()` against `NULL ic_bss` during the resume detach / re-attach window. | Built, applied as belt-and-braces |
| `bwfm-suspend-down.diff` | `sys/dev/sdmmc/if_bwfm_sdio.c` + `sys/net80211/ieee80211_node.c` | v3: register a `ca_activate` on `bwfm_sdio_ca`; on `DVACT_QUIESCE` tear net80211 down WITHOUT firmware I/O (cancel BA timeouts, free node tree, force `ic_state = S_INIT`, drop `IFF_UP|IFF_RUNNING`). v1 hung suspend because `bwfm_stop()` issued sync SDIO commands to an already-quiesced chip. | Built, unvalidated on device |

### Diagnostic-only

These are noisy, intended to localise a hang or a missing call path.
Apply only when chasing a regression; back out with `patch -p0 -R`
before production builds.

| File | Target | Purpose |
|---|---|---|
| `diag-suspend-verbose.diff` | `sys/kern/subr_suspend.c` + `sys/kern/subr_autoconf.c` | Phase markers + per-device `susp: <ACT> <devname>` before each `ca_activate` call |
| `bwfm-attach-diag.diff` | `sys/dev/sdmmc/if_bwfm_sdio.c` | Numbered `BWFMD NN` printfs across `match` / `attach` / `preinit` / `detach` / `activate` |

### BTstack patches (not kernel)

| File | Target | Purpose |
|---|---|---|
| `btstack-v1.6.2-openbsd-compat.patch` | BTstack v1.6.2 `port/posix-h4` build | OpenBSD strict POSIX: add `#include <errno.h>` and `<strings.h>` for `strncasecmp` |
| `btstack-config-disable-le-secure-for-pair-stability.patch` | BTstack `port/posix-h4/btstack_config.h` | Disable LE Secure Connections + Cross Transport Key Derivation to stabilise Just Works pairing against Android |

These two are not OpenBSD source patches; they live here for
co-location with the rest of the AP6212A / Broadcom plumbing. Apply
with `patch -p1 -d port/posix-h4` (config patch) or
`patch -p1 -d /path/to/btstack` (compat patch). License inheritance:
BlueKitchen BTstack BSD-like.

### Reference snapshots

| File | Notes |
|---|---|
| `bcmbt_fdt.c.patched` | Full text of `sys/dev/fdt/bcmbt_fdt.c` AFTER `bcmbt-delay-2s.patch` is applied. Retains upstream OpenBSD copyright header. Use for environments where `patch -l` cannot reconcile context drift — just copy this file over the upstream one. |

## Application order

Apply from the OpenBSD source root (`/usr/src` on the build VM) with
`patch -p0` unless noted otherwise. Order matters: later patches in
the suspend / resume chain depend on the earlier ones.

```sh
# on build VM (arm64 OpenBSD with clang-22), in /usr/src
# Stage 1 — required to build at all on cross-toolchain
patch -p0 < .../bus_space_notimpl-align-clang22.diff

# Stage 2 — BT bring-up (always required for thin-client role)
patch -p0 < .../bcmbt-delay-2s.patch

# Stage 3 — optional: mlterm-fb / CJK support
patch -p0 < .../rkdrm-wsdisplay-smode.patch

# Stage 4 — WiFi suspend/resume v3 stack (apply as a set or skip the set)
patch -p0 < .../dwmmc-resume-full-cycle.diff       # replaces dwmmc-resume-pwrseq.diff
patch -p0 < .../bwfm-sdio-resume-guard.diff
patch -p0 < .../ieee80211-media-status-nullguard.diff
patch -p0 < .../bwfm-suspend-down.diff             # v3

# Stage 5 — diagnostic, only while chasing a regression
patch -p0 < .../diag-suspend-verbose.diff
patch -p0 < .../bwfm-attach-diag.diff
```

If applying the v3 WiFi stack, **do not also apply
`dwmmc-resume-pwrseq.diff`** — `dwmmc-resume-full-cycle.diff` replaces
it and both touching `dwmmc_activate(DVACT_RESUME)` will conflict.

## Dependency graph

```
  bus_space_notimpl-align-clang22.diff   (cross-build prereq, no runtime effect)
  bcmbt-delay-2s.patch                   (independent)
  rkdrm-wsdisplay-smode.patch            (independent)

  ── WiFi suspend/resume v3 stack ──
  dwmmc-resume-pwrseq.diff               (superseded -> next)
     └─ dwmmc-resume-full-cycle.diff     (foundation: chip enumerates both SDIO functions on resume)
        └─ bwfm-sdio-resume-guard.diff   (softc + task safety during the re-attach window)
           └─ ieee80211-media-status-nullguard.diff   (ioctl race safety)
              └─ bwfm-suspend-down.diff  (v3: net80211 teardown WITHOUT firmware I/O on QUIESCE)

  ── Diagnostics (orthogonal) ──
  diag-suspend-verbose.diff              (phase + per-device markers)
  bwfm-attach-diag.diff                  (numbered BWFMD NN markers)
```

## Prebuilt kernel correspondence

The companion `prebuilt-info/` directory documents which patch
combination produced each prebuilt artifact:

| Prebuilt artifact | Patches applied |
|---|---|
| `bsd.armv7.jcs-original` | None — vanilla jcs/openbsd-src `rk3128` GENERIC build |
| `bsd.armv7.delay-2s` | `bcmbt-delay-2s.patch` only |
| `bsd.armv7.wfmfix2` (v2) | `bcmbt-delay-2s.patch` + `dwmmc-resume-pwrseq.diff` + `bwfm-sdio-resume-guard.diff` |
| `bsd.armv7.wfmfix3` (v3) | `bcmbt-delay-2s.patch` + `dwmmc-resume-full-cycle.diff` + `bwfm-sdio-resume-guard.diff` + `ieee80211-media-status-nullguard.diff` + `bwfm-suspend-down.diff` |

See `prebuilt-info/` for the exact SHA256 of each binary; the on-device
deployment procedure (hardlink-break, `/bsd.jcs-original` rollback
slot, etc.) lives in `install/`.

## Status notes (operational caveats)

1. **BT bring-up is REAL but not always single-shot.** `bcmbt-delay-2s`
   makes the AP6212A reachable past the firmware download; HCI Reset
   then sometimes still needs a retry depending on the unit. The
   day-to-day workaround is automation in `panctl/` (retry on
   `pairing complete status=0x05`), not more kernel work.

2. **WiFi suspend/resume is FROZEN.** The v3 stack builds clean and
   each step is documented end-to-end (root cause, history, why v1
   hung suspend, why ic_bss preservation is safe), but device
   validation was not finished. The reproducible failure mode without
   any of these patches is "close lid -> resume -> kernel data abort
   in `bwfm_update_node` or `ieee80211_ba_del`" followed by the
   resume-crash -> unclean reboot loop occasionally damaging the eMMC
   `/` partition (recoverable via single-user `fsck`). The recommended
   operating mode while this is frozen is **do not close the lid with
   WiFi up; turn WiFi off when on BT-tether**. The patches stay in the
   tree to make resumption straightforward — pair them with the
   `harness/` persistent-log methodology (see `../harness/`) when
   picking the thread back up.

3. **`/bsd` and `/bsd.booted` are a hardlink on a fresh jcs install.**
   Replacing `/bsd` with `cp` would silently overwrite `/bsd.booted`'s
   inode contents and destroy the rollback slot. Always
   `mv /bsd /bsd.jcs-original` first to break the link before
   `install`-ing a new kernel. See `install/` for the safe-replace
   procedure.

4. **`reorder_kernel` must be disabled at install time** so the boot
   loader doesn't relink an upstream `/bsd` over the patched one. See
   `install/`.

5. **Line numbers in patches drift with `rk3128` HEAD.** Hunks were
   captured against a specific clone in the 2026-05..06 window. Use
   `patch -l` for whitespace tolerance, or copy `bcmbt_fdt.c.patched`
   directly when context refuses to reconcile.

## TODO

- [ ] Record `BASE_COMMIT` SHA (current placeholder: `<TBD>`).
- [ ] Re-validate the v3 WiFi suspend/resume stack on hardware with
      `harness/` collecting `susp:` + `BWFMD NN` traces.
- [ ] Try post-firmware HCI Reset retry loop (3..5 attempts) as an
      alternative to a longer single `delay()`.
- [ ] Evaluate alternative BT firmware blobs (armbian
      `BCM43430A1.hcd`, Cypress official) and build a matrix vs.
      `bcmbt-delay-2s` variants (1s / 2s / 5s).

## Cross-references

- Build VM / cross-compile pipeline: [`../docs/`](../docs/) (clang-22
  wrapper, obj/ ownership gotcha)
- Suspend/resume harness + persistent-log methodology:
  [`../harness/`](../harness/)
- `/bsd` replacement procedure, hardlink break, rollback slot:
  [`../install/`](../install/)
- BTstack RFCOMM client + pairing automation: [`../panctl/`](../panctl/)
- Prebuilt kernel SHA256 + provenance: [`../prebuilt-info/`](../prebuilt-info/)
