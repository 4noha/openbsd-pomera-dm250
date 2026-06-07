<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# prebuilt-info

DM250 用にビルド済みの **カーネル** と、 配置に必要な **BT patchram firmware**
の取得手順。 リポにはバイナリ本体を置かず、 GitHub Releases に分離している。

- 一覧表 (name, size, SHA256, source patches, license): [`MANIFEST.md`](MANIFEST.md)
- ソースから自分で再ビルドしたい: [`BUILD-FROM-SOURCE.md`](BUILD-FROM-SOURCE.md)

## 前提

- `gh` (GitHub CLI) が入っていて認証済み (`gh auth status` で確認)。
- macOS なら `shasum`、 OpenBSD/Linux なら `sha256` / `sha256sum` のどれかが
  使える。
- pomera (DM250) には Tailscale + ssh で入れる状態（DHCP/Wi-Fi/BT-tether は
  問わない）。 ホスト名を `<pomera-host>` とする。

## 1. Release から取得（mac 側）

```sh
# on mac
REPO=<your-org>/pomera-workspace-meta       # 公開後のリポ名に差し替え
REL=armv7-artifacts-v1

mkdir -p ~/dm250-artifacts && cd ~/dm250-artifacts

gh release download "$REL" -R "$REPO" -p 'bsd.armv7.delay-2s'
gh release download "$REL" -R "$REPO" -p 'bsd.armv7.jcs-original'      # rollback 用 (optional)
gh release download "$REL" -R "$REPO" -p 'BCM4343A1.hcd.armbian-ap6212'
```

## 2. SHA256 で integrity 検証（mac 側）

```sh
# on mac
shasum -a 256 bsd.armv7.delay-2s bsd.armv7.jcs-original BCM4343A1.hcd.armbian-ap6212
```

期待値（一致しなかったら **絶対に pomera に転送しない**）:

```
1aa585ece250c1eb0269f2c974dc379efe40a4acd58fab53060473572fa3b0a4  bsd.armv7.delay-2s
dd5c93947e98ba6f795bcfe9f210cbe6d6981c7de1c51a5431dc09c3358d0a0a  bsd.armv7.jcs-original
d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0  BCM4343A1.hcd.armbian-ap6212
```

ワンライナーで検証する場合:

```sh
# on mac
cat > /tmp/SHA256.dm250 <<'EOF'
1aa585ece250c1eb0269f2c974dc379efe40a4acd58fab53060473572fa3b0a4  bsd.armv7.delay-2s
dd5c93947e98ba6f795bcfe9f210cbe6d6981c7de1c51a5431dc09c3358d0a0a  bsd.armv7.jcs-original
d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0  BCM4343A1.hcd.armbian-ap6212
EOF
shasum -a 256 -c /tmp/SHA256.dm250
```

## 3. pomera に転送（mac → pomera）

```sh
# on mac
scp bsd.armv7.delay-2s BCM4343A1.hcd.armbian-ap6212 <your-pomera-user>@<pomera-host>:/tmp/
# optional (rollback 用):
scp bsd.armv7.jcs-original <your-pomera-user>@<pomera-host>:/tmp/
```

## 4. pomera 上で SHA256 再検証 → 配置

```sh
# on pomera (installed)
cd /tmp

cat > /tmp/SHA256.dm250 <<'EOF'
1aa585ece250c1eb0269f2c974dc379efe40a4acd58fab53060473572fa3b0a4  bsd.armv7.delay-2s
d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0  BCM4343A1.hcd.armbian-ap6212
EOF
sha256 -c /tmp/SHA256.dm250
```

### 4.1 BT patchram を `/etc/firmware/` に配置（**リネーム必須**）

```sh
# on pomera (installed)
doas install -m 0644 /tmp/BCM4343A1.hcd.armbian-ap6212 /etc/firmware/BCM4343A1.hcd
```

> `BCM4343A1.hcd.armbian-ap6212` → `BCM4343A1.hcd` のリネームは kernel
> (`bcmbt_fdt.c`) が hardcode した名前を `loadfirmware(9)` に渡すため。
> 詳細は [`MANIFEST.md`](MANIFEST.md) の「ファイル名の罠」セクション。

### 4.2 カーネル本体を `/bsd` に配置（hardlink を切る順序が大事）

```sh
# on pomera (installed)
# ★ 先に mv で /bsd と /bsd.booted の hardlink を切る (jcs オリジナルは
#   両者で同一 inode を共有しているので install で書き換えると rollback が壊れる)
doas mv /bsd /bsd.jcs-original
doas install -m 0700 -o root -g wheel /tmp/bsd.armv7.delay-2s /bsd
doas reboot       # ★ reboot -n は使わない (disk flush race)
```

起動後の確認:

```sh
# on pomera (installed)
sysctl kern.version | head -1               # build 日時で新旧を判別
dmesg | grep -E 'bcmbt|bwfm'                # bcmbt0: address ... が出ていれば BT bring-up 完走
```

## 5. インストール手順本体への接続

このカーネル＋ firmware の配置は、 install フロー全体の中の post-install
ステップ。 install フロー本体（SD 作成 → bsd.rd インストール → post-install）の
中の正しい位置については [`../install/04-…`](../install/) を参照。

## 6. ロールバック

「BT bring-up が完走しない / 何かが壊れた」ときは、 取っておいた jcs
オリジナルに戻す:

```sh
# on pomera (installed)
doas mv /bsd /bsd.delay-2s.broken
doas install -m 0700 -o root -g wheel /bsd.jcs-original /bsd
doas reboot
```

bootloader からの一時的なブート切り替えなら:

```text
# on pomera (bootloader)
boot> boot /bsd.jcs-original
```

## License

- カーネルバイナリは OpenBSD ソース由来で ISC 継承 (`../LICENSE-ISC`)。
- 本ドキュメント (`README.md` / `MANIFEST.md` / `BUILD-FROM-SOURCE.md`) は MIT
  (`../LICENSE`)。
- BT firmware は Broadcom proprietary、 armbian 経由の再配布。 改変なし。
