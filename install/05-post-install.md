<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 05. ポストインストール (Wi-Fi / BT firmware / LED / dotfiles)

[`04-openbsd-install.md`](04-openbsd-install.md) でインストール後、初回
ログインから始める基本セットアップ。

シンクライアント本体 (panctl + Tailscale + netwatchd) は次章
[`06-thinclient.md`](06-thinclient.md) を参照。

## 5.1 bwfm の nvram を配置 (初回起動後すぐ)

最初の起動で `bwfm0: loadfirmware ... nvram_ap6212a.txt` というエラーが出る。
SD の EFI パーティションに `_sdboot.sh` が退避した firmware ディレクトリが
残っているので、そこから持ってくる:

```sh
# on pomera (installed)
doas mount /dev/sd0i /mnt
doas cp /mnt/firmware/nvram_ap6212a.txt \
  /etc/firmware/brcmfmac43430-sdio.rockchip,pomera-dm250.txt
doas umount /mnt
```

リブート後、Wi-Fi/Bluetooth が認識されるはず (`ifconfig bwfm0` で確認)。

## 5.2 BT patchram firmware を配置

> [!IMPORTANT]
> **動作条件**: BT firmware は **armbian の `ap6212/bcm43438a1.hcd`** を使う。
> RPi-Distro の `BCM43430A1.hcd` はチップ silicon revision との微妙な
> 不一致で patchram 完走しない。
> さらに **`bcmbt_fdt.c` の `delay(250000)` を 2s に拡張する kernel patch も
> 必要** ([`../kernel-patches/`](../kernel-patches/) 配下の bcmbt delay patch)。

DM250 のカーネル (`bcmbt_fdt.c` / jcs/openbsd-src rk3128) は **boot 時に**
`/etc/firmware/BCM4343A1.hcd` を読みに来て、AP6212A の HCI patchram を
カーネル内で投入する。これが無いと `bcmbt0` の attach が失敗し、BT が一切
使えない (`dmesg` に `bcmbt0: failed to load firmware BCM4343A1.hcd (error 2)`
が出る)。

入手元と SHA256 は [`PROVENANCE.md`](PROVENANCE.md)。OpenBSD カーネルは
ファイル名 `BCM4343A1.hcd` を期待しているので、ダウンロード後リネームして
配置する。

```sh
# on pomera (installed)
EXPECTED_SHA256=d396912aa4efa7e0ea93dc6b63b1088619b59676ab53404d14fe79f5c71a5da0

ftp -V -o /tmp/bcm43438a1.hcd \
    https://raw.githubusercontent.com/armbian/firmware/master/ap6212/bcm43438a1.hcd

ACTUAL=$(sha256 -q /tmp/bcm43438a1.hcd)
if [ "$ACTUAL" != "$EXPECTED_SHA256" ]; then
    echo "SHA mismatch! expected=$EXPECTED_SHA256 actual=$ACTUAL" >&2
    exit 1
fi

doas mkdir -p /etc/firmware
doas install -m 0644 /tmp/bcm43438a1.hcd /etc/firmware/BCM4343A1.hcd
rm /tmp/bcm43438a1.hcd
```

> [!NOTE]
> ファイル名のリネーム理由はカーネルソース側の hardcoded 名
> (`bcmbt_fdt.c` の `snprintf(fwname, sizeof(fwname), "BCM4343A1.hcd")`) に
> 合わせるため。armbian の `bcm43438a1.hcd` の中身がそのまま DM250 用
> patchram として使える (リネームだけで OK)。
>
> armbian から直接取れない時は **GitHub Releases** から取れる
> ([`../prebuilt-info/`](../prebuilt-info/) の `BCM4343A1.hcd.armbian-ap6212`)。

リブートして:

```sh
# on pomera (installed)
dmesg | grep bcmbt
# 期待: bcmbt0: address xx:xx:xx:xx:xx:xx  ← BD_ADDR が出れば成功
# 失敗時: bcmbt0: post-firmware HCI reset failed → kernel patch 当たってない可能性
```

