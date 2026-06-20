<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# Build VM and cross-build environments

This is the host-Mac (Apple Silicon) side of the toolchain — what you need on the Mac in order to drive OpenBSD/armv7 work on the pomera DM250. Verified working on 2026-05-28.

There are **two distinct environments** with two different jobs:

| Purpose | Engine | Location in repo | Section |
|---|---|---|---|
| **SD card prep** (see install guide §3) | qemu-system-aarch64 + HVF + install79.img | `install/files/` (`qemu-prep-sd.sh` / `driver.py` / `prep-sd.sh`) | §1 below |
| **panctl etc. cross-build** | Apple clang + lld + sysroot | `panctl/.crossroot/openbsd-armv7-7.9/` | §2 below |

The arm64 OpenBSD VM used for **kernel** cross-builds (route C in [`cross-build-kernel.md`](cross-build-kernel.md)) is a third environment — a long-lived VM, not the throwaway installer used for SD prep. It is described in §3 below.

---

## 1. qemu VM for SD card creation

### 1.1 What it is for

The SD-card-prep procedure (install guide §3) assumes an OpenBSD host, but macOS lacks `signify(1)`, `newfs(8)` (FFS), and `disklabel(8)`. So instead of building a permanent OpenBSD host VM, we **boot the OpenBSD installer raw under qemu on the Mac for each SD prep job**, drive its shell from the host, and let it halt when finished.

### 1.2 Host requirements

| Item | Value confirmed working |
|---|---|
| Mac arch | **arm64 (Apple Silicon)** — verified on an Apple M1 Ultra |
| HVF | **enabled** (`sysctl kern.hv_support` → 1) |
| qemu | `brew install qemu` (11.0.1 confirmed) |
| EDK2 firmware | `/opt/homebrew/share/qemu/edk2-aarch64-code.fd` (ships with qemu) |
| sudo | required for qemu raw access to `/dev/disk*` |
| python3 | for `driver.py` (Python 3.x) and the `python3 -m http.server` loopback |

### 1.3 File layout under `install/files/`

`install/files/` には 2 系統のファイルが入る:

- **first-party scripts** — checked into this repo. `git clone` でそのまま揃う。
- **distribution binaries** — このリポには含めない。
  [`../install/PROVENANCE.md`](../install/PROVENANCE.md) の URL から各自取得して
  同 dir に展開する (権利・サイズの都合)。

```
install/files/
├─ qemu-prep-sd.sh        [in-tree] Mac-side launcher (HTTP server + qemu + driver, under sudo)
├─ driver.py              [in-tree] Drives the qemu serial socket from Python: parses
│                                   installer output, sends commands, runs prep-sd.sh
├─ prep-sd.sh             [in-tree] Runs inside the qemu OpenBSD installer ramdisk shell.
│                                   Self-contained: fdisk / newfs / mount / ftp deploy of sets.
│
├─ arm64-snapshot/        [PROVENANCE: cdn.openbsd.org/.../snapshots/arm64/]
│   ├─ install79.img        OpenBSD 7.9 arm64 install image (660 MB, bootable raw) — qemu VM boot
│   ├─ SHA256, SHA256.sig
│   └─ miniroot79.img       (unused — minimal, no GUI variant)
│
├─ armv7-snapshot/        [PROVENANCE: cdn.openbsd.org/.../snapshots/armv7/]
│   ├─ SHA256.sig, INSTALL.armv7, BOOTARM.EFI
│   └─ base79.tgz comp79.tgz game79.tgz man79.tgz xbase79.tgz xfont79.tgz xserv79.tgz xshare79.tgz
│
├─ dm250/                 [PROVENANCE: jcs.org/2026/04/09/openbsd-dm250]
│   ├─ bsd                  DM250 用カスタム armv7 kernel
│   ├─ bsd.rd               DM250 用 install ramdisk
│   ├─ uboot.img, _sdboot.sh, restore.sh
│   ├─ rk3128_ddr_300MHz_v2.12.bin, u-boot-ums.bin
│   └─ logo.bmp             起動ロゴ
│
├─ firmware/              [PROVENANCE: bwfm = signify-signed tarball / hcd = armbian]
│   ├─ bwfm-firmware-20200316.1.3p5.tgz, SHA256.sig
│   └─ bcm43438a1.hcd       (実機展開時に BCM4343A1.hcd へリネーム)
│
└─ dm250/backup-tool/...  [PROVENANCE: ekesete.net/log/?p=9504, DM200_DM250_emmc_backup_restore_v0.2/]
```

