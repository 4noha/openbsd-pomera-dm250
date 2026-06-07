<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# DM250 custom kernel — armv7 cross-build (route C)

Native kernel builds on the DM250 (RK3128, single-core) take ~73 minutes and the build load destabilizes the device. **Cross-compiling armv7 GENERIC under an arm64 OpenBSD VM with clang-22** finishes in 5–6 minutes. This document covers the VM setup, the build steps, the patches that go into the custom kernel, and deploy/rollback to the device.

This is route C in the routes summary (the other routes — native build on device, or reusing a prebuilt binary — are described elsewhere in this repo, under `prebuilt-info/` and the install guide). Route C is the recommended path.

> [!IMPORTANT]
> The DM250 crashes under build load. **Always cross-build the kernel under this VM. Do not run native kernel builds on the device.**

Execution-location tags used in this document:

- `# on mac` — the host Mac where this repository lives
- `# on builder-vm` — the arm64 OpenBSD qemu VM described in [`build-vm.md`](build-vm.md) (reached via `builder@localhost:2222`)
- `# on pomera (installed)` — the DM250 after OpenBSD installation

---

## 0. Build VM

The same VM used for any other armv7 cross-build (e.g. mozc) is reused — there is no need to spin up a separate one. See [`build-vm.md`](build-vm.md) for the canonical setup.

Recap of what `build-vm.md` covers:

- VM: OpenBSD/arm64 7.9, qemu + HVF acceleration. ssh is exposed at `localhost:2222` as `builder@`, with `doas permit nopass :wheel`.
- Disk image and known_hosts live under the build-env directory (see `build-vm.md` for exact paths under this repo).
- Boot is started by the launcher script described in `build-vm.md`.

```sh
# on mac — canonical command to enter the VM
ssh -p 2222 \
  -o UserKnownHostsFile=<path-to>/build-env/disk/known_hosts \
  builder@localhost
```

Prerequisites inside the VM:

- `llvm-22` package (provides `clang-22`, `llvm-ar-22`, `llvm-nm-22`, `llvm-strip-22` in `/usr/local/bin/`).
- `/usr/src` is a clone of **jcs/openbsd-src branch `rk3128`**. If absent: `git clone --depth 1 -b rk3128 https://github.com/jcs/openbsd-src.git /usr/src`.
- `/usr` is tight (~3.1 GB on this VM). If you run out, prune `/usr/ports/distfiles/*`, `/usr/ports/packages/aarch64`, and any older cross-build trees. See `build-vm.md` for housekeeping notes.

---

## 1. Cross-compiler wrapper

OpenBSD's kernel Makefile invokes `cc`. We swap that for a wrapper that points at clang-22 targeting armv7.

```sh
# on builder-vm
mkdir -p ~/bin
cat > ~/bin/cc <<'EOF'
#!/bin/sh
exec /usr/local/bin/clang-22 -target arm-unknown-openbsd7.9 \
  -Wno-uninitialized-const-pointer "$@" -Wno-error
EOF
chmod +x ~/bin/cc
```

Why each flag:

- `-target arm-unknown-openbsd7.9` — produce armv7 output.
- `-Wno-uninitialized-const-pointer` / `-Wno-error` — clang-22 is stricter than base clang (19). One spot in `sys/arch/armv7/exynos/crosec.c:90` (passing an uninitialized `req` as a const pointer) trips `-Werror`. That driver is unrelated to the DM250, so we keep the warning but do not let it kill the build.

> [!NOTE]
> The native build route (route B) uses base clang (19) and needs neither this wrapper nor the clang-22 alignment prereq patch below. Both are **specific to the clang-22 cross-build path**.

---

## 2. clang-22-specific prereq patch

clang-22's integrated assembler is stricter than base clang's; one armv7 assembly site does not assemble as-is.

- [`kernel-patches/bus_space_notimpl-align-clang22.diff`](../kernel-patches/bus_space_notimpl-align-clang22.diff) — change `.align 0` to `.align 2` at the tail of the `NOT_IMPL` macro in `sys/arch/arm/arm/bus_space_notimpl.S`. Without alignment after odd-length `.asciz`, the next stub's `adr` target becomes misaligned and the build fails with "Relocation not aligned". The change is functionally harmless (just adds a byte of padding).

```sh
# on builder-vm
cd /usr/src
patch -p0 < ~/patches/bus_space_notimpl-align-clang22.diff   # ship the .diff from kernel-patches/
```

---

## 3. Feature patches (the custom kernel content)

The patches under `kernel-patches/` are applied on top of jcs/rk3128. Order is independent.

| Patch | Target | Purpose | Status |
|---|---|---|---|
| `bcmbt-delay-2s.patch` | `sys/dev/fdt/bcmbt_fdt.c` | Extend post-firmware HCI reset delay from 250 ms → 2 s. Required for reliable BT bring-up. | landed |
| `dwmmc-resume-pwrseq.diff` | `sys/dev/fdt/dwmmc.c` | On resume, power-cycle the SDIO (Wi-Fi) chip via pwrseq. Removes the "error 60" failure. | partial |
| `bwfm-sdio-resume-guard.diff` | `sys/dev/sdmmc/if_bwfm_sdio.c` | NULL-deref guard + detach barrier for the resume re-attach race. | partial |

