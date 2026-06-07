# Phase 1 — `mmcblk0p3.img` RSCE header analysis

Date: 2026-05-29
Input: `~/Backups/pomera-dm250-emmc-YYYYMMDD/mmcblk0p3.img` (6 MB)

## Header (sector 0, 0x000-0x01F)

```
00000000: 5253 4345 0000 0000 0101 0100 0300 0000  RSCE............
00000010: 0000 0000 0000 0000 0000 0000 0000 0000  ................
```

| offset | size | value | meaning |
|---|---:|---|---|
| 0x00 | 4 | `RSCE` | magic |
| 0x04 | 2 | 0x0000 | resource_ptn_version |
| 0x06 | 2 | 0x0000 | index_tbl_version |
| 0x08 | 1 | 0x01 | header_size (sectors) |
| 0x09 | 1 | 0x01 | tbl_offset (sectors from RSCE start) |
| 0x0A | 1 | 0x01 | tbl_entry_size (sectors) |
| 0x0B | 1 | 0x00 | (padding) |
| 0x0C | 4 | 0x00000003 | **tbl_num = 3 entries** |
| 0x10-0x1FF | - | 0 | padding |

Layout: sector 0 = header, sector 1 = entry[0], sector 2 = entry[1],
sector 3 = entry[2], sector 4+ = data area.

## Entry format (each 1 sector = 512B)

```
0x000-0x003 : 'E' 'N' 'T' 'R'             magic
0x004-0x0FF : name (252 bytes, NUL terminated)
0x100-0x103 : (padding, 0)
0x104-0x107 : offset (uint32 LE, sectors from RSCE start)
0x108-0x10B : size   (uint32 LE, bytes)
0x10C-0x1FF : padding
```

> The README draft initially assumed "name 252B + hash 32B + offset/size"
> but the on-disk image has no hash region — name is followed by 4B of
> padding, then a 4B offset, then a 4B size. A minor variant of the
> rkbin layout.

## Entry values

### entry[0] — `rk-kernel.dtb`

```
00000200: 454e 5452 726b 2d6b 6572 6e65 6c2e 6474  ENTRrk-kernel.dt
00000210: 6200                                     b.
...
00000300: 0000 0000 0400 0000 4620 0100 0000 0000  ........F ......
```

| field | value (LE) | computed |
|---|---|---|
| offset | 0x00000004 | sector 4 = **byte 2048** |
| size | 0x00012046 | **73798 bytes (~72 KB)** |

The device tree blob lives in the resource partition by Rockchip
convention. It is not strictly related to the boot logo, but it is worth
extracting; diffing the factory dtb against jcs.org's customised one
gives hints about the factory dts.

### entry[1] — `logo.bmp`

```
00000400: 454e 5452 6c6f 676f 2e62 6d70 0000 0000  ENTRlogo.bmp....
...
00000500: 0000 0000 9500 0000 28ac 0000 0000 0000  ........(.......
```

| field | value (LE) | computed |
|---|---|---|
| offset | 0x00000095 | sector 149 = **byte 76288** |
| size | 0x0000ac28 | **44072 bytes (~43 KB)** |

### entry[2] — `logo_kernel.bmp`

```
00000600: 454e 5452 6c6f 676f 5f6b 6572 6e65 6c2e  ENTRlogo_kernel.
00000610: 626d 70                                  bmp
...
00000700: 0000 0000 ec00 0000 7e0b 0000 0000 0000  ........~.......
```

| field | value (LE) | computed |
|---|---|---|
| offset | 0x000000ec | sector 236 = **byte 120832** |
| size | 0x00000b7e | **2942 bytes (~3 KB)** |

## Layout consistency check

- entry[0]: starts at sector 4, occupies ceil(73798/512)=145 sectors,
  ends at sector 148 (inclusive).
- entry[1]: starts at sector 149 — adjacent, no gap.
  - 149 + ceil(44072/512) = 149 + 87 = sector 236 (end exclusive).
- entry[2]: starts at sector 236 — adjacent, no gap.
  - 236 + ceil(2942/512) = 236 + 6 = sector 242 (end exclusive).

Three entries packed densely, fully consistent with the header.

## Extraction commands (used in Phase 2)

```sh
# on mac
SRC=~/Backups/pomera-dm250-emmc-YYYYMMDD/mmcblk0p3.img
cd ./out

dd if=$SRC of=rk-kernel.dtb   bs=1 skip=2048   count=73798
dd if=$SRC of=logo.bmp        bs=1 skip=76288  count=44072
dd if=$SRC of=logo_kernel.bmp bs=1 skip=120832 count=2942
```
