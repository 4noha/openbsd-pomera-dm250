<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 04. OpenBSD インストール

[`03-recovery-mode.md`](03-recovery-mode.md) で `boot>` プロンプトに到達した
状態から続ける。

## 4.1 bsd.rd を起動

```text
# on pomera (recovery -> bootloader)
boot> b bsd.rd
```

`cannot open sd0a:/etc/random.seed` の警告は無視してよい。インストーラが
起動する。

## 4.2 インストーラ対話

- `(I)nstall / (U)pgrade / ...?` → `i`
- root disk → `?` で一覧確認。**eMMC は `sd1` (7.3G)**、SD カードが `sd0`。
  インストール先は `sd1`
- パーティション初期化 → `whole` (MBR を作り直す。EFI を 16MB オフセットに
  して、Rockchip ID block と U-Boot を踏まない)
- パーティションレイアウト → auto-allocate でも 1 ルート + swap でも可
- セットの所在 → `disk` → `not mounted?` → `n` → デバイス `sd0` →
  パーティション `a` → パス `/`
- signify の警告 (snapshot 鍵で署名された旧 sets の場合) は無視
- bwfm firmware は自動で拾われてインストールされる

## 4.3 カスタムカーネルを上書き

インストール末尾で **再リンクされたカーネルは上流版**になっているので、
SD の `bsd` (DM250 用) で上書きする。

```sh
# on pomera (installer, after install complete)
Exit to (S)hell, (H)alt or (R)eboot? [reboot] s

# SD カードの OpenBSD パーティションをマウントしてカーネルをコピー
mount /dev/sd0a /mnt2
cp /mnt2/bsd /mnt/bsd

# 次回起動時に reorder_kernel が走るとまた上流に戻ってしまうので無効化
mv /mnt/usr/libexec/reorder_kernel /mnt/usr/libexec/reorder_kernel.disabled
echo -n > /mnt/usr/libexec/reorder_kernel
chmod +x /mnt/usr/libexec/reorder_kernel

# ここで reboot
reboot
```

> [!NOTE]
> `bsd` は jcs/openbsd-src の rk3128 branch ベースに、本リポの
> [`../kernel-patches/`](../kernel-patches/) を当てた kernel。
> Bluetooth (`bcmbt_fdt.c`) の `delay()` 拡張パッチが入った version 推奨。

## 4.4 初回起動

リブート後、OpenBSD のログイン画面が出る。SD カードは挿したままでよい
(`05-post-install.md` で `/usr/local` を SD に逃がす運用にする場合は必須)。

ログインしたら次の章: [`05-post-install.md`](05-post-install.md)。

## kernel の出所

`bsd` / `bsd.rd` 入手元:

- jcs/openbsd-src rk3128 branch ([`PROVENANCE.md`](PROVENANCE.md) の URL)
- カスタム patch は [`../kernel-patches/`](../kernel-patches/)
- prebuilt artifacts は [`../prebuilt-info/`](../prebuilt-info/) の
  `bsd.armv7.*` 系 (`bsd.armv7.delay-2s` 等)

## 失敗したとき

- インストーラが SD カードを認識しない → [`02-make-sd.md`](02-make-sd.md)
  §2.6 の OpenBSD パーティション (`sd1a`) のマウント確認
- `bsd` 上書きを忘れて再起動 → 上流 kernel で立ち上がり BT/Wi-Fi 不動
  → 再度 bsd.rd で boot → shell → §4.3 をやり直す
- 完全に詰んだ → [`07-recovery.md`](07-recovery.md) で工場 Linux に戻して
  最初からやり直し