> [!NOTE]
> The 3 first-party scripts are the only files this repo physically checks in
> under `install/files/`. Everything else is fetched per `PROVENANCE.md` URLs.
> A fresh clone is therefore **incomplete on purpose** — see
> [`../install/02-make-sd.md`](../install/02-make-sd.md) §2.1 for the
> "in-tree vs fetched" note in the user-facing install flow.

### 1.4 Data flow

```
Mac (macOS)
+- python3 -m http.server 8000 --bind 127.0.0.1   <- serves install/files/
+- qemu-system-aarch64 (sudo)
    +- -drive install79.img        (boot disk = VM sd0)
    +- -drive /dev/disk4           (SD card pass-through = VM sd1)
    +- -netdev user,id=net0        (user-mode net: 10.0.2.2 -> Mac loopback)
    +- -serial unix:/tmp/dm250-qemu-serial.sock,server,nowait
        +- driver.py (sudo)
            +- socket recv: parse installer output
            +- socket send: command stream
                +- s                                    (Welcome -> shell)
                +- ifconfig vio0 inet autoconf + dhcpleased   (DHCP up)
                +- ftp http://10.0.2.2:8000/prep-sd.sh
                +- YES=1 SD=sd1 sh /tmp/prep-sd.sh
                    +- ftp sets from the loopback HTTP server
                    +- write SD (VM sd1)
                    +- halt -p
```

### 1.5 Run procedure

```sh
# on mac
# 1. Insert the SD card into the Mac. FAT32 / empty / anything — it will be wiped.
# 2. Find its device number.
diskutil list external | grep 'external, physical'   # e.g. /dev/disk4

# 3. Warm up sudo (if running through an automation harness, pipe via stdin
#    rather than printing the password to terminal).
sudo -v

# 4. Run the launcher.
cd <repo>/install/files
sudo SD_DEV=/dev/disk4 ./qemu-prep-sd.sh
```

Pass a different `SD_DEV=` if your SD is on another device node. Progress is logged at `/tmp/dm250-runner.log`. `driver.py` waits for the marker `SD-PREP-COMPLETE`, then issues `halt -p` to the VM and exits.

### 1.6 Gotchas (kept for the record)

The installer ramdisk is **not** the same environment as a normal OpenBSD install with `base*.tgz` / `comp*.tgz` extracted — it is much smaller. The following bit us:

