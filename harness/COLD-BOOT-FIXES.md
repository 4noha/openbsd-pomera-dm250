<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# cold-boot で thin-client を SD 非依存に立ち上げる

DM250 を電源オンから自動で「Tailscale + BT-tether(panctl) が立つ thin-client」
にする際に踏んだ cold-boot バグ 4 件と、 その恒久修正の記録。 再現・別機展開用。

## 背景となる storage 構成

- eMMC=sd1: `/`(1G) `/usr`(2.9G, 狭い) `/home`(1.9G)。 SD=sd0a: `/mnt/sd`(28.7G)。
- `/usr` が狭いので **`/usr/local` は `/mnt/sd/usr_local`(SD) への symlink**。
- SD は fstab で **`noauto`**、 `/etc/rc.local` が
  `fsck -p /dev/sd0a; mount /mnt/sd`。
- `/etc/rc` の順序は **pkg_scripts → rc.local**。 つまり pkg_scripts 起動時点で
  SD は未マウント = `/usr/local/*` のバイナリは見えない。

## 症状

電源オンから始めると `tailscaled` / `panctl` が上がらず「外で繋がらない」。
`/var/log/daemon` に `sh: /usr/local/bin/tailscaled: not found`、
`panctl[..]: divert_create failed: Device busy` が出る。

## 根本原因と恒久修正

### A. boot critical バイナリが noauto SD 上にある → not found

pkg_scripts(`tailscaled` / `panctl` / `netwatchd`) が SD マウント前に起動 →
バイナリ not found。

**修正**: boot critical な 4 つを eMMC にコピーし rc.d を向け替え。

```sh
# on pomera (installed)
doas mkdir -p /usr/local.boot
doas cp -p /mnt/sd/usr_local/bin/tailscaled    /usr/local.boot/
doas cp -p /mnt/sd/usr_local/bin/tailscale     /usr/local.boot/
doas cp -p /mnt/sd/usr_local/sbin/panctl       /usr/local.boot/
doas cp -p /mnt/sd/usr_local/sbin/netwatchd.sh /usr/local.boot/
# rc.d の daemon= を /usr/local.boot/ に変更
# (panctl/netwatchd は repo template 反映済み、 tailscaled は pkg 提供なので
# sed で書き換え)
```

- repo: `panctl/etc/rc.d/panctl`、 `harness/netwatchd.rc.d`。
- bulk(fonts 1.1G・ libexec/git 640M・ mozc 辞書) は SD のまま。 eMMC 増分 ~36M。
- ※ 当初の「/usr/local 丸ごと eMMC へ pax relocate(~550M)」は失敗・破棄
  (find 除外が効かず・ ssh 断で孤児化・ 1GB RAM で過負荷)。 小さく eMMC コピーが正解。

### B. cold-boot で tailscaled が tun0 を奪取 → panctl が Device busy

tun の番号は浮動。 cold-boot で `tailscaled` が `panctl` より先に `/dev/tun0`
を開くと、 panctl(=tun0 専用) が `divert_create: Device busy` で起動不能になり、
`netwatchd` の再起動と相まって storm 化する。

**修正**: `tailscaled` を `tun1` に恒久固定。

```sh
# on pomera (installed)
doas rcctl set tailscaled flags "--tun tun1"
doas rcctl restart tailscaled
# 以後 panctl=tun0 / tailscaled=tun1 で固定。 fstat | grep tun で確認。
```

### C. netwatchd が panctl を見失い重複起動

`netwatchd.sh` の `is_panctl_running()` が旧パス
`pgrep -f /usr/local/sbin/panctl` を見ていて、 eMMC パスの `panctl` を検知
できず重複起動 → B を悪化。

**修正**: パス非依存に。 `pgrep -x panctl` (repo `harness/netwatchd.sh` 反映済み)。

### D. ld.so hints から /usr/local/lib が漏れる → mozc/fep/mlterm が lib load 不可

`/usr/local` が SD symlink のため、 boot 時 `ldconfig` が SD 未マウントで
走ると `/usr/local/lib` を hints から落とす。 `/usr/local/lib` 依存のもの
(`mozc_server` / `fep` / `mlterm` 等) が "shared lib not found" で起動不能。
`tailscaled` / `panctl` は `/usr/lib` のみ依存なので無事 → 長く気づかれな
かった (`mlterm-fb` も `/usr/lib` 中心)。

**修正**: `/etc/rc.local` の SD マウント後に `ldconfig -m /usr/local/lib` を
追加 (repo `harness/rc.local` 反映済み)。 boot 毎に SD マウント後 hints を
再マージ。

## 関連する他の boot 修正

- **WiFi → BT default フェイルオーバー**: `/etc/hostname.tun0` 末尾に
  `!route -qn add -priority 48 default 10.66.66.1` (repo
  `install/hostname.tun0.sample`)。 WiFi(prio12) があれば WiFi、 無ければ
  tun0(BT) が実効 default に自動降格。

## 別機 / 再ビルド時の展開順

1. `/usr/local` を SD symlink にしている前提を確認。
2. 上記 A の eMMC コピー + rc.d 向け替え。
3. B の `tailscaled --tun tun1` 固定。
4. C の `netwatchd.sh` は repo 版 (`pgrep -x panctl`) を配備。
5. `hostname.tun0` に prio48 failover。
6. D の `/etc/rc.local` に SD マウント後 `ldconfig -m /usr/local/lib`
   (repo 版を配備)。
7. `doas reboot` (`reboot -n` は disk flush race で使わない)
   → `fstat | grep tun` で `panctl=tun0` / `tailscaled=tun1`、
   `grep "not found\|Device busy" /var/log/daemon` がこの boot に無いことを確認。

## 残課題

- **真の「WiFi 完全圏外」cold-boot** の実機実証 (外に持ち出す物理テスト)。
  起動順上、 netstart で tun0 prio48 default が即入り、 WiFi default
  (`dhcpleased`) が無い環境では `tailscaled` bootstrap が `panctl` / BT 確立
  まで待たされる。 self-heal するはずだが未実測。
- `netwatchd.sh` 内の bare `tailscale` 呼びへの `/usr/local.boot` PATH 前置
  (SD 不在時の degraded 解消、 任意)。
