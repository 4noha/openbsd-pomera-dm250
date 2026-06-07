<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 01. eMMC バックアップ (最初にやること)

eMMC 全体を SD カードに吸い出す。jcs.org のカスタム U-Boot を書く前に
**必ず**やる。工場 Linux に戻す唯一の道。

> [!CAUTION]
> ここから先は実機操作。バッテリー残量を満タン近くにしておく
> (バックアップ中に電源が落ちると面倒)。

ツール: ichinomoto 氏の「DM200/DM250 eMMC NAND バックアップ/リストア ツール
v0.2」。出典・SHA256 は [`PROVENANCE.md`](PROVENANCE.md) 参照。
取得済みの実体はこのリポでは `files/backup-tool/` 相当の位置に配置している
想定 (実バイナリは権利上リポに含めていない、PROVENANCE.md の URL から取得)。

## 1.1 SD カードを用意

- **FAT32 でフォーマット済み、16GB 以上** (DM250 の eMMC は 8GB だが、
  ファイルシステムオーバーヘッドも含めて 16GB 推奨)
- 新しめのカード (古いカードは書き込みが遅く、バックアップ完走に時間がかかる)

```sh
# on mac
diskutil list external
# → /dev/disk10 などを特定。サイズと "Windows_FAT_32" であることを確認
```

すでに FAT32 でフォーマット済みなら再フォーマット不要。既存ファイルが
あっても DM250 側のスクリプトは無視するが、念のため **`backup/` という
名前のディレクトリだけは事前に作らない**こと
(`_sdboot.sh` の判定でバックアップがスキップされる)。

## 1.2 バックアップツールを SD カードへコピー

```sh
# on mac
SRC=~/works/openbsd-pomera-dm250-staging/install/files/backup-tool/DM200_DM250_emmc_backup_restore_v0.2/backup
DST="/Volumes/NO NAME"   # SD カードのマウントポイント。実際の名前で置換

cp -R "$SRC"/_sdboot.sh "$SRC"/backup.sh "$SRC"/res "$DST"/
dot_clean -m "$DST"      # macOS の ._* AppleDouble を除去 (任意)

# 確認: _sdboot.sh / backup.sh / res/imgs/{top.bin,bottom.bin,25/,27/} が見える
ls -la "$DST"
ls "$DST"/res/imgs/27/ | wc -l   # 27 と出れば OK
```

`.Spotlight-V100` `.fseventsd` は macOS が勝手に作るが DM250 側のスクリプト
は見ないので残してよい。

## 1.3 DM250 でリカバリブート

1. DM250 の電源ボタンを **長押し**して完全に電源 OFF
2. USB-C ケーブルが刺さっていたら抜く
3. SD カードを DM250 のスロットに挿す
4. **左 Alt + 右 Shift + 電源ボタン** を同時押し
5. POMERA ロゴが画面に出てから **約 3 秒間**、3 ボタンとも押したまま
   - 押す時間が長すぎると工場ハードウェアテストモードに入ってしまうので
     ロゴが出たら 3 秒数えて離す
6. 画面上下にプログレスバー風の画像が表示される。そのまま放置
7. DM250 の eMMC 全体 (8GB) を SD カードに書き出すので、SD カードの速度に
   よっては **10〜30 分** かかる
8. 完了すると自動でリブートし、通常の Pomera 画面 (メモ帳) に戻る

## 1.4 バックアップを Mac に取り込む

```sh
# on mac
diskutil mount disk10           # 番号は実際のものに
ls -la "/Volumes/NO NAME/backup/"
# → mmcblk0p1.img 〜 mmcblk0p27.img の 27 ファイルがあるはず

# Mac の外付け SSD 等、安全な場所にコピー (このリポには入れない)
BACKUP_DEST=~/Backups/pomera-dm250-emmc-$(date +%Y%m%d)
mkdir -p "$BACKUP_DEST"
rsync -av "/Volumes/NO NAME/backup/" "$BACKUP_DEST/"
ls "$BACKUP_DEST" | wc -l       # 27 と出れば OK

# サイズ確認: 合計 8GB 程度になるはず
du -sh "$BACKUP_DEST"

# (推奨) SHA256 を取って MANIFEST を作っておく — 後で再現性を担保
(cd "$BACKUP_DEST" && shasum -a 256 *.img > MANIFEST.sha256)
```

> [!IMPORTANT]
> リストアに最低限必要なのは **`mmcblk0p14.img` と `mmcblk0p15.img`**
> (工場 U-Boot とリカバリ kernel/initramfs に相当)。ただし他のパーティション
> (ユーザーデータ、設定など) も全部戻せるよう、27 個まとめて保管しておく。
>
> `mmcblk0p3.img` は工場ロゴ抽出にも使う (→ [`../logo/`](../logo/))。

## 1.5 SD カードを再利用するために掃除

これから同じ SD カードを OpenBSD インストール用に作り直すので、中身を
全部消す (バックアップ取得後、Mac 側に保管済みになっていることを確認して
から):

```sh
# on mac
# /Volumes/NO NAME 配下を再帰削除 (バックアップは別場所に逃がし済み)
rm -rf "/Volumes/NO NAME/"{_sdboot.sh,backup.sh,res,backup,backup.log}
ls -la "/Volumes/NO NAME"        # macOS 内部用ディレクトリ以外が空であること

diskutil eject disk10
```

OpenBSD インストール用の SD 作成は [`02-make-sd.md`](02-make-sd.md) へ。

## 復元したくなったら

工場 Linux に戻す手順は [`07-recovery.md`](07-recovery.md) 参照。
配布物 (`restore.sh` 等) の入手元は [`PROVENANCE.md`](PROVENANCE.md)。