> [!CAUTION]
> **The Wi-Fi-resume patches (`dwmmc-resume-pwrseq` + `bwfm-sdio-resume-guard`) are not a full fix.** They eliminate the "error 60 → task NULL deref" symptom, but the resume crash itself just shifts further down the stack and still occurs. Repeated resume-crash → unclean reboot cycles will eventually corrupt the eMMC root filesystem and require a single-user `fsck`.
>
> **Current operational workaround**: if you need Wi-Fi, do not close the lid. If you need to close the lid, switch to BT-tether and turn Wi-Fi off. These two patches are kept in the repository so the running kernel can be rebuilt and so the resume work can be picked back up later. Only `bcmbt-delay-2s` is required and stable in current use. If you do not care about Wi-Fi resume, you can drop the other two.

```sh
# on builder-vm — apply the patches you want from kernel-patches/
cd /usr/src
patch -p0 < ~/patches/bcmbt-delay-2s.patch          # check each diff header for the right -p
patch -p0 < ~/patches/dwmmc-resume-pwrseq.diff
patch -p0 < ~/patches/bwfm-sdio-resume-guard.diff
```

> Diff paths begin with `sys/dev/...` so apply from `/usr/src` with `-p0`. If hunks drift, use `patch -l` to absorb whitespace or edit the target files directly. Line numbers track the jcs/rk3128 commit.

---

## 4. Build

```sh
# on builder-vm
cat > ~/build-wfm-kernel.sh <<'EOF'
#!/bin/sh
set -e
export PATH=$HOME/bin:$PATH        # put ~/bin/cc (clang-22 wrapper) first
export BSDOBJDIR=/home/builder/usrobj
cd /usr/src/sys/arch/armv7/conf
doas config GENERIC
cd /usr/src/sys/arch/armv7/compile/GENERIC
make obj
make
EOF
chmod +x ~/build-wfm-kernel.sh

~/build-wfm-kernel.sh 2>&1 | tee /tmp/kbuild.log
```

- Wall time: 5–6 minutes (vs 73 minutes for native on the device).
- Artifact: `/usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd` (`file` should report `ELF 32-bit LSB executable, ARM`). Size around 7.5 MB.
- The `ctfstrip` step renames the unstripped image to `bsd.gdb` and leaves the stripped `bsd` next to it.

```sh
# on builder-vm — sanity check
ls -l /usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd
file /usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd
```

---

## 5. Deploy

VM → Mac → pomera. There is no direct route from the VM to the device.

```sh
# on mac
scp -P 2222 \
  -o UserKnownHostsFile=<path-to>/build-env/disk/known_hosts \
  builder@localhost:/usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd /tmp/bsd.armv7.new

scp /tmp/bsd.armv7.new <pomera-host>:/tmp/        # <pomera-host> = Tailscale alias from ssh config
```

```sh
# on pomera (installed)
# Back up the running kernel under a known name (break any hardlink to /bsd.booted).
# Pick a fresh backup name if /bsd.before-wfm-resume already exists.
doas mv /bsd /bsd.before-wfm-resume        # first run only; later use /bsd.prev or similar
doas install -m 0700 -o root -g wheel /tmp/bsd.armv7.new /bsd
doas reboot                                # do NOT use `reboot -n` — disk flush race,
                                           # see harness/ notes
```

Post-reboot verification:

```sh
# on pomera (installed)
sysctl kern.version | head -1              # build timestamp distinguishes old vs new
dmesg | grep -E 'bcmbt|bwfm|sdmmc'         # bcmbt address (BT) / bwfm attach
dmesg | grep -i 'error 60' || echo "no error 60"
```

---

## 6. Rollback

```text
# on pomera — at the bootloader, select the saved kernel
boot> boot /bsd.before-wfm-resume
```

To make the rollback permanent:

```sh
# on pomera (installed)
doas mv /bsd /bsd.wfm-frozen
doas install -m 0700 -o root -g wheel /bsd.before-wfm-resume /bsd
doas reboot
```

> Watch out for the hardlink between `/bsd` and `/bsd.booted`. Always `mv` the running kernel to a backup name first to break the link, then `install`/`cp` the new image. Otherwise you can clobber the running kernel in place.

---

## 7. Reproducibility checklist

- [ ] VM comes up at `builder@localhost:2222` (see `build-vm.md`)
- [ ] `~/bin/cc` wrapper points at clang-22 (§1)
- [ ] `/usr/src` is on the jcs/rk3128 branch
- [ ] `bus_space_notimpl-align-clang22.diff` applied (clang-22 prereq, §2)
- [ ] Feature patches applied as desired (at minimum `bcmbt-delay-2s`; add `dwmmc` + `bwfm` guard only if chasing Wi-Fi resume, §3)
- [ ] `make` produces `obj/bsd` (§4)
- [ ] On the device: back up old kernel (`/bsd.before-wfm-resume`) → install new one → normal `reboot` (§5)
- [ ] `dmesg` shows the bcmbt address, bwfm attach, and no "error 60"
