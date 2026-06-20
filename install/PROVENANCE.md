<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# PROVENANCE — install/ 配下で参照する外部配布物

このリポは外部配布物 (DM250 用 U-Boot、OpenBSD snapshot、bwfm/BT firmware
等) を **配布元 URL から取得して使う**前提。リポ自体には権利上の理由で
バイナリを含めないものが多い。本ファイルは取得元 URL と SHA256 を一覧化
する一次資料。

> [!NOTE]
> SHA256 が一致しない場合、配布元側の差し替えが起きている可能性がある。
> その場合は本リポの該当バージョンを更新してから手順を再実行すること。

## 一次資料 (記事)

- joshua stein, *Installing OpenBSD on the Pomera DM250*  
  URL: <https://jcs.org/2026/04/09/openbsd-dm250>  
  扱い: 出典明記の上、必要箇所の抜粋のみ。全文転載しない。

- joshua stein, *Disassembling the King Jim Pomera DM250*  
  URL: <https://jcs.org/2025/03/14/dm250>  
  扱い: TP501 短絡時の参考のみ。

## jcs.org DM250 配布物

`install/files/dm250/` 相当の位置に配置する想定。配布元:

| ファイル | 入手元 | 用途 | SHA256 |
|---|---|---|---|
| `uboot.img` | jcs.org/dm250 配布 | DM250 用カスタム U-Boot | (要記入: `sha256` 取得後) |
| `_sdboot.sh` | 同上 | リカバリブート時のスクリプト | (要記入) |
| `bsd` | 同上 (jcs/openbsd-src rk3128 build) | DM250 用カスタム kernel | (要記入) |
| `bsd.rd` | 同上 | DM250 用 install ramdisk | (要記入) |
| `logo.bmp` | 同上 (puffy ロゴ、24-bit BMP) | U-Boot 起動ロゴ | (要記入) |
| `rk3128_ddr_300MHz_v2.12.bin` | Rockchip 配布 (jcs 経由) | MaskROM 初期化 | (要記入) |
| `u-boot-ums.bin` | 同上 | MaskROM 経由 UMS 化 U-Boot | (要記入) |
| `restore.sh` | jcs 配布 | eMMC リストアスクリプト | (要記入) |

> 取得後に `(cd install/files/dm250 && shasum -a 256 *) > install/files/dm250/SHA256` で
> ハッシュを記録し、本ファイルの SHA256 列を更新する運用。

## OpenBSD armv7 snapshot

OpenBSD project 配布。本手順は **OpenBSD 7.9 (snapshot date 2026-05-26)** で
検証済。

- 配布元: <https://ftp.openbsd.org/pub/OpenBSD/snapshots/armv7/>
  または mirror (`<mirror>/pub/OpenBSD/snapshots/armv7/`)

`install/files/armv7-snapshot/` に置くファイル:

| ファイル | 用途 | SHA256 |
|---|---|---|
| `SHA256.sig` | 同梱の signify 署名 | (snapshot 由来) |
| `INSTALL.armv7` | インストール手順テキスト | (snapshot 由来) |
| `BOOTARM.EFI` | EFI 上の OpenBSD bootloader | (snapshot 由来) |
| `base79.tgz` | OpenBSD base set | (snapshot 由来) |
| `comp79.tgz` | compiler set | (snapshot 由来) |
| `game79.tgz` | games set (使わなくても入れる) | (snapshot 由来) |
| `man79.tgz` | man pages | (snapshot 由来) |
| `xbase79.tgz` | X base (X 自体は起動しない) | (snapshot 由来) |
| `xfont79.tgz` | X fonts | (snapshot 由来) |
| `xserv79.tgz` | X server | (snapshot 由来) |
| `xshare79.tgz` | X shared | (snapshot 由来) |

> `signify -C -p /etc/signify/openbsd-79-base.pub -x SHA256.sig *.tgz` で
> snapshot 同梱の SHA256.sig を検証してから使う。
>
> 7.10 にアップグレードする場合はファイル名を `*80.tgz` に揃え、本ファイルの
> バージョン番号も同時に更新する。

## OpenBSD arm64 snapshot (qemu installer)

`install/files/arm64-snapshot/install79.img` は Mac 上の qemu-system-aarch64
(HVF) で起動する OpenBSD/arm64 installer ramdisk。SD prep の経路 A
([`02-make-sd.md`](02-make-sd.md) §2.3, [`../docs/build-vm.md`](../docs/build-vm.md) §1)
専用で、pomera 実機には入らない (実機は armv7)。

- 配布元: <https://ftp.openbsd.org/pub/OpenBSD/snapshots/arm64/>
  または mirror (`<mirror>/pub/OpenBSD/snapshots/arm64/`)

