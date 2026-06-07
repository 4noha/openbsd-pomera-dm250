# logo/

Tools to build the **boot splash logo** for the DM250 after installing
OpenBSD via the [`install/`](../install/) flow. The boot logo is loaded by
u-boot from the EFI partition (`/dev/sd1i`, `msdosfs`, mounted as
`/logo.bmp`).

## Goals

- Reproducibly assemble a 1024x600 24-bit BMP that the u-boot logo loader
  on the DM250 (Rockchip RK3128) can render.
- Compose three building blocks:
  1. The **"pomera" wordmark** extracted from each user's own eMMC backup
     (never bundled, never redistributed).
  2. The **OpenBSD puffy logo** distributed by Joshua Stein for the DM250
     (`install/files/dm250/logo.bmp`, see provenance there).
  3. An **optional user-supplied mascot** image.

Writing the BMP back to the device is offered as an opt-in `--deploy-host`
step in [`build-deploy-logo.sh`](build-deploy-logo.sh).

## Trademark / licensing notes

> The KING JIM "pomera" wordmark is a third-party trademark of KING JIM
> Co., Ltd. This project deliberately **does not** ship the wordmark
> image. The build script re-extracts it from the user's own DM250 eMMC
> backup on every run, so each user only ever reads bytes from hardware
> they own. Do not publish or redistribute the composed output BMP.

- The puffy logo (`install/files/dm250/logo.bmp`) originates from
  jcs.org/dm250 (Joshua Stein's DM250 work). Credit and provenance are
  tracked under [`install/`](../install/).
- A mascot, if provided, is the user's responsibility. The script accepts
  a filesystem path; the file is `scp`'d once to the device and never
  committed.

## Layout

```
logo/
|-- README.md
|-- build-deploy-logo.sh    end-to-end build + optional deploy
|-- notes/
|   |-- phase-1-rsce-header.md         RSCE header analysis (offsets, sizes)
|   `-- phase-2-5-extraction-and-verify.md   extract / verify / convert / inspect
`-- out/                    build artefacts (gitignored)
    |-- logo.bmp                       16-bit RGB565 + alpha, 500x44, 44072B
    |-- logo_kernel.bmp                1-bit mono, 500x44, 2942B
    |-- rk-kernel.dtb                  factory DT blob (bonus, 73798B)
    |-- logo-24bit.bmp                 24-bit BGR, 500x44 (candidate)
    |-- logo-preview-2x.png            1000x88 preview
    `-- logo-deploy-1024x600.bmp       final 24-bit Win 3.x BMP to deploy
```

The `out/` directory is meant to stay out of version control; the
extracted wordmark and the composed derivative belong on your local disk
only.

## Background

The DM250 ships with a Rockchip-style boot splash embedded inside a
**resource partition** (`mmcblk0p3.img`, ~6 MB, magic `RSCE`). The
partition holds three entries:

| name              | offset    | size    | use                     |
|-------------------|----------:|--------:|-------------------------|
| `rk-kernel.dtb`   |  2048 B   | 73798 B | factory DT blob         |
| `logo.bmp`        | 76288 B   | 44072 B | boot splash (500x44)    |
| `logo_kernel.bmp` | 120832 B  |  2942 B | kernel-stage mini-logo  |

See [`notes/phase-1-rsce-header.md`](notes/phase-1-rsce-header.md) for
the byte-level walkthrough.

The DM250 LVDS panel is **1024x600**. u-boot centres the 500x44 logo on
that canvas, so the composed deploy BMP renders the wordmark in the
middle with puffy (and the optional mascot) tucked into the
bottom-right corner.

## Usage

```sh
# on mac (build-only, puffy only)
./build-deploy-logo.sh

# on mac (with mascot, build-only)
./build-deploy-logo.sh --mascot /path/to/mascot.png

# on mac (build + deploy via Tailscale magic DNS)
./build-deploy-logo.sh --mascot /path/to/mascot.png \
    --deploy-host <pomera-host>

# on mac (build + deploy over LAN)
./build-deploy-logo.sh --mascot /path/to/mascot.png \
    --deploy-host <your-pomera-user>@<dm250-lan-ip>
```

### Prerequisites

- macOS with ImageMagick (`brew install imagemagick`).
- An eMMC backup directory of the form
  `~/Backups/pomera-dm250-emmc-YYYYMMDD/` containing `mmcblk0p3.img`.
  Pass `--backup-dir DIR` to override the auto-detection (latest match).
  See [`install/`](../install/) for how to take the backup itself.
- For `--deploy-host`: an ssh key that already authenticates as the
  remote user, plus `doas` access (wheel group) and an EFI partition at
  `/dev/sd1i` (`msdosfs`) on the target.

## Verification (after deploy)

```sh
# on <pomera-host>
doas mount -t msdos /dev/sd1i /mnt
doas md5 /mnt/logo.bmp     # compare with the md5 the build script printed
doas umount /mnt
reboot
```

The new boot logo should show on the next power cycle.

## Pointers to related subtrees

- [`install/`](../install/) — overall DM250 OpenBSD install flow; the
  eMMC backup procedure that produces `mmcblk0p3.img` lives there.
- [`kernel-patches/`](../kernel-patches/) — kernel changes required to
  bring the DM250 to a usable state. The boot logo path is independent
  of these patches but the patched kernel is what eventually runs.

## Out of scope

- Redistribution of the extracted wordmark in any form (trademark / copyright).
- Reverse engineering of the rest of the factory firmware.
- DM200 or other Pomera models (only DM250 is targeted here).
