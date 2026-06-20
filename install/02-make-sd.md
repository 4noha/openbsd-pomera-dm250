<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 02. OpenBSD インストール用 SD カードの準備

`01-backup-emmc.md` で eMMC バックアップを取ったら、同じ SD カードを
**OpenBSD インストール用**に作り直す。

> [!NOTE]
> snapshot のバージョン番号 (`79` = OpenBSD 7.9) は取得日 2026-05-26 時点。
> `files/armv7-snapshot/` の実ファイル名と本手順のバージョン番号は **必ず
> 揃える**こと。食い違っていたら snapshot を取り直す
> ([`PROVENANCE.md`](PROVENANCE.md) の URL 参照)。

## 2.1 経路の選択

SD prep には OpenBSD ホストが要る (`signify(1)` / `newfs(8)` / `disklabel(8)`
は OpenBSD でしか動かないため、macOS だけでは不可)。2 経路ある:

| 経路 | 想定 | 詳細 |
|---|---|---|
| **A. macOS で完結する自動化** (推奨) | Mac 上の qemu に OpenBSD installer を 使い捨てで起動して SD prep を完走 | [`../docs/build-vm.md`](../docs/build-vm.md) |
| **B. 既設 OpenBSD ホスト** | 別の OpenBSD 機/VM があるなら手動でも | §2.3〜§2.5 |

§2.3〜§2.5 は原典ベースの手動手順で、A の自動化に問題が出たときの再現
材料として残してある。

> [!NOTE]
> **`install/files/` の構成 (両経路共通)**
>
> 以降の手順で叩く `install/files/` には 2 系統のファイルが入る:
>
> - **first-party scripts** — `qemu-prep-sd.sh` / `driver.py` / `prep-sd.sh`
>   はリポ同梱。clone すればそのまま揃う。
> - **配布物バイナリ** — `armv7-snapshot/*.tgz`, `arm64-snapshot/install79.img`,
>   `dm250/{bsd,bsd.rd,uboot.img,logo.bmp,...}`, `firmware/bwfm-firmware-*.tgz`,
>   `firmware/bcm43438a1.hcd`, `dm250/backup-tool/...` などは権利・サイズの
>   都合でリポには含めない。[`PROVENANCE.md`](PROVENANCE.md) の URL から
>   各自取得して同 dir に展開する。
>
> 以降の手順で参照する `~/works/openbsd-pomera-dm250-staging/install/files/`
> は、本リポを clone して配布物を入れ終えた状態の `install/files/` の
> ことを指す。リポ clone 直下で作業しても、別ディレクトリに rsync して
> 作業しても (`rsync -a <repo>/install/files/ ~/works/.../staging/install/files/`)
> どちらでも構わない。

## 2.2 ファイル一式を OpenBSD ホストへ転送 (経路 B のみ)

```sh
# on mac
cd ~/works/openbsd-pomera-dm250-staging/install
rsync -av files/ openbsd-host:/home/4noha/dm250-install/
```

以降の SD カード作成手順は **OpenBSD ホスト上の `~/dm250-install/`** が起点。

## 2.3 macOS で完結する自動化パス (推奨)

実機 OpenBSD ホストの代わりに **macOS 上の qemu に OpenBSD installer を
使い捨てで起動**して、そのシェルから SD prep を完走させる方法が
[`../docs/build-vm.md`](../docs/build-vm.md) に整理されている。

```sh
# on mac
cd ~/works/openbsd-pomera-dm250-staging/install/files
sudo SD_DEV=/dev/disk4 ./qemu-prep-sd.sh
# → /tmp/dm250-runner.log に SD-PREP-COMPLETE が出れば完了
```

中身は `qemu-prep-sd.sh` (Mac ランチャー) + `driver.py` (qemu serial を
Python expect で駆動) + `prep-sd.sh` (VM 内 SD prep 本体)。動作要件と
原理は [`../docs/build-vm.md`](../docs/build-vm.md) を参照。

