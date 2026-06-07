<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# prebuilt-info / MANIFEST

`pomera-workspace-meta` Release tag **`armv7-artifacts-v1`** に置いてある
binary artifact の一覧。 リポにはバイナリ本体を置かない（OpenBSD ISC 改変物・
Broadcom 配布 firmware・サイズの 3 点を考慮しての判断）ので、 取得は
`gh release download` 経由になる（詳細 [`README.md`](README.md)）。

| Name | Bytes | SHA256 | Source patches | License |
| --- | ---: | --- | --- | --- |
| `bsd.armv7.delay-2s` | 7.4 MB | `1aa585ece250c1eb0269f2c974dc379efe40a4acd58fab53060473572fa3b0a4` | [`../kernel-patches/bcmbt-delay-2s.patch`](../kernel-patches/bcmbt-delay-2s.patch) を `jcs/openbsd-src` (rk3128) に適用してクロスビルド | ISC (OpenBSD-derived) |
| `bsd.armv7.jcs-original` | 7.4 MB | `dd5c93947e98ba6f795bcfe9f210cbe6d6981c7de1c51a5431dc09c3358d0a0a` | パッチ未適用。 `https://jcs.org/dm250/bsd` (jcs.org 配布版) と byte 完全一致 | ISC (OpenBSD-derived) |
| `BCM4343A1.hcd.armbian-ap6212` | 33,376 B | `d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0` | armbian-firmware の `ap6212/bcm43438a1.hcd` をリネームしただけ（バイナリ無改変） | Broadcom proprietary firmware (armbian 再配布) |

## 用途と組み合わせ

| Artifact | 役割 | 必須? |
| --- | --- | --- |
| `bsd.armv7.delay-2s` | BT bring-up (bcmbt0) 用カーネル。 `bcmbt_fdt.c` の post-firmware HCI Reset 前 delay を 250ms→2s に拡張 | BT 使うなら必須 |
| `bsd.armv7.jcs-original` | パッチ前 jcs オリジナル。 ロールバック / 比較用 | optional |
| `BCM4343A1.hcd.armbian-ap6212` | `/etc/firmware/BCM4343A1.hcd` に配置する HCI patchram。 kernel が `loadfirmware(9)` で読みに来る | BT 使うなら必須 |

> [!IMPORTANT]
> `bsd.armv7.delay-2s` は **BT (`bcmbt-delay-2s`) のパッチ 1 本のみ**を当てた
> ビルドではなく、 同時期に試した WiFi-resume 系（`dwmmc-resume-pwrseq` +
> `bwfm-sdio-resume-guard`）が乗っている版を指す運用名。 WiFi suspend/resume
> は根治していない（[`../kernel-patches/README.md`](../kernel-patches/README.md)
> の CAUTION 参照）。 BT だけが本リリースで「必須・安定」の status。

## ファイル名の罠（重要）

`BCM4343A1.hcd.armbian-ap6212` は **そのまま `/etc/firmware/` に置いてはダメ**。
カーネル (`bcmbt_fdt.c`) は **`BCM4343A1.hcd`** という名前で `loadfirmware(9)`
を呼ぶので、 配置時にリネーム必須:

```sh
# on pomera (installed)
doas install -m 0644 /tmp/BCM4343A1.hcd.armbian-ap6212 /etc/firmware/BCM4343A1.hcd
```

歴史的経緯: 配布元 (RPi-Distro / armbian) は チップ型番ベースの `bcm43438a1.hcd`
を使うが、 OpenBSD カーネル側は patchram 系列名ベースの `BCM4343A1.hcd`
（"0" が抜ける）を hardcode している。 バイナリ自体は同一系統。

## License

- `bsd.armv7.*` は OpenBSD ソースをビルドしたもの。 ISC 継承（`../LICENSE-ISC`）。
- `BCM4343A1.hcd.armbian-ap6212` は Broadcom proprietary firmware の armbian
  再配布。 改変はしていない（リネームのみ）。 配布条件は armbian-firmware の
  `LICENCE.broadcom_bcm43xx` に従う。

## 取得手順 / SHA 検証

→ [`README.md`](README.md)

## ソースから自分でビルドし直したい

→ [`BUILD-FROM-SOURCE.md`](BUILD-FROM-SOURCE.md)
