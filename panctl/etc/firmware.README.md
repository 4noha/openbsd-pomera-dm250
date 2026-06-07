# firmware.README — BT patchram (`BCM4343A1.hcd`) の入手手順

[DESIGN.md](../DESIGN.md) Phase B Step 3。AP6212A (= BCM43430A1) の HCI patchram。**バイナリ自体は本リポジトリには置かない**。ビルド時に取得して `/etc/firmware/BCM4343A1.hcd` に配置する。

> [!IMPORTANT]
> 配布元のファイル名は `BCM43430A1.hcd`（チップ型番ベース）、 OpenBSD
> カーネル (`bcmbt_fdt.c` / jcs/openbsd-src rk3128) は `BCM4343A1.hcd`
> （patchram 系列名、 "0" が抜ける）を `loadfirmware(9)` で期待する。 **取得後にリネーム**して `/etc/firmware/BCM4343A1.hcd` として配置する。 バイナリ自体は同一。 jcs 側 `bcmbt_fdt.c` には `/* TODO: per-chipset firmware files */` のコメントあり、 将来 chip ID 自動判定になったら本ファイルも更新する。

## 採用元

**Raspberry Pi `RPi-Distro/bluez-firmware` の `master` ブランチ**。

| 項目 | 値 |
|---|---|
| リポジトリ | `https://github.com/RPi-Distro/bluez-firmware` |
| ブランチ | `master` (default の `pios/trixie` には**入っていない**ので注意) |
| ファイル | `broadcom/BCM43430A1.hcd` |
| サイズ | 30,049 bytes |
| **SHA256** | `c096ad4a5c3f06ed7d69eba246bf89ada9acba64a5b6f51b1e9c12f99bb1e1a7` |
| 最終更新 | 2020-08-05 (commit `afe608e7` "Update to BCM43438 firmware with Spectra fixes") |

linux-firmware (kernel.org / `git.kernel.org/.../linux-firmware`) の `brcm/` には**該当 `.hcd` は存在しない** (BT patchram は再配布条件の都合で取り込まれていない)。`torvalds/linux-firmware` という GitHub mirror も存在しない。したがって **RPi 版一択**。

## 取得手順

OpenBSD 実機 (DM250) で。 boot 時に `bcmbt0` の attach が走るので、 **これを
やらないと BT が一切上がらない**（`dmesg` に
`bcmbt0: failed to load firmware BCM4343A1.hcd (error 2)` が出る）:

```sh
# on pomera (installed)
EXPECTED_SHA256=c096ad4a5c3f06ed7d69eba246bf89ada9acba64a5b6f51b1e9c12f99bb1e1a7

ftp -V -o /tmp/BCM43430A1.hcd \
    https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/master/broadcom/BCM43430A1.hcd

ACTUAL=$(sha256 -q /tmp/BCM43430A1.hcd)
if [ "$ACTUAL" != "$EXPECTED_SHA256" ]; then
    echo "SHA mismatch! expected=$EXPECTED_SHA256 actual=$ACTUAL" >&2
    exit 1
fi

doas mkdir -p /etc/firmware
doas install -m 0644 /tmp/BCM43430A1.hcd /etc/firmware/BCM4343A1.hcd
rm /tmp/BCM43430A1.hcd

# 反映確認: リブート後
doas reboot
# ↑ 再ログイン後
dmesg | grep bcmbt    # "bcmbt0 at com0: address xx:xx:..." が出れば OK
```

Phase A の amd64 OpenBSD VM 上で BTstack `chipset/bcm` を動かす場合（USB HCI ドングルではなく UART チップを試す稀ケース）は配置先パスを使う側が変えてよい — `bcmbt_fdt.c` のリネーム制約は DM250 のカーネルだけの話。 通常 Phase A の USB ドングルパスでは patchram は使わない。

## ライセンス上の留意

- RPi 版に明示的な `LICENCE.broadcom` ファイルは無いが、実態は Cypress/Broadcom の `LICENCE.broadcom-bcm43xx` 系条文に準拠して配布されている (「再配布可、改変不可、リバースエンジニアリング不可」)
- 本ワークスペースが個人用途を超えて配布物に含める場合は別途確認が要る (PLANS.md「やらないこと」)
- **本リポにはコミットしない**。ビルド成果物にも組み込まない。runtime に取得して `/etc/firmware/` に置く

## SHA ドリフトしたら

将来 RPi 側で firmware が更新されると SHA が変わる。その時は:

1. リポ側でこの値を更新する前に、`bluez-firmware` の commit 履歴を読んで変更内容を確認 (security fix なら通す、機能変更なら panctl で peer-tested してから)
2. SHA 値だけ更新するコミットは PR 分離 (理由を commit message に明記)

## panctl からの参照

**DM250 では patchram はカーネル (`bcmbt_fdt.c`) が boot 時に済ませる**ので、
panctl 側は patchram 投入をしない。 `panctl-h4` の起動コマンドは:

```sh
# on pomera (installed)
panctl-h4 -d /dev/cua00
```

`-f` (firmware path) 引数は **DM250 では不要**。 BTstack の chipset 初期化も
**`chipset/none`** で OK（`btstack_chipset_bcm_*` 系 API は呼ばない）。 panctl
は cua00 を 115200 8N1 + CRTSCTS で open(2) して、 そのまま H4 frame 入出力に
入る。

Phase A の amd64 + USB HCI ドングルでは `panctl-libusb` を使うので、 そもそも
patchram は不要（OpenBSD VM 上の標準ドングルは出荷時に内部 ROM の firmware で
動く）。 一部 ドングル (BCM 系 USB) で patchram が要る場合のみ
`btstack_chipset_bcm_set_hcd_folder_path(...)` で `.hcd` を指定する。 詳細は
[02-btstack-port-notes.md §2](../02-btstack-port-notes.md)。
