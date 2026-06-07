<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 07. リカバリ

工場 Linux に戻す or U-Boot が起動しなくなったとき。
FFS 不整合で single-user に落ちたとき。MaskROM 経由のフル復元。

## 7.1 電源が入らない / U-Boot が起動しない

1. 電源ボタン **10 秒**長押しで完全電源 OFF
2. 数秒置いて電源ボタンを **数秒**押して起動を試す
3. 数秒押しても画面バックライトが点かない場合、RK3128 は **MaskROM モード**
   に入っているはず

## 7.2 MaskROM 経由で USB Mass Storage 化

Mac から `xrock` で操作する。

```sh
# on mac
# xrock 導入 (未導入なら)
brew install libusb
git clone https://github.com/xboot/xrock && cd xrock && make && sudo make install

# DM250 と Mac を USB-C で接続
ioreg -p IOUSB | grep -i rockchip   # "vendor 0x2207" が見えれば MaskROM
```

```sh
# on mac
cd ~/works/openbsd-pomera-dm250-staging/install/files/dm250
sudo xrock reset maskrom
sudo xrock maskrom rk3128_ddr_300MHz_v2.12.bin u-boot-ums.bin
```

`rk3128_ddr_300MHz_v2.12.bin` / `u-boot-ums.bin` の入手元と SHA256 は
[`PROVENANCE.md`](PROVENANCE.md) 参照。

成功すると DM250 画面に eMMC 情報が出て、Mac に **USB Mass Storage** として
eMMC が見える (`diskutil list` で 7.4GB 程度の外付けディスクが現れる)。

## 7.3 工場 Linux への復元

[`01-backup-emmc.md`](01-backup-emmc.md) でバックアップを取っていれば、
jcs.org の `restore.sh` で書き戻せる。`restore.sh` は **OpenBSD ホスト**で
動かす想定 (`/dev/rsd3c` のような raw デバイスを引数に取る)。

macOS の `/dev/rdiskN` でも `dd` のオフセット指定は互換だが、`restore.sh`
の中身を読んで sed 等で `mmcblk` 部を `rdiskN` に読み替えた方が安全。
中身は単純な `dd` の連続:

```sh
# files/dm250/restore.sh の中身を確認してから手で叩くのが安全
less ~/works/openbsd-pomera-dm250-staging/install/files/dm250/restore.sh
```

## 7.4 TP501 ショート (最終手段)

U-Boot が動くせいで MaskROM に落ちないとき、eMMC チップ脇の **TP501 テスト
パッド**を GND と短絡しながら電源 ON すると強制的に MaskROM に入る。詳細・
写真は外部記事 (jcs.org の DM250 分解記事) を参照。本リポでは引用のみ。

## 7.5 起動時 fsck 失敗で single-user に落ちた (FFS 不整合)

不正シャットダウンの蓄積 (WiFi+resume クラッシュの反復、電源強制断、build
中の hang 等) で eMMC `/`(sd1a) の FFS が **preen で直せない不整合**を起こすと、
起動が single-user に落ちて手動 fsck を要求する。画面例:

```text
/dev/sd1a (xxxxxxxx.a): UNEXPECTED INCONSISTENCY; RUN fsck_ffs MANUALLY.
Automatic file system check failed; help!
Enter pathname of shell or RETURN for sh:
```

(`INCORRECT BLOCK COUNT` / `PARTIALLY TRUNCATED INODE` 等が併記される)

**復旧 (本体コンソールで) — データは基本消えない:**

```sh
# on pomera (installed) — single-user プロンプトで
# 1. RETURN(Enter) を押して # シェルへ
# 2. / を手動 fsck (出る修復プロンプトは全部 yes)
fsck -y /dev/sd1a
#    "UNEXPECTED INCONSISTENCY" がまた出たら同じコマンドを 2〜3 回、
#    正常サマリ(** N files, ... free)だけになるまで繰り返す (fsck は数回で収束)
# 3. 残りの FS も一応修復
fsck -y
# 4. 通常起動へ
reboot
```

`fsck -y` を回しても収束しない／特定 inode が壊れている場合は、§7.2 の
MaskROM 経由で eMMC を Mac に見せ、§7.3 のバックアップから当該パーティション
を書き戻す。

> [!NOTE]
> 主な誘発源は **「WiFi(bwfm0) が up のまま蓋を閉じて suspend → resume」で
> 起きるカーネルクラッシュ**。 カーネルパッチは
> [`../kernel-patches/`](../kernel-patches/) にあるが **未根治・凍結**。
> 再発を防ぐ実運用は「WiFi を活かすなら蓋を閉じない」「BT-tether 運用時は
> WiFi off」。BT(UART)・SD・eMMC は resume を越えるので、WiFi off なら
> 蓋スリープは安全。
>
> resume の追い込みハーネスは [`../harness/`](../harness/)。

## 7.6 kernel パニックループ

`/bsd.old` (post-install で取ってあるべきバックアップ) から uboot
single-user で復元、もしくは SD で boot して fix:

```sh
# on pomera (installed) — single-user で
doas mv /bsd /bsd.panic
doas install -m 0700 -o root -g wheel /bsd.old /bsd
doas reboot
```

`/bsd.old` を取っていなかった場合は SD カードから `bsd.rd` で boot して
shell に落ち、SD の `/mnt2/bsd` (= jcs カスタムカーネル) を `/mnt/bsd` に
コピーし直す ([`04-openbsd-install.md`](04-openbsd-install.md) §4.3 と同じ手順)。

## 付録: 工場 U-Boot を完全に書き換える前にやり残しがないか

`_sdboot.sh` が走ると **eMMC の工場 U-Boot は上書きされる**ので、その前に
以下を確認:

- [ ] [`01-backup-emmc.md`](01-backup-emmc.md) のバックアップを取った
- [ ] バックアップを Mac の外付け SSD 等、安全な場所にコピーした
- [ ] SD カードの内容を `ls /mnt` (マウント後) で目視確認
  - EFI 側: `efi/boot/BOOTARM.EFI`, `uboot.img`, `_sdboot.sh`, `logo.bmp`
  - OpenBSD 側: `bsd`, `bsd.rd`, `base79.tgz` ほか sets, `bwfm-firmware-*.tgz`
- [ ] バッテリー残量が十分 (バッテリー切れ + USB-C 給電のみだと書き込み中に
      落ちて文鎮化のリスク)
