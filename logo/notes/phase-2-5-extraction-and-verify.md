# Phase 2-5 — extract, verify, convert, inspect

Date: 2026-05-29.
Builds on [Phase 1](phase-1-rsce-header.md), using the offsets and sizes
derived there.

## Phase 2 — `dd` extraction

```sh
# on mac
SRC=~/Backups/pomera-dm250-emmc-YYYYMMDD/mmcblk0p3.img
cd ./out

dd if=$SRC of=rk-kernel.dtb   bs=1 skip=2048   count=73798
dd if=$SRC of=logo.bmp        bs=1 skip=76288  count=44072
dd if=$SRC of=logo_kernel.bmp bs=1 skip=120832 count=2942
```

Result:

```
44072 bytes transferred  -> logo.bmp
2942 bytes transferred   -> logo_kernel.bmp
73798 bytes transferred  -> rk-kernel.dtb
```

Sizes match the header declarations exactly. No short reads or trailing
garbage.

## Phase 3 — `file(1)` format check

```
logo.bmp:        PC bitmap, Adobe Photoshop with alpha channel mask, 500 x 44 x 16,
                 cbSize 44072, bits offset 70
logo_kernel.bmp: PC bitmap, Windows 95/NT4 and newer format, 500 x 44 x 1,
                 cbSize 2942, bits offset 126
rk-kernel.dtb:   Device Tree Blob version 17, size=73798, boot CPU=0,
                 string block size=4182, DT structure block size=69560
```

### Dimensions — 500 x 44 px

Rockchip boot logos are typically a centred banner rather than a
full-screen image. 500 x 44 is consistent with that: u-boot is expected
to centre the banner on the 1024 x 600 LVDS panel. No upscaling needed.

### Colour depth

- `logo.bmp`: **16-bit (RGB565-ish)** with the Adobe Photoshop alpha
  channel mask extension — what the factory firmware (Linux) u-boot
  expects.
- `logo_kernel.bmp`: **1-bit monochrome** in Windows 3.x format — kept
  small for the kernel-stage / charging splash.

### `identify -verbose logo.bmp` (excerpt)

```
Geometry: 500x44+0+0
Type: Palette
Depth: 8/16-bit
Channels: 3.0  (Red/Green/Blue each 16-bit)
Colorspace: sRGB
```

ImageMagick reads it as 16-bit per channel because of the alpha mask
extension. To hand it to the patched u-boot, converting to **24-bit BGR
(Windows 3.x format)** is the safe option.

## Phase 4 — convert to 24-bit BMP

```sh
# on mac
magick logo.bmp        -define bmp:format=bmp3 -depth 8 logo-24bit.bmp
magick logo_kernel.bmp -define bmp:format=bmp3 -depth 8 logo_kernel-24bit.bmp
```

Result:

```
logo-24bit.bmp:        PC bitmap, Windows 3.x format, 500 x 44 x 24,
                       image size 66000, cbSize 66054, bits offset 54
logo_kernel-24bit.bmp: PC bitmap, Windows 3.x format, 500 x 44 x 1,
                       image size 2816, cbSize 2878, bits offset 62
```

- `logo-24bit.bmp` (66 KB) matches the format of the puffy logo
  distributed for the DM250 (24-bit Win 3.x). Drop-in replacement.
- `logo_kernel-24bit.bmp` (3 KB) — kept at 1-bit. Promoting it to 24-bit
  inflates size 5x for no extra information.

## Phase 5 — visual inspection

```sh
# on mac
magick logo.bmp -resize 200% logo-preview-2x.png
open logo-preview-2x.png    # Preview.app, optional
```

The wordmark renders as a **white "pomera" mark on a black background**
(the KING JIM wordmark). Preview.app shows the right orientation and no
colour shifts.

## Artefacts (`out/`, gitignored)

```
out/
|-- logo.bmp                 extracted (16-bit, 44072B)
|-- logo_kernel.bmp          extracted (1-bit, 2942B)
|-- rk-kernel.dtb            factory dtb (bonus, 73798B)
|-- logo-24bit.bmp           converted (24-bit, 66054B) <- deploy candidate
|-- logo_kernel-24bit.bmp    converted (24-bit, 2878B)
`-- logo-preview-2x.png      2x preview (34139B)
```

## Hand-off

The remaining task is the compose-and-deploy run by
[`build-deploy-logo.sh`](../build-deploy-logo.sh), which combines the
extracted wordmark with the puffy logo shipped under
[`install/files/dm250/`](../../install/) and writes the resulting BMP to
the device's EFI partition.

Trade-offs to weigh before deploying:

- Keep the factory wordmark: visually closer to stock pomera.
- Keep puffy only: less of the KING JIM wordmark in your own derivative
  image — keep the OpenBSD nature visible.
- Add a mascot of your own: makes the splash personal without depending
  on a third-party trademark.
