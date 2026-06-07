<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# harness/ — suspend/resume debug harness for DM250 OpenBSD

pomera DM250 (OpenBSD/armv7) は「蓋を閉じる(=`apm` suspend)」「蓋を開ける
(=resume)」の前後で WiFi(bwfm)・SD・LED・default route などが連鎖的に死ぬ
傾向があります。 ここに置いているのは、その挙動を **クラッシュや自動 reboot
を生き残る永続ログ** で切り分けるための小さな診断 harness 一式です。

ssh 切断・kernel panic・自動 reboot の **直前** に何が起きていたかを後追い
できるよう、各書込みごとに `sync(8)` で eMMC root fs に flush する設計です。

## 何が入っているか

| ファイル | 役割 |
|---|---|
| `apmsuspend.c` | `apm(8)` / `zzz(8)` 相当の suspend 発火。 base に無いので自作 |
| `wifi-suspend-test.sh` | suspend 前/後の状態を `/var/log/wifi-suspend-test.log` に追記。 boot fook で「直前に ARMED していたか」を判定して crash-reboot を見抜く |
| `netresume-test.sh` | nohup で常駐し、 蓋閉じ/開けでネット状態がどう壊れて戻るかを `/var/log/netresume-test.log` に時系列記録 |
| `mount-sd-local.sh` | SD を冪等にマウントし `/usr/local/lib` の ld.so hints を回復。 `rc.local` と hotplug の両方から呼ぶ |
| `hotplug-attach` / `hotplug-detach` | `/etc/hotplug/{attach,detach}`。 SD の挿抜で `mount-sd-local.sh` / `umount` を発火 |
| `rc.local` | boot 時 keymap 設定、 SD mount、 daemon (netwatchd / panctl / tailscaled) の遅延起動 |
| `rc.securelevel` | LED 制御用に `gpio1` の `red_led` / `green_led` ピンを open |
| `netwatchd.sh` / `netwatchd.rc.d` | resume 検知 + WiFi/BT 監視 + LED + PS1 cache 常駐。 `/etc/rc.d/netwatchd` から起動 |
| `mlterm-main` / `mlterm-aafont` | mlterm-fb (framebuffer 直叩き端末) の最小設定 |
| `COLD-BOOT-FIXES.md` | cold-boot で thin-client が立たない件の根本原因 4 件 (A〜D) と恒久修正の記録 |

## どう使うか — 典型シナリオ

### 1. resume で WiFi が死ぬのを切り分ける

```sh
# on pomera (installed) — 永続ログ常駐を起動
doas sh -c 'nohup sh /root/netresume-test.sh >/dev/null 2>&1 & echo PID=$!'

# → 蓋を閉じる → 30 秒待つ → 蓋を開ける → 1〜2 分待つ

# 結果を読む
doas cat /var/log/netresume-test.log
# RESUME 検知行 + ifconfig bwfm0 / dhcp / 1.1.1.1 ping / tailscale up の time-series

# 停止
doas touch /root/.netresume-stop
```

`gap` (時刻ジャンプ) が `GAP=20` 秒以上のサンプルが「resume 直後」のマーカに
なります。 そこから WiFi/tun0/default route がいつ戻ったかを 1 行ずつ追える。

### 2. 「WiFi up のまま suspend して resume で crash する」を実機確認する

`wifi-suspend-test.sh` は **既定で suspend を起こさない**。 `--go` を渡した
時だけ `apmsuspend --go` が走り実 suspend に入ります。

```sh
# on pomera (installed) — boot fook を登録 (前回 ARMED の残骸を判定)
# /etc/rc.local の末尾に:
#   doas sh /root/wifi-suspend-test.sh boot-hook

doas sh /root/wifi-suspend-test.sh dry          # 状態採取 + sync (suspend しない)
doas sh /root/wifi-suspend-test.sh --label v3 --go   # 本番 (実 suspend)
```

`--go` 経路は

1. PRE-SUSPEND 状態採取 + `sync`
2. `ARMED <armed_boottime>` を `/var/db/wifi-suspend-test.state` に書いて `sync`
3. `apmsuspend --go` で suspend
4. resume 後に T+0/3/10/30/60/180/300s で連続キャプチャ (bwfm 再 attach が非同期 kthread 経路で遅れて来るため)
5. clean resume なら `STATE` を解除

を行います。 途中で crash-reboot しても次回 boot で `boot-hook` が `STATE`
を見て `CRASH-REBOOT` 判定行を log に追記します。

### 3. 蓋復帰時の SD/WiFi 復活を常駐監視する

`netwatchd.sh` が `/usr/local.boot/netwatchd.sh` (eMMC = boot critical
location) に置かれ、 `rc.d/netwatchd` 経由で常駐します。 機能は:

- `sleep` が想定より長かった = resume と判定 → `RESUME_SETTLE=8s` 待ってから
  カーネルが自力で `bwfm0` を戻していなければ `wifi_up` し、 `panctl` を
  restart。