**kernel patch も必須**: `bcmbt_fdt.c` の `delay(250000)` → `delay(2000000)`
拡張。詳細は [`../kernel-patches/`](../kernel-patches/) 配下の README、
prebuilt artifacts は [`../prebuilt-info/`](../prebuilt-info/) の
`bsd.armv7.delay-2s`。

attach 後、BT chip は **`/dev/cua00` (= uart0 の userland 側) で 115200 8N1
の HCI ready 状態**で座っている。ただし数秒の idle で chip が auto-sleep
に入るため、単純な userland HCI コマンドだと無応答に見える。これは
[`../panctl/`](../panctl/) の BCM 用 wake シーケンスで透過的に解消する。

### 既知不具合: BT firmware 配置で Wi-Fi が殺される

実機検証で確認された挙動:

| firmware 状態 | bcmbt 挙動 | bwfm 信号 / 接続 |
|---|---|---|
| `/etc/firmware/BCM4343A1.hcd` 配置 | `bcmbt0 at com0` → patchram 投入 → `bcmbt0: post-firmware HCI reset failed` で chip stuck | -73dBm、`status: no network`、association 不能 |
| firmware 避け (`.bak` 等にリネーム) | `bcmbt0: failed to load firmware BCM4343A1.hcd (error 2)` で kernel が早期 return、chip は Download Minidriver 直前で停止 | -65dBm、`status: active`、安定接続 |

原因: AP6212A は **Wi-Fi (BCM43430) と BT (BCM43438) が同一シリコンダイ**で
共有 RF front-end + coexistence pins (BT_ACTIVE/WL_ACTIVE) で TDM 切り替え。
`bcmbt_fdt.c` の post-firmware HCI Reset 失敗で chip が中途半端な状態で
止まると coex pin が「BT TX 中」固定になり Wi-Fi の RF 時間を奪う。

**当面の運用方針**: `/etc/firmware/BCM4343A1.hcd` は **配置しない**。BT を
触る段階 ([`06-thinclient.md`](06-thinclient.md) の panctl 着手時) になっ
たら、先に [`../kernel-patches/`](../kernel-patches/) の delay 拡張 kernel
に差し替えてから firmware を戻す。Wi-Fi だけで運用するなら本ステップ全体を
スキップしてよい (ssh / tmux / Tailscale には影響なし)。

## 5.3 起動ロゴ (任意)

U-Boot が EFI パーティションの `/logo.bmp` を起動時に表示する。
[`02-make-sd.md`](02-make-sd.md) §2.5 で puffy ロゴをコピー済みなら何も
しなくてよい。自前画像に差し替えるなら同名で置き換える:

```sh
# on pomera (installed)
doas mount /dev/sd1i /mnt   # 補足: DM250 内蔵 eMMC は sd0、SD は sd1
doas cp /path/to/new-logo.bmp /mnt/logo.bmp
doas umount /mnt
```

> [!NOTE]
> インストール後の SD/eMMC の番号は **インストーラ時とは逆になる**ことが
> ある。必ず `dmesg | grep -E '^(sd|umass)'` で実機の番号を確認してから
> マウントすること。

工場ロゴ抽出 + 合成スクリプトは [`../logo/`](../logo/) 参照。

## 5.4 LED を制御可能にする

DM250 / DM250X / DM250XY は USB-C ポート脇に赤・緑の LED が 2 個ある。
`gpioctl` で制御するために `/etc/rc.securelevel` でピンを open しておく
(`kern.securelevel` が上がる前にやる必要がある):

```sh
# on pomera (installed)
doas tee /etc/rc.securelevel <<'EOF'
#!/bin/sh
gpioctl -q gpio1 8  set out red_led
gpioctl -q gpio1 12 set out green_led
EOF
doas chmod +x /etc/rc.securelevel
```

リブート後:

```sh
# on pomera (installed)
gpioctl gpio1 red_led 1     # 赤 ON
gpioctl gpio1 red_led 0     # 赤 OFF
```

## 5.5 DNS 永続化 (resolvd 無効化)

