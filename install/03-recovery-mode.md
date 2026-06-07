<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 03. リカバリモードで U-Boot を差し替え

> [!WARNING]
> ここで eMMC の先頭が DM250 用カスタム U-Boot に書き換わる。失敗または
> 中断すると工場 Linux が起動しなくなる。
> [`01-backup-emmc.md`](01-backup-emmc.md) を取ってあること。

## 3.1 リカバリ起動の手順

1. DM250 を **電源 OFF** (USB-C も外す)
2. [`02-make-sd.md`](02-make-sd.md) で作成した SD カードを挿す
3. **Right Shift + Left Alt + Power** を同時に押し続ける
4. Pomera ロゴが出てから **約 2 秒**で全部離す
   (長すぎると factory hardware test に入ってしまう)
5. ロゴが消え、リカバリカーネルが SD の `_sdboot.sh` を実行する
   - 画面には何も出ない
   - 実体: 工場 firmware と現行 U-Boot を SD にバックアップしてから、
     SD 上の `uboot.img` を eMMC に書く
6. 約 30 秒待つと自動でリブート

## 3.2 リブート後の確認

リブート後は **DM250 用の新 U-Boot で立ち上がり**、SD の EFI パーティション
から `BOOTARM.EFI` を読み込んで OpenBSD bootloader が起動する。
画面には bootloader プロンプトが出る:

```text
No EFI variables loaded
Loading Boot0000 'mmc 0' failed
Booting: Label: mmc 1 Device path: /VenHw(...)/SD(1)/SD(0)
disks: sd0* sd1
>> OpenBSD/armv7 BOOTARM 1.23
boot>
```

ここまで来たら U-Boot 移行は成功。
次は [`04-openbsd-install.md`](04-openbsd-install.md) でインストーラを走らせる。

## 失敗したとき

`boot>` プロンプトに到達しない (画面真っ黒、もしくは Pomera ロゴで止まる) 場合:

- SD カードが正しく差さっているか確認
- [`02-make-sd.md`](02-make-sd.md) §2.5 の `BOOTARM.EFI` の signify 検証を再確認
- U-Boot 自体が書き込まれていないだけなら、SD カードから再度
  `_sdboot.sh` を呼び直すために再度リカバリ起動 (左 Alt + 右 Shift + Power) を試す
- それでも復旧しなければ [`07-recovery.md`](07-recovery.md) の MaskROM 経由で
  工場 U-Boot を書き戻す