- 定期的に `bwfm0` link を polling、 落ちてたら `ifconfig down/up` + `dhclient`。
- `panctl` の生存監視 (パス非依存 `pgrep -x panctl`) と log scrape による
  BT 接続有無の `/tmp/.bt-state` 反映。
- LED (`gpio1` の red/green) を電源状態 (`hw.sensors.simplebat0`) で制御。
- PS1 cache: OCV 補正の battery SOC と WiFi/BT glyph を `/tmp/.prompt-{bat,net}`
  に書き、 `~/.kshrc` 側は `cat` だけで PS1 を組めるように。

## サンプル出力

`wifi-suspend-test.sh` (実機, 抜粋):

```
##### wifi-suspend-test RUN start 2026-06-05 21:43:11 mode=--go label=v3 #####
----- [PRE-SUSPEND] 2026-06-05 21:43:11 -----
  seq=1717634591  pid=14823
  uptime: ...up 12 mins, 1 user, load averages: 0.31, 0.25, 0.15
  boottime_epoch=1717633871  (...)
  ifconfig bwfm0:
    | bwfm0: flags=...UP,BROADCAST,RUNNING...
    | 	media: IEEE802.11 autoselect (HT-MCS7)
    | 	status: active
    | 	ieee80211: nwid <your-wifi-ssid> chan 11 ...
  dmesg tail:
    | bwfm0: firmware ...
ARMED 2026-06-05 21:43:11 (boottime_epoch=1717633871)  <- ここで suspend を仕掛けた印
GOING-TO-SUSPEND 2026-06-05 21:43:11 -> exec /usr/local.boot/apmsuspend --go
BACK-FROM-SUSPEND 2026-06-05 21:43:42
===== POST-RESUME CHECK (T+0s) 2026-06-05 21:43:42 =====
  VERDICT: boottime 不変 (1717633871) -> 箱は生存 = RESUME (no reboot)
  bwfm: dmesg に 'error 60' なし
  ifconfig bwfm0 (full):
    | bwfm0: flags=...
  /mnt/sd mounted : yes
  default route   : bwfm0
  netwatchd       : alive
  panctl          : alive
```

`netresume-test.sh` (実機, 抜粋):

```
===== netresume-test start 2026-06-05 21:40:01 (interval=6s max=3600s) =====
  boottime=...
21:40:07 T+6s wifi=active tun0=up tun1=up def=bwfm0 | gwping=ok dns=ok tcp1111=ok ts=up bt=up
21:40:13 T+12s wifi=active tun0=up tun1=up def=bwfm0 | gwping=ok dns=ok tcp1111=ok ts=up bt=up
  ===== RESUME 検知: 約 47s の時刻ジャンプ(=suspend)後に再開 @ 21:41:00 =====
21:41:00 T+59s wifi=DOWN  tun0=-  tun1=-  def=none   | gwping=NG dns=NG tcp1111=NG ts=DOWN bt=down
21:41:06 T+65s wifi=DOWN  tun0=-  tun1=-  def=none   | gwping=NG dns=NG tcp1111=NG ts=DOWN bt=down
21:41:12 T+71s wifi=active tun0=-  tun1=-  def=bwfm0 | gwping=ok dns=NG tcp1111=NG ts=DOWN bt=down
21:41:18 T+77s wifi=active tun0=up tun1=up def=bwfm0 | gwping=ok dns=ok tcp1111=ok ts=up bt=up
```

## 環境変数 / placeholder

別機・別環境に展開するときは以下を実値に置換 (もしくは export):

| placeholder | 意味 | 例 |
|---|---|---|
| `SD_DUID` (`hotplug-attach`) | `/mnt/sd` にしたい SD カードの 16 桁 hex DUID。 別 SD を差したら誤 mount しないよう ID で照合する | `SD_DUID=0123456789abcdef` |
| `/usr/local.boot` | boot critical バイナリの eMMC 配置先。 `tailscaled` / `panctl` / `netwatchd.sh` / `apmsuspend` をここに置く。 詳細は `COLD-BOOT-FIXES.md` § A | (固定) |
| `RESUME_SETTLE` (`netwatchd.sh`) | resume 検知から WiFi に触れるまで待つ秒数。 短すぎると bwfm resume と race して crash | `8` (既定) |
| `GAP` (`netresume-test.sh`) | 「これ以上の時刻ジャンプは resume」と見なす閾値 (秒) | `20` (既定) |

`SD_DUID` は **その機体固有** なので、 リポでは placeholder
(`<dm250-sd-duid>`、 環境変数で上書き可) を入れてあります。 実機に当てるときは
`disklabel sd0 | grep duid` で取得した 16 桁 hex 値で `hotplug-attach` の冒頭を
書き換えるか、 環境変数 `SD_DUID=...` で渡してください。

## 関連

- 起動順とどこに何を置くべきかは `COLD-BOOT-FIXES.md` (cold-boot で
  thin-client が立たない問題の A〜D 修正)。
- bwfm の resume 不具合のカーネル側修正は `../kernel-patches/`。
- BT-tether daemon 本体は `../panctl/`。
- Tailscale thin-client 周りは `../tailscale-optional/`。
- LED 計算式や OCV 補正の根拠は `../battery/`。