`tailscaled` を入れる前段で、DNS が `resolvd` に書き換えられて Tailscale の
magic DNS が効かなくなる事故を防ぐ。

```sh
# on pomera (installed)
doas rcctl disable resolvd
doas rcctl stop    resolvd

# dhclient 復活時にも 100.100.100.100 / 1.1.1.1 を頭に置く
echo "prepend domain-name-servers 100.100.100.100, 1.1.1.1;" | doas tee /etc/dhclient.conf

# 静的 resolv.conf
doas tee /etc/resolv.conf <<'EOF'
nameserver 100.100.100.100
nameserver 1.1.1.1
EOF
```

## 5.6 sysctl: panic 時 auto reboot

`ddb.panic=1` (default) だと panic 時に kernel debugger に落ちて画面が無
反応のまま hang する。物理 reboot 不要にするため:

```sh
# on pomera (installed)
doas sysctl ddb.panic=0
echo "ddb.panic=0  # panic 時は debugger でなく reboot" | doas tee -a /etc/sysctl.conf
```

## 5.7 dotfiles (ユーザー側)

```sh
# on pomera (installed) — <your-pomera-user> として
mkdir -p ~/.ssh && chmod 700 ~/.ssh

# ~/.kshrc は battery PS1 (電池/ネット indicator + HISTFILE 永続化込み)
# サブツリー ../battery/ から取得
scp 4noha@<jumphost>:.../battery/kshrc/v3.1-cal-R120-net-tun0-histfile.kshrc ~/.kshrc
# 詳細は ../battery/ の README

# SSH key 生成 (再 install では新 key、Mac 側の authorized_keys に追加要)
ssh-keygen -t ed25519 -C "<your-pomera-user>@<pomera-host>" -f ~/.ssh/id_ed25519 -N "<passphrase>"
cat ~/.ssh/id_ed25519.pub
# → 上記公開鍵を Mac 側で:
#    cat >> /Users/$USER/.ssh/authorized_keys   # Mac 側のユーザで

# ~/.ssh/config に Tailscale 越え用 alias
cat > ~/.ssh/config <<'EOF'
Host <home-mac>
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m

Host claude
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m
EOF
chmod 600 ~/.ssh/config
```

## 5.8 wsconsctl.conf (キー配列)

```sh
# on pomera (installed)
doas tee /etc/wsconsctl.conf <<'EOF'
keyboard.encoding=jp
keyboard.map+=keycode 13 = asciicircum asciitilde
keyboard.map+=keycode 58 = Control_L
keyboard.map+=keycode 139 = Mode_switch
keyboard.map+=keycode 103 = Up Up Prior Prior
keyboard.map+=keycode 108 = Down Down Next Next
EOF
# 反映は再 login or reboot で
```

設定内容:
- `keycode 13` を `^/~` に (JP pomera 配列の特殊キー対応)
- `keycode 58` = Caps Lock を Control_L 化
- `keycode 139` = Menu キーを Mode_switch (修飾子化)
- `keycode 103/108` = ↑/↓ の Mode_switch 押下時 variant を Prior/Next →
  Menu+↑ で Page Up、Menu+↓ で Page Down

## 5.9 コンソールを wsvt25 (8 色対応) にする

OpenBSD インストール直後の `/etc/ttys` は ttyC* が `vt220` で起動する
設定だが、vt220 はモノクロ仕様なので ls/vi/less などが全部白黒になる。
pomera の wsdisplay は vt100 emulation + 8 色 hardware なので、`wsvt25` に
変えれば物理コンソールでカラー表示できる:

```sh
# on pomera (installed)
doas sed -i.bak 's|\(^console.*\)vt220|\1wsvt25|' /etc/ttys
doas sed -i     's|\(^ttyC[1-5].*\)vt220|\1wsvt25|' /etc/ttys
doas kill -HUP 1                # init 再読込
# 物理 console で一度ログアウト → 再ログインで反映 (現セッションは変わらない)
# 確認:
#   echo $TERM        → wsvt25
#   ls -G             → 色付き
#   infocmp $TERM | grep colors  → colors#8
```