`install/files/arm64-snapshot/` に置くファイル:

| ファイル | 用途 | SHA256 |
|---|---|---|
| `install79.img` | OpenBSD arm64 installer (660 MB, bootable raw) | (snapshot 由来) |
| `SHA256` | snapshot 同梱の SHA256 一覧 | (snapshot 由来) |
| `SHA256.sig` | 同梱 signify 署名 | (snapshot 由来) |

> 取得後:
> ```sh
> cd install/files/arm64-snapshot
> shasum -a 256 -c SHA256       # install79.img を検証
> # 署名も検証する場合:
> signify -C -p /etc/signify/openbsd-79-base.pub -x SHA256.sig install79.img
> ```
>
> 7.10 にアップグレードする場合はファイル名 `install80.img` に揃え、
> 同時に armv7 snapshot のバージョン番号 (§ OpenBSD armv7 snapshot) も
> 同期して更新する。

## bwfm firmware (Broadcom)

- 配布元: OpenBSD signify 署名付きの bwfm firmware tarball  
  通常は `pkg_add` 経由だが、SD 上に置いてインストーラに食わせる流れ。

| ファイル | 用途 | SHA256 |
|---|---|---|
| `bwfm-firmware-20200316.1.3p5.tgz` | bwfm 用 firmware (nvram は別途) | (snapshot 鍵で署名) |
| `SHA256.sig` | 同梱署名 | — |

nvram (`nvram_ap6212a.txt`) は工場 U-Boot/`_sdboot.sh` が SD カードに退避
する個体由来データ。リポには含めず、ユーザーの個体から取り出して使う
([`05-post-install.md`](05-post-install.md) §5.1)。

## BT patchram firmware (armbian ap6212)

- 配布元:
  <https://raw.githubusercontent.com/armbian/firmware/master/ap6212/bcm43438a1.hcd>

| ファイル | 用途 | SHA256 |
|---|---|---|
| `bcm43438a1.hcd` | DM250 用 BCM43438 patchram | `d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0` |

> kernel が期待する名前は `BCM4343A1.hcd` (`0` 抜き)。`install` 時に
> リネームして `/etc/firmware/BCM4343A1.hcd` に置く
> ([`05-post-install.md`](05-post-install.md) §5.2)。

armbian リポ自体のライセンスは BSD 系。リポへの再配布をする場合は元
ライセンス表示を残すこと。

## ekesete eMMC backup tool

- 配布元: <https://www.ekesete.net/log/?p=9504>  
  作者: ichinomoto  
  名称: 「DM200/DM250 eMMC NAND バックアップ/リストア ツール v0.2」

| ファイル | 用途 | SHA256 |
|---|---|---|
| `DM200_DM250_emmc_backup_restore_v0.2/backup/_sdboot.sh` | リカバリブート起点 | (取得時に記録) |
| `DM200_DM250_emmc_backup_restore_v0.2/backup/backup.sh` | eMMC dump スクリプト | (取得時に記録) |
| `DM200_DM250_emmc_backup_restore_v0.2/backup/res/...` | 画像等のリソース | (取得時に記録) |

リポでの再配布は配布元の利用条件に従う。本リポは URL リファレンスのみで
バイナリは含めない方針 (個別取得が必要)。

## Prebuilt artifacts (本プロジェクトのビルド成果物)

カスタムカーネルや mlterm-fb、mozc-server などのビルド済みバイナリは
GitHub Releases で配布する。SHA256 と asset 名は
[`../prebuilt-info/`](../prebuilt-info/) のマニフェスト参照。

主要 asset:

- `bsd.armv7.delay-2s` — `bcmbt_fdt.c` の delay 拡張カスタム kernel
- `bsd.armv7.jcs-original` — patch なしの jcs build (比較用)
- `BCM4343A1.hcd.armbian-ap6212` — armbian bcm43438a1.hcd を rename したもの
- `mlterm-fb.armv7` — DM250 表示修正版 mlterm-fb

## 取得後の verify 推奨フロー

```sh
# on mac
cd ~/works/openbsd-pomera-dm250-staging/install/files

# OpenBSD armv7 snapshot
( cd armv7-snapshot && signify -C -p /etc/signify/openbsd-79-base.pub -x SHA256.sig *.tgz )
# OpenBSD arm64 snapshot (qemu installer)
( cd arm64-snapshot && shasum -a 256 -c SHA256 )

# 自前で記録した SHA256
shasum -a 256 -c dm250/SHA256
```

mismatch があれば配布元の差し替え or 改ざんを疑い、再取得 + 本ファイルの
更新を実施。
