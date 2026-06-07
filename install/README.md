<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# install/ — DM250 OpenBSD + BT-tether + Tailscale thin-client install

Pomera DM250 (DM250 / DM250X / DM250XY、日本向け) に OpenBSD 7.9 を入れて、
**自宅 PC の `claude-master` tmux を Tailscale 越しに ssh で表示する専用
シンクライアント**に仕立てる一連の手順。

> [!IMPORTANT]
> DM250US は U-Boot と LED 制御が違うため対象外。
> jcs.org のカスタム U-Boot を eMMC に書き込んだ瞬間、工場 Linux は起動できなくなる。
> 戻すには [`07-recovery.md`](07-recovery.md) の手順で MaskROM 経由の復元が必要。
> 事前に [`01-backup-emmc.md`](01-backup-emmc.md) を必ず実行すること。

## 章立て

| 章 | 内容 | 主な実行場所 |
|---|---|---|
| [01-backup-emmc.md](01-backup-emmc.md) | eMMC 全体を SD カードへ吸い出す | on pomera (recovery) → on mac |
| [02-make-sd.md](02-make-sd.md) | OpenBSD インストール用 SD カード作成 | on mac (qemu) または on openbsd-host |
| [03-recovery-mode.md](03-recovery-mode.md) | リカバリモードで U-Boot を差し替え | on pomera (recovery) |
| [04-openbsd-install.md](04-openbsd-install.md) | bsd.rd 起動 → インストーラ対話 → カスタムカーネル上書き | on pomera (installer) |
| [05-post-install.md](05-post-install.md) | bwfm/BT firmware、LED、wsconsctl、コンソール、dotfiles | on pomera (installed) |
| [06-thinclient.md](06-thinclient.md) | panctl (BT PAN) + Tailscale + netwatchd | on pomera (installed) |
| [07-recovery.md](07-recovery.md) | 工場 Linux 復元・single-user fsck・MaskROM | on mac + on pomera |
| [PROVENANCE.md](PROVENANCE.md) | 一次資料・配布物の URL と SHA256 一覧 | — |

## 全体フロー

```
[01] eMMC backup → [02] SD prep → [03] U-Boot 差し替え → [04] OpenBSD install
                                                              ↓
                                                          [05] post-install
                                                              ↓
                                                          [06] thin-client (panctl + Tailscale + netwatchd)
                                                              ↓
                                                          (任意) battery PS1、起動ロゴ、kernel patch 群
```

## ラベル

各章のコードブロックには **実行場所** を冒頭コメントで明示している:

- `# on mac` — このリポを clone した Mac
- `# on openbsd-vm (installer)` — [02 章](02-make-sd.md) の使い捨て qemu VM (`docs/build-vm.md` 参照)
- `# on openbsd-host` — 手動手順を別の OpenBSD 機で叩く場合
- `# on pomera (recovery)` — Right Shift + Left Alt + Power で起動した状態
- `# on pomera (installer)` — `bsd.rd` のインストーラ
- `# on pomera (installed)` — インストール後の DM250 上 OpenBSD

`# on pomera` 単体は使わない (recovery / installer / installed は全部別物)。

## 前提と用意するもの

- 対象機 **DM250 / DM250X / DM250XY** (バッテリーは満充電に近い状態で)
- microSD ではなく **フルサイズ SD カード**、FAT32 16GB 以上 (中身は消える)
- USB-C ケーブル (リカバリ時に Mac と DM250 をつなぐ)
- macOS + Apple Silicon 推奨 (build VM の HVF アクセラレーション用)
- `brew install qemu` 程度の基本ツール
- このリポを `~/works/openbsd-pomera-dm250-staging/` などに clone 済み

## クロス参照

このリポの他のサブツリーへの主なリンク:

- [`../kernel-patches/`](../kernel-patches/) — jcs/openbsd-src rk3128 patch 集 (BT delay 拡張、resume 関連)
- [`../panctl/`](../panctl/) — BT PAN-tether daemon (mux ベース)
- [`../harness/`](../harness/) — suspend/resume debug harness
- [`../logo/`](../logo/) — 起動ロゴ抽出 + 合成
- [`../battery/`](../battery/) — OCV 補正 PS1
- [`../tailscale-optional/`](../tailscale-optional/) — Tailscale thin-client 設定
- [`../docs/`](../docs/) — build VM の作り方、クロスビルド sysroot
- [`../prebuilt-info/`](../prebuilt-info/) — GitHub Releases artifacts の SHA256 一覧

## Prebuilt artifacts

カスタムカーネル・BT firmware・mozc-server などの自作ビルド物は GitHub Releases
に置かれている (asset 名と SHA256 は [`../prebuilt-info/`](../prebuilt-info/))。
各章で「ソースからビルドし直す」と書いてあるところは、代わりに Release から
取得して scp で運べば速い (kernel は VM 5 分/実機 73 分の再ビルドを省ける)。