`console` 行 (= 物理 LCD への primary getty) が一番重要。これを忘れると
pomera 本体でログインしたとき TERM=vt220 のまま。`ttyC1-C5` は
Ctrl-Alt-F1..F5 で切替する追加 vt。`ttyC0` は serial console 想定で
`vt220` 維持。ssh 経由ログインの TERM は client 側 (Mac iTerm 等) が決める
ので本変更は無関係。

## 5.10 SD カードに `/usr/local` を逃がす (任意、mlterm 入れたい場合)

OpenBSD の `wsdisplay` は ASCII/Latin-1 glyph しか持たないため、ssh 先で
日本語が出ても pomera console では化ける。framebuffer 端末 `mlterm-fb`
(mlterm package 同梱) を入れれば独自描画で日本語表示可能だが、依存
`gtk+2` が ~150MB あって DM250 の 2.9GB `/usr` (`/dev/sd1d`) に入りきらない。

→ **SD カード (`/dev/sd0a`, FFS) に `/usr/local` を逃がす**ことで解決。
インストール用 SD カードがインストール後はそのまま空き 28GB 残っているので
それを再利用する。

```sh
# on pomera (installed)
# サービス停止
doas rcctl stop panctl tailscaled

# SD card マウント (sd0a は既に FFS でフォーマット済 = installer 残り)
doas mkdir -p /mnt/sd
doas mount /dev/sd0a /mnt/sd

# /usr/local を SD card にコピー → 元を消して symlink
doas cp -a /usr/local/. /mnt/sd/usr_local/
doas rm -rf /usr/local
doas ln -s /mnt/sd/usr_local /usr/local

# サービス再開
doas rcctl start panctl tailscaled

# 起動時自動マウント (DUID を fstab に登録)
doas disklabel sd0 | awk '/^duid:/ {print $2}'   # → xxxxxxxxxxxxxxxx
echo 'xxxxxxxxxxxxxxxx.a /mnt/sd ffs rw,nodev,nosuid 1 2' | doas tee -a /etc/fstab
```

(`xxxxxxxxxxxxxxxx` は `disklabel sd0` で表示された `duid:` の値に置換)

これで `/usr` の使用率は変わらず、`/mnt/sd` に 26GB 以上空く。

その上で mlterm 本体 + 日本語フォントを入れる:

```sh
# on pomera (installed)
doas pkg_add mlterm           # gtk+2 と一緒に入る (~150MB, SD card に逃げる)
doas pkg_add ja-sazanami-ttf  # CJK TTF (~10MB)
```

mlterm-fb の DM250 表示修正版 (emoji/Nerd Font/resize ハング解決) は
[`../prebuilt-info/`](../prebuilt-info/) の `mlterm-fb.armv7` 参照。

## 5.11 動作確認

```sh
# on pomera (installed)
dmesg | grep -E 'bwfm0|bcmbt0'   # → MAC アドレスが見える
ifconfig bwfm0                    # → status: active, inet ...
gpioctl gpio1 red_led 1; sleep 1; gpioctl gpio1 red_led 0   # LED 点灯
```

次は [`06-thinclient.md`](06-thinclient.md) で panctl + Tailscale を入れる。

## suspend/resume の注意

> [!CAUTION]
> **WiFi(bwfm0) が up のまま蓋を閉じて suspend → resume するとカーネルが
> クラッシュする** (bwfm/dwmmc の resume バグ、未根治・凍結)。復帰の
> 反復で eMMC `/` が破損し single-user fsck 復旧が要る事故が起きる
> ([`07-recovery.md`](07-recovery.md) §「FFS 不整合」参照)。
>
> 安全な運用:
> - **BT-tether 運用 (WiFi off) なら蓋スリープは安全** ← 本来の thin-client 形態
> - WiFi をデバッグ等で活かす日は **蓋を閉じない**、もしくは閉じる前に
>   `ifconfig bwfm0 down`、もしくは `sysctl machdep.lidaction=0`
> - カーネル追い込みの経緯・パッチは [`../kernel-patches/`](../kernel-patches/)、
>   resume harness は [`../harness/`](../harness/)