成功すれば SD カード作成は完了。SD カードを抜いて
[`03-recovery-mode.md`](03-recovery-mode.md) へ。

## 2.4 パーティションを切る (手動手順、OpenBSD ホスト用)

経路 B / 自動化に失敗したときの代替。

> [!WARNING]
> 下の `disklabel -E` 行は **modern OpenBSD では実行しないこと**。
> `fdisk -ygb` が GPT を書く際に partition `a` を自動生成するので、
> 続く `echo -e "a\n..."` は既存の `a` を再追加しようとして失敗し、
> 次に `b` を作って disklabel の意図と乖離する。jcs 原典の手順がそのまま
> 残してあるだけ。経路 A 推奨。詳細は
> [`../docs/build-vm.md`](../docs/build-vm.md) §1.6 trap #5。

```sh
# on openbsd-host
# SD カードのデバイス名を確認
dmesg | tail
# → sd1 at scsibus3 ... "SD/MMC" ... など

# GPT で切る: 100MB EFI (MSDOS) + 残り OpenBSD (FFS)
doas fdisk -ygb 204800 sd1
# ↓ modern fdisk -g 後は不要 (上の WARNING 参照)
# doas sh -c 'echo -e "a\n\n\n\n\nw\nx" | disklabel -E sd1'
doas newfs /dev/rsd1a
doas newfs_msdos /dev/rsd1i
```

## 2.5 EFI パーティション側を埋める (手動)

```sh
# on openbsd-host
doas mount /dev/sd1i /mnt

# BOOTARM.EFI を /efi/boot/ に配置
doas mkdir -p /mnt/efi/boot
doas cp ~/dm250-install/armv7-snapshot/BOOTARM.EFI /mnt/efi/boot/
doas signify -C -x ~/dm250-install/armv7-snapshot/SHA256.sig \
  -p /etc/signify/openbsd-79-base.pub ~/dm250-install/armv7-snapshot/BOOTARM.EFI

# DM250 用 U-Boot と工場リカバリ用 _sdboot.sh
doas cp ~/dm250-install/dm250/uboot.img   /mnt/uboot.img
doas cp ~/dm250-install/dm250/_sdboot.sh  /mnt/_sdboot.sh

# 任意: 起動ロゴ (詳細は ../logo/)
doas cp ~/dm250-install/dm250/logo.bmp    /mnt/logo.bmp

doas umount /mnt
```

## 2.6 OpenBSD パーティション側に snapshot 一式を置く (手動)

```sh
# on openbsd-host
doas mount /dev/sd1a /mnt
doas mkdir -p /mnt/openbsd

# snapshot セット
doas cp ~/dm250-install/armv7-snapshot/{SHA256.sig,INSTALL.armv7,\
base79.tgz,comp79.tgz,game79.tgz,man79.tgz,\
xbase79.tgz,xfont79.tgz,xserv79.tgz,xshare79.tgz} /mnt/

# 署名検証
cd /mnt
doas signify -C -x SHA256.sig *.tgz
cd

# bwfm ファームウェア
doas cp ~/dm250-install/firmware/{SHA256.sig,bwfm-firmware-20200316.1.3p5.tgz} /mnt/
# (bwfm 側 SHA256.sig は別鍵で署名されているので、ここでは検証スキップ
#  インストーラが自動取得時に検証する)

# DM250 用カスタムカーネルとインストーラ ramdisk
doas cp ~/dm250-install/dm250/bsd     /mnt/bsd
doas cp ~/dm250-install/dm250/bsd.rd  /mnt/bsd.rd

doas umount /mnt
```

SD カード作成完了。抜いて DM250 へ → [`03-recovery-mode.md`](03-recovery-mode.md)。

## 入手元

`dm250/uboot.img` `_sdboot.sh` `bsd` `bsd.rd` `logo.bmp` の出所と SHA256 は
[`PROVENANCE.md`](PROVENANCE.md) 参照。OpenBSD snapshot の URL も同じファイル。