| # | Trap | Workaround |
|---|---|---|
| 1 | No `printf` binary (and not a builtin) | use `echo` / `cat <<EOF` |
| 2 | No `tail` binary | drop the preflight tail; not strictly needed |
| 3 | `/dev/sd1*` nodes missing initially | prepend `cd /dev && sh ./MAKEDEV sd1` to the script |
| 4 | `signify -Cqp -x` prints `-V` usage (broken `-C` mode or pubkey path issue on the ramdisk) | **skip signature verification** in the VM; transport integrity is assured by the loopback HTTP serving files that the Mac side already verified with `shasum -c` |
| 5 | Modern `fdisk -g` creates partition `a` automatically when it lays out GPT, so the original guide's `echo -e "a\n..\nw\nx" \| disklabel -E` fails on the already-present `a` and tries to add `b` | **remove the disklabel step entirely** |
| 6 | qemu `-display none` captures stdio so the harness cannot drive it | replace with `-serial unix:SOCK,server,nowait`, drive via Python expect over the UNIX socket |
| 7 | `dhclient` was removed in OpenBSD 7.x in favor of `dhcpleased` | use `ifconfig vio0 inet autoconf` + `dhcpleased &` |
| 8 | On the **real DM250** during installer, the JIS keyboard layout is not recognized, so `>` / `\` etc. cannot be typed | avoid shell redirection; e.g. use `touch file` instead of `echo -n > file` |
| 9 | macOS will not auto-mount the FAT16 EFI partition of the SD | data is still written correctly; you can verify with `sudo dd if=/dev/disk4s1 \| strings` |
| 10 | We worried that installer-time vs. post-install disk ordering (SD=sd0, eMMC=sd1) might flip. It did **not** — ordering matched between installer and the installed system | — |

### 1.7 Reproducibility checklist

- [ ] SD is empty / safe to wipe (and `SD_DEV` matches)
- [ ] sudo is cached (`sudo -v` just before running)
- [ ] `install/files/dm250/`, `armv7-snapshot/`, `firmware/` are all populated (the download phase has been completed)
- [ ] arm64 `install79.img` SHA256 verified with `shasum -c SHA256`
- [ ] qemu and EDK2 firmware paths are correct
- [ ] No other qemu running (PID collisions)
- [ ] `/tmp/dm250-*.sock` / log from a previous run cleaned up

If `/tmp/dm250-runner.log` reaches **`SD-PREP-COMPLETE`** it worked. If not, the tail of the log shows which ramdisk-side step blew up.

### 1.8 Approximate timings

- qemu boot → installer welcome prompt: ~60 s
- DHCP + ftp of `prep-sd.sh`: a few seconds
- fdisk + newfs + ftp of all sets (~280 MB) to SD: ~30 s (HVF + qemu user-mode loopback gives ~9 MB/s)
- Total (one successful pass): **~2 minutes**
- Add the same time for each failed iteration during debugging.

### 1.9 Git status

`prep-sd.sh` / `driver.py` / `qemu-prep-sd.sh` live under `install/files/` as plain checked-in files. The 660 MB `arm64-snapshot/` binaries are intentionally **not** tracked; re-fetch them from `cdn.openbsd.org/pub/OpenBSD/snapshots/arm64/`. See `install/PROVENANCE.md` for the URLs.

---

## 2. Cross-build sysroot (for panctl)

The authoritative documentation is `panctl/.crossroot/README.md`. Here we only describe the relationship with the `install/` side:

- Sysroot: `panctl/.crossroot/openbsd-armv7-7.9/`.
- Source data is **the same** `base*.tgz` + `comp*.tgz` snapshot pair as the one consumed by `install/files/armv7-snapshot/` (both fetched from `cdn.openbsd.org/.../snapshots/armv7/`). The snapshot is consumed **twice**: once as installer sets, once as a cross-build sysroot.
- Additional package: `libusb1-1.0.29.tgz` from `cdn.openbsd.org/pub/OpenBSD/7.9/packages/arm/`, unpacked into `.crossroot/openbsd-armv7-7.9/usr/{include,lib}/`.
- Host tools required on the Mac:
  - Apple clang (`xcode-select --install`)
  - `brew install lld` (links OpenBSD ELF)
  - `brew install llvm` (for `llvm-strip` etc.)
- Build script: `panctl/tools/build-armv7.sh`.
- Output: `panctl/build-armv7/*_armv7{,.stripped}` — ARM EABI5 OpenBSD ELF, copy directly to the device.

When OpenBSD 7.9 is superseded (e.g. by 7.10), both sysroot and `install/files/armv7-snapshot/` must be bumped in lockstep — `base79`/`comp79`/`libusb1-1.0.29` → `base80`/`comp80`/`libusb1-x.x.x`. Bump procedure is documented in `panctl/.crossroot/README.md`.

---

## 3. arm64 OpenBSD VM for kernel cross-builds

The third environment is the long-lived arm64 OpenBSD VM used by [`cross-build-kernel.md`](cross-build-kernel.md). It is **separate** from the throwaway installer in §1 — it stays around and is reused across kernel and mozc cross-builds.

- VM: OpenBSD/arm64 7.9 under qemu + HVF.
- Reached over ssh at `localhost:2222` as `builder@`. `doas permit nopass :wheel`.
- Backing disk image, host launcher (`boot-host.sh`), and `known_hosts` live under the build-env directory of this repo. The exact location is intentionally not hard-coded in this document because it lives in the same tree as the cross-build artifacts; follow the relative path in `cross-build-kernel.md`.
- Inside: `llvm-22` package, `/usr/src` cloned from jcs/openbsd-src branch `rk3128`.

Housekeeping notes (the VM's `/usr` is small, ~3.1 GB):

- Prune `/usr/ports/distfiles/*`, `/usr/ports/packages/aarch64`, and old cross-build trees if you run out of space.
- One VM can serve both the kernel cross-build and any armv7 user-space cross-build (mozc, etc.).

---

## 4. Related files

- [`../install/02-make-sd.md`](../install/02-make-sd.md) — SD card creation procedure (uses the §1 VM)
- [`../install/README.md`](../install/README.md) — full install index (01..07)
- [`../install/PROVENANCE.md`](../install/PROVENANCE.md) — origin of files used by the install steps
- [`cross-build-kernel.md`](cross-build-kernel.md) — armv7 kernel cross-build (uses the §3 VM)
